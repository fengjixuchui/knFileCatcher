/*
 * DriverClient.cs
 * Thin wrapper around FilterConnectCommunicationPort + FilterSendMessage.
 *
 * MaxConnections=1: only one of {WPF UX, this process} can hold the
 * port at any moment. Since the WPF UX is now the sole client, no
 * IPC pipe is needed - all control flows through this class.
 */

using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace KnFc.Ux;

internal sealed class DriverClient : IDisposable
{
    private SafeFileHandle? _port;

    public bool IsConnected => _port is { IsInvalid: false, IsClosed: false };
    public SafeFileHandle Port => _port!;

    public int Connect()
    {
        int hr = Native.FilterConnectCommunicationPort(
            WireProto.PortName, 0, IntPtr.Zero, 0, IntPtr.Zero, out var port);
        if (hr != 0)
        {
            return Native.HResultToWin32(hr);
        }
        _port = port;
        return 0;
    }

    public void Disconnect()
    {
        if (_port != null)
        {
            try { Native.CancelIoEx(_port, IntPtr.Zero); } catch { }
            _port.Dispose();
            _port = null;
        }
    }

    public int SendHeader(MsgType type)
    {
        if (!IsConnected) { return Native.ERROR_INVALID_HANDLE; }
        var hdr = new MsgHeader { Type = (uint)type, Size = (uint)Marshal.SizeOf<MsgHeader>() };
        return SendBuffer(in hdr, (uint)Marshal.SizeOf<MsgHeader>(), IntPtr.Zero, 0, out _);
    }

    public unsafe int AddWatchRoot(string ntPath)
    {
        if (string.IsNullOrEmpty(ntPath)) { return Native.ERROR_ACCESS_DENIED; }
        return SendVariablePayload(MsgType.AddWatchRoot, ntPath);
    }

    public unsafe int AddExclude(string pattern)
    {
        if (string.IsNullOrEmpty(pattern)) { return Native.ERROR_ACCESS_DENIED; }
        return SendVariablePayload(MsgType.AddExclude, pattern);
    }

    private unsafe int SendVariablePayload(MsgType type, string payload)
    {
        uint bytes  = (uint)(payload.Length * sizeof(char));
        uint msgLen = (uint)Marshal.SizeOf<AddPathHeader>() + bytes;
        IntPtr buf = Marshal.AllocHGlobal((int)msgLen);
        try
        {
            var hdr = new AddPathHeader
            {
                Header = new MsgHeader { Type = (uint)type, Size = msgLen },
                PayloadLengthBytes = bytes
            };
            Marshal.StructureToPtr(hdr, buf, false);
            fixed (char* p = payload)
            {
                Buffer.MemoryCopy(p, (byte*)buf + Marshal.SizeOf<AddPathHeader>(), bytes, bytes);
            }
            int hr = Native.FilterSendMessage(_port!, buf, msgLen, IntPtr.Zero, 0, out _);
            return hr == 0 ? 0 : Native.HResultToWin32(hr);
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
    }

    public int GetStats(out StatsWire stats)
    {
        stats = default;
        if (!IsConnected) { return Native.ERROR_INVALID_HANDLE; }

        var reqHdr = new MsgHeader { Type = (uint)MsgType.GetStats, Size = (uint)Marshal.SizeOf<MsgHeader>() };
        int replySize = Marshal.SizeOf<StatsReply>();
        IntPtr replyBuf = Marshal.AllocHGlobal(replySize);
        try
        {
            int rc = SendBuffer(in reqHdr, (uint)Marshal.SizeOf<MsgHeader>(), replyBuf, (uint)replySize, out _);
            if (rc != 0) { return rc; }
            var reply = Marshal.PtrToStructure<StatsReply>(replyBuf);
            stats = reply.Stats;
            return 0;
        }
        finally
        {
            Marshal.FreeHGlobal(replyBuf);
        }
    }

    public unsafe int GetProcessTree(out System.Collections.Generic.List<ProcWire> procs, out bool truncated)
    {
        procs = new System.Collections.Generic.List<ProcWire>();
        truncated = false;
        if (!IsConnected) { return Native.ERROR_INVALID_HANDLE; }

        var reqHdr = new MsgHeader { Type = (uint)MsgType.GetProcessTree, Size = (uint)Marshal.SizeOf<MsgHeader>() };
        int headSize = Marshal.SizeOf<ProcessTreeReplyHead>();
        int procSize = sizeof(ProcWire);
        int replySize = headSize + WireProto.MaxTreeEntries * procSize;

        IntPtr replyBuf = Marshal.AllocHGlobal(replySize);
        try
        {
            int rc = SendBuffer(in reqHdr, (uint)Marshal.SizeOf<MsgHeader>(),
                replyBuf, (uint)replySize, out _);
            if (rc != 0) { return rc; }
            var head = Marshal.PtrToStructure<ProcessTreeReplyHead>(replyBuf);
            truncated = head.Truncated != 0;
            byte* p = (byte*)replyBuf + headSize;
            for (uint i = 0; i < head.Count && i < WireProto.MaxTreeEntries; ++i)
            {
                var pr = Marshal.PtrToStructure<ProcWire>((IntPtr)(p + i * procSize));
                procs.Add(pr);
            }
            return 0;
        }
        finally
        {
            Marshal.FreeHGlobal(replyBuf);
        }
    }

    private unsafe int SendBuffer<T>(in T req, uint reqSize, IntPtr reply, uint replySize, out uint bytesReturned)
        where T : struct
    {
        bytesReturned = 0;
        IntPtr reqBuf = Marshal.AllocHGlobal((int)reqSize);
        try
        {
            Marshal.StructureToPtr(req, reqBuf, false);
            int hr = Native.FilterSendMessage(_port!, reqBuf, reqSize, reply, replySize, out bytesReturned);
            return hr == 0 ? 0 : Native.HResultToWin32(hr);
        }
        finally
        {
            Marshal.FreeHGlobal(reqBuf);
        }
    }

    public void Dispose() => Disconnect();
}
