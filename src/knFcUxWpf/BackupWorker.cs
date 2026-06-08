/*
 * BackupWorker.cs
 * Single C# reader task blocking on FilterGetMessage. On a
 * BackupRequest it copies the source (\\?\GLOBALROOT + NT path) into
 * the session mirror directory and replies with status.
 *
 * Disk-full handling: a single bool is flipped by the ViewModel's
 * disk monitor; the reader short-circuits to ERROR_DISK_FULL.
 *
 * Each successful backup invokes OnBackup(entry); MainViewModel
 * marshals it to the UI thread so the history grid updates without
 * polling.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace KnFc.Ux;

public sealed class BackupEvent
{
    public ulong  Id        { get; init; }
    public DateTime TsUtc   { get; init; }
    public ulong  Pid       { get; init; }
    public ulong  RootPid   { get; init; }
    public uint   Flags     { get; init; }
    public ulong  SizeHint  { get; init; }
    public string SrcNt     { get; init; } = "";
    public string Dest      { get; init; } = "";
    public int    Status    { get; init; }
}

public sealed class ProcessEventNotify
{
    public uint   EventType { get; init; }   /* 1=created, 2=exited */
    public ulong  Pid       { get; init; }
    public ulong  ParentPid { get; init; }
    public ulong  RootPid   { get; init; }
    public uint   Flags     { get; init; }
    public string Image     { get; init; } = "";
}

public sealed class BackupWorker : IDisposable
{
    private readonly SafeFileHandle _port;
    private readonly string         _sessionDir;
    private readonly ManifestWriter _manifest;
    private readonly CancellationTokenSource _cts = new();
    private readonly List<Task>     _tasks = new();
    private long _backed;
    private long _failed;
    private long _refused;
    private long _procEventsRx;
    private long _unknownRx;
    private long _dupRx;
    private volatile bool _diskFull;

    /* fltmgr can redeliver a BackupRequest even after we successfully
     * answered it (observed in DbgView: same msgId arrives every second,
     * second reply returns STATUS_FLT_NO_WAITER_FOR_REPLY = 0x801F0020).
     * Track ids we've already processed and ignore the dups so we don't
     * pollute History with phantom rows and don't spin on a dead
     * message. Cap the set so it can't grow unbounded. */
    private readonly HashSet<ulong>    _seenMsgIds   = new();
    private readonly Queue<ulong>      _seenMsgOrder = new();
    private const int                  SeenMsgCap    = 4096;

    public Action<BackupEvent>?         OnBackup;
    public Action<ProcessEventNotify>?  OnProcessEvent;

    public long Backed       => Interlocked.Read(ref _backed);
    public long Failed       => Interlocked.Read(ref _failed);
    public long Refused      => Interlocked.Read(ref _refused);
    public long ProcEventsRx => Interlocked.Read(ref _procEventsRx);
    public long UnknownRx    => Interlocked.Read(ref _unknownRx);
    public long DupRx        => Interlocked.Read(ref _dupRx);

    private bool RememberMsgId(ulong msgId)
    {
        /* Called only from the single WorkerLoop thread - no lock needed. */
        if (msgId == 0)
        {
            /* Defensive only: current kernel pushes use reply mode and
             * carry real message ids. Never dedup a zero-id message. */
            return true;
        }
        if (_seenMsgIds.Contains(msgId))
        {
            return false;
        }
        if (_seenMsgIds.Count >= SeenMsgCap)
        {
            ulong evict = _seenMsgOrder.Dequeue();
            _seenMsgIds.Remove(evict);
        }
        _seenMsgIds.Add(msgId);
        _seenMsgOrder.Enqueue(msgId);
        return true;
    }

    /* Latest exception captured by WorkerLoop, if any. The UX polls this
     * and shows it in the status bar so problems are not silent. */
    private volatile string _lastError = "";
    public string LastError => _lastError;

    private void LogException(int id, Exception ex)
    {
        _lastError = "[w" + id + "] " + ex.GetType().Name + ": " + ex.Message;
    }
    public bool DiskFull
    {
        get => _diskFull;
        set => _diskFull = value;
    }

    public BackupWorker(SafeFileHandle port, string sessionDir, ManifestWriter manifest)
    {
        _port = port;
        _sessionDir = sessionDir;
        _manifest = manifest;
    }

    public void Start(int workerCount)
    {
        if (workerCount < 1)  { workerCount = 1; }
        if (workerCount > 16) { workerCount = 16; }
        for (int i = 0; i < workerCount; ++i)
        {
            int id = i;
            _tasks.Add(Task.Run(() => WorkerLoop(id, _cts.Token)));
        }
    }

    public void Stop()
    {
        _cts.Cancel();
        try { Native.CancelIoEx(_port, IntPtr.Zero); } catch { }
        try { Task.WaitAll(_tasks.ToArray(), TimeSpan.FromSeconds(10)); } catch { }
    }

    public void Dispose()
    {
        Stop();
        _cts.Dispose();
    }

    private unsafe void WorkerLoop(int id, CancellationToken ct)
    {
        const int BufSize = 64 * 1024;
        IntPtr buf = Marshal.AllocHGlobal(BufSize);
        try
        {
            int headSize = Marshal.SizeOf<FilterMessageHeader>();
            int reqSize  = Marshal.SizeOf<BackupRequest>();
            int replySize = Marshal.SizeOf<FilterReplyHeader>() + Marshal.SizeOf<BackupReply>();
            IntPtr replyBuf = Marshal.AllocHGlobal(replySize);
            try
            {
                while (!ct.IsCancellationRequested)
                {
                    try
                    {
                    int hr = Native.FilterGetMessage(_port, buf, BufSize, IntPtr.Zero);
                    if (hr != 0)
                    {
                        int err = Native.HResultToWin32(hr);
                        if (ct.IsCancellationRequested
                            || err == Native.ERROR_OPERATION_ABORTED
                            || err == Native.ERROR_INVALID_HANDLE)
                        {
                            break;
                        }
                        Thread.Sleep(50);
                        continue;
                    }

                    /* buf layout: FILTER_MESSAGE_HEADER + KNFC_MSG_HEADER + body[] */
                    var msgHdr = Marshal.PtrToStructure<FilterMessageHeader>(buf);
                    IntPtr bodyHead = (IntPtr)((byte*)buf + headSize);
                    var inner = Marshal.PtrToStructure<MsgHeader>(bodyHead);

                    if (inner.Type == (uint)MsgType.BackupRequest)
                    {
                        var req = Marshal.PtrToStructure<BackupRequest>(bodyHead);
                        int cch = (int)(req.PathLengthBytes / sizeof(char));
                        string srcNt = Marshal.PtrToStringUni(
                            (IntPtr)((byte*)bodyHead + reqSize), cch);

                        bool isFresh = RememberMsgId(msgHdr.MessageId);
                        if (!isFresh)
                        {
                            /* fltmgr redelivered a message we already
                             * processed. Reply with STATUS_SUCCESS so
                             * the message slot at least gets a chance
                             * to clean up, but skip OnBackup so History
                             * stays single-entry. */
                            Interlocked.Increment(ref _dupRx);
                            Marshal.WriteInt32(replyBuf, 0, 0);
                            Marshal.WriteInt32(replyBuf, 4, 0);
                            Marshal.WriteInt64(replyBuf, 8, (long)msgHdr.MessageId);
                            IntPtr dupBody = (IntPtr)((byte*)replyBuf + Marshal.SizeOf<FilterReplyHeader>());
                            var dupRep = new BackupReply
                            {
                                Header    = new MsgHeader { Type = (uint)MsgType.BackupRequest, Size = (uint)Marshal.SizeOf<BackupReply>() },
                                RequestId = req.RequestId,
                                Status    = 0
                            };
                            Marshal.StructureToPtr(dupRep, dupBody, false);
                            _ = Native.FilterReplyMessage(_port, replyBuf, (uint)replySize);
                            continue;
                        }

                        string destPath = "";
                        int status;
                        if (_diskFull)
                        {
                            status = Native.ERROR_DISK_FULL;
                            Interlocked.Increment(ref _refused);
                        }
                        else
                        {
                            status = DoBackup(srcNt, req.RequestId, out destPath);
                            if (status == 0) { Interlocked.Increment(ref _backed); }
                            else             { Interlocked.Increment(ref _failed); }
                        }

                        var evt = new BackupEvent
                        {
                            Id       = req.RequestId,
                            TsUtc    = DateTime.UtcNow,
                            Pid      = req.OwnerPid,
                            RootPid  = req.RootPid,
                            Flags    = req.Flags,
                            SizeHint = req.FileSizeHint,
                            SrcNt    = srcNt,
                            Dest     = destPath,
                            Status   = status
                        };
                        _manifest.WriteEntry(evt.Id, evt.TsUtc, evt.Pid, evt.RootPid, evt.Flags,
                            evt.SizeHint, evt.SrcNt, evt.Dest, evt.Status);
                        OnBackup?.Invoke(evt);

                        /* Reply: FILTER_REPLY_HEADER (16 bytes) + KNFC_BACKUP_REPLY.
                         * Status at offset 0, then 4 bytes of padding, then
                         * MessageId at offset 8 - matches the explicit
                         * layout in FilterReplyHeader. */
                        Marshal.WriteInt32(replyBuf, 0, 0);
                        Marshal.WriteInt32(replyBuf, 4, 0);
                        Marshal.WriteInt64(replyBuf, 8, (long)msgHdr.MessageId);
                        IntPtr bodyPtr = (IntPtr)((byte*)replyBuf + Marshal.SizeOf<FilterReplyHeader>());
                        var body = new BackupReply
                        {
                            Header    = new MsgHeader { Type = (uint)MsgType.BackupRequest, Size = (uint)Marshal.SizeOf<BackupReply>() },
                            RequestId = req.RequestId,
                            Status    = (uint)status
                        };
                        Marshal.StructureToPtr(body, bodyPtr, false);

                        int rhr = Native.FilterReplyMessage(_port, replyBuf, (uint)replySize);
                        if (rhr != 0 && ct.IsCancellationRequested) { break; }
                    }
                    else if (inner.Type == (uint)MsgType.ProcessEvent)
                    {
                        Interlocked.Increment(ref _procEventsRx);
                        /* Push-mode ProcessEvent still requires a reply.
                         * Ack after decoding so fltmgr releases the slot.
                         */
                        var pev = Marshal.PtrToStructure<ProcessEventMsg>(bodyHead);

                        string image = "";
                        int icch = (int)pev.Proc.ImageLenChars;
                        if (icch > 0 && icch <= 120)
                        {
                            /* Image is the last field of ProcWire which is the last field
                             * of ProcessEventMsg, so its offset = total size - buffer size.
                             * Avoids Marshal.OffsetOf quirks with fixed buffers.
                             */
                            int imgOff = Marshal.SizeOf<ProcessEventMsg>() - 120 * sizeof(char);
                            IntPtr imgPtr = (IntPtr)((byte*)bodyHead + imgOff);
                            image = Marshal.PtrToStringUni(imgPtr, icch) ?? "";
                        }

                        OnProcessEvent?.Invoke(new ProcessEventNotify
                        {
                            EventType = pev.EventType,
                            Pid       = pev.Proc.Pid,
                            ParentPid = pev.Proc.ParentPid,
                            RootPid   = pev.Proc.RootPid,
                            Flags     = pev.Proc.Flags,
                            Image     = image
                        });

                        /* ProcessEvent now expects a reply (driver waits 500 ms)
                         * so fltmgr cleans the message slot. Without this,
                         * a "push" leaks back into the user queue and we
                         * receive the same EXITED event every second
                         * forever. */
                        int peReplyBodySize = Marshal.SizeOf<ProcessEventReply>();
                        int peReplyTotal    = Marshal.SizeOf<FilterReplyHeader>() + peReplyBodySize;
                        IntPtr peReplyBuf   = Marshal.AllocHGlobal(peReplyTotal);
                        try
                        {
                            Marshal.WriteInt32(peReplyBuf, 0, 0);
                            Marshal.WriteInt32(peReplyBuf, 4, 0);
                            Marshal.WriteInt64(peReplyBuf, 8, (long)msgHdr.MessageId);
                            IntPtr peBody = (IntPtr)((byte*)peReplyBuf + Marshal.SizeOf<FilterReplyHeader>());
                            var peBodyVal = new ProcessEventReply
                            {
                                Header = new MsgHeader { Type = (uint)MsgType.ProcessEvent, Size = (uint)peReplyBodySize },
                                Status = 0
                            };
                            Marshal.StructureToPtr(peBodyVal, peBody, false);
                            _ = Native.FilterReplyMessage(_port, peReplyBuf, (uint)peReplyTotal);
                        }
                        finally
                        {
                            Marshal.FreeHGlobal(peReplyBuf);
                        }
                    }
                    else
                    {
                        /* Drop unknown message types - bump a counter for diag. */
                        Interlocked.Increment(ref _unknownRx);
                    }
                    }
                    catch (Exception ex)
                    {
                        /* Without this catch, an exception inside an OnBackup
                         * callback (e.g. a ViewModel doing UI work) would
                         * tear down this entire worker thread - silently.
                         * Now we capture the failure to disk and to a
                         * volatile LastError string the UX can display.
                         *
                         * We do NOT use OutputDebugString here - if many
                         * messages stack up while a debugger is attached,
                         * the DBWIN buffer mutex contention can hang both
                         * the worker AND the debugger.
                         */
                        LogException(id, ex);
                        Interlocked.Increment(ref _failed);
                    }
                }
            }
            finally
            {
                Marshal.FreeHGlobal(replyBuf);
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
    }

    private int DoBackup(string srcNt, ulong requestId, out string destPath)
    {
        destPath = "";
        if (string.IsNullOrEmpty(srcNt))
        {
            return 87;  /* ERROR_INVALID_PARAMETER */
        }

        string globalSrc = @"\\?\GLOBALROOT" + srcNt;

        using SafeFileHandle src = Native.CreateFile(
            globalSrc,
            Native.GENERIC_READ,
            Native.FILE_SHARE_READ | Native.FILE_SHARE_WRITE | Native.FILE_SHARE_DELETE,
            IntPtr.Zero,
            Native.OPEN_EXISTING,
            Native.FILE_ATTRIBUTE_NORMAL | Native.FILE_FLAG_SEQUENTIAL_SCAN,
            IntPtr.Zero);
        if (src.IsInvalid)
        {
            return Marshal.GetLastWin32Error();
        }

        string rel = SanitizeNt(srcNt);
        string dest = Path.Combine(_sessionDir, rel);

        string? parent = Path.GetDirectoryName(dest);
        if (!string.IsNullOrEmpty(parent))
        {
            try { Directory.CreateDirectory(parent); } catch { }
        }

        /* Same-path retention (.r<id> suffix) */
        if (File.Exists(dest))
        {
            dest = dest + ".r" + requestId.ToString();
        }
        destPath = dest;
        string destLong = MakeLongPath(dest);

        using SafeFileHandle dst = Native.CreateFile(
            destLong,
            Native.GENERIC_WRITE,
            Native.FILE_SHARE_READ,
            IntPtr.Zero,
            Native.CREATE_ALWAYS,
            Native.FILE_ATTRIBUTE_NORMAL,
            IntPtr.Zero);
        if (dst.IsInvalid)
        {
            return Marshal.GetLastWin32Error();
        }

        const int Chunk = 1 << 20;  /* 1 MB */
        IntPtr buf = Marshal.AllocHGlobal(Chunk);
        try
        {
            while (true)
            {
                if (!Native.ReadFile(src, buf, (uint)Chunk, out uint got, IntPtr.Zero))
                {
                    return Marshal.GetLastWin32Error();
                }
                if (got == 0) { break; }
                if (!Native.WriteFile(dst, buf, got, out uint wrote, IntPtr.Zero) || wrote != got)
                {
                    return Marshal.GetLastWin32Error();
                }
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
        return 0;
    }

    private static string SanitizeNt(string nt)
    {
        if (string.IsNullOrEmpty(nt)) { return nt; }
        int start = (nt[0] == '\\') ? 1 : 0;
        var sb = new System.Text.StringBuilder(nt.Length);
        for (int i = start; i < nt.Length; ++i)
        {
            char c = nt[i];
            if (c == ':') { c = '$'; }   /* ADS escape */
            sb.Append(c);
        }
        return sb.ToString();
    }

    private static string MakeLongPath(string p)
    {
        if (string.IsNullOrEmpty(p)) { return p; }
        if (p.StartsWith(@"\\?\", StringComparison.Ordinal)) { return p; }
        if (p.Length >= 2 && p[0] == '\\' && p[1] == '\\')
        {
            return @"\\?\UNC\" + p.Substring(2);
        }
        if (p.Length >= 2 && p[1] == ':')
        {
            return @"\\?\" + p;
        }
        return p;
    }
}
