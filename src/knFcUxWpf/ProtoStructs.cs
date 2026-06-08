/*
 * ProtoStructs.cs
 * C# mirror of src\common\knFcProto.h. All structs are Pack=1 so the
 * layout matches the kernel/user wire format exactly.
 */

using System.Runtime.InteropServices;

namespace KnFc.Ux;

internal static class WireProto
{
    public const string PortName            = "\\knFcFltPort";
    public const uint   Version             = 0x00070000;

    public const int    MaxWatchPathChars   = 520;
    public const int    MaxExcludeChars     = 128;
    public const int    MaxTreeEntries      = 128;
    public const int    TreeImageChars      = 120;
}

internal enum MsgType : uint
{
    Ping              = 1,
    ClearWatchRoots   = 5,
    AddWatchRoot      = 6,
    Start             = 7,
    Stop              = 8,
    ClearExcludes     = 9,
    AddExclude        = 10,
    GetStats          = 11,
    GetProcessTree    = 12,
    BackupRequest     = 100,
    EventLog          = 101,
    ProcessEvent      = 102,
}

internal static class ProcEventType
{
    public const uint Created = 1;
    public const uint Exited  = 2;
}

internal static class TrackFlags
{
    public const uint Root     = 0x0001;
    public const uint Child    = 0x0002;
    public const uint Exited   = 0x0004;
    public const uint FromSnap = 0x0008;
}

internal static class ShcFlags
{
    public const uint WriteIntent     = 0x01;
    public const uint Modified        = 0x02;
    public const uint DeleteOnClose   = 0x04;
    public const uint Renamed         = 0x08;
    public const uint Temporary       = 0x10;
    public const uint Created         = 0x20;
    public const uint BackedUp        = 0x40;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct MsgHeader
{
    public uint Type;
    public uint Size;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct PingReply
{
    public MsgHeader Header;
    public ulong     TickCount;
    public uint      Version;
    public uint      Reserved;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct AddPathHeader
{
    public MsgHeader Header;
    public uint      PayloadLengthBytes;
    /* wchar_t Payload[]; appended in the same buffer */
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct StatsWire
{
    public uint  Active;
    public uint  TrackedProcessCount;
    public uint  WatchRootCount;
    public uint  ExcludeCount;
    public ulong QueueDepth;
    public ulong QueueSent;
    public ulong QueueSendFailed;
    public ulong QueueDropped;
    public ulong ExcludeMatched;
    public ulong SyncCleanupSent;
    public ulong SyncCleanupFailed;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct StatsReply
{
    public MsgHeader Header;
    public StatsWire Stats;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal unsafe struct ProcWire
{
    public ulong Pid;
    public ulong ParentPid;
    public ulong RootPid;
    public uint  Flags;
    public uint  ImageLenChars;
    /* fixed ushort Image[120] */
    public fixed ushort Image[120];
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct ProcessTreeReplyHead
{
    public MsgHeader Header;
    public uint      Count;
    public uint      Truncated;
    /* ProcWire Procs[MaxTreeEntries] follows */
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct ProcessEventMsg
{
    public MsgHeader Header;
    public uint      EventType;   /* ProcEventType.Created / Exited */
    public uint      Reserved;
    public ProcWire  Proc;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct ProcessEventReply
{
    public MsgHeader Header;
    public uint      Status;
    public uint      Reserved;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct BackupRequest
{
    public MsgHeader Header;
    public ulong     RequestId;
    public ulong     OwnerPid;
    public ulong     RootPid;
    public ulong     FileSizeHint;
    public uint      Flags;
    public uint      PathLengthBytes;
    /* wchar_t Path[] follows */
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct BackupReply
{
    public MsgHeader Header;
    public ulong     RequestId;
    public uint      Status;
    public uint      Reserved;
}

/* fltUser.h: FILTER_MESSAGE_HEADER / FILTER_REPLY_HEADER
 *
 * IMPORTANT: these MUST be 16 bytes each. The Win32 typedefs:
 *   typedef struct _FILTER_MESSAGE_HEADER {
 *       ULONG     ReplyLength;
 *       ULONGLONG MessageId;
 *   };
 *   typedef struct _FILTER_REPLY_HEADER {
 *       NTSTATUS  Status;
 *       ULONGLONG MessageId;
 *   };
 * have default alignment - the ULONGLONG forces 4 bytes of padding after
 * the leading 4-byte field. Pack=1 would give 12 bytes and silently
 * shift every subsequent body read by 4 bytes, which scrambles the
 * KNFC_MSG_HEADER and leaves Reply's MessageId at the wrong offset so
 * the kernel never matches the reply. Use explicit layout for clarity.
 */
[StructLayout(LayoutKind.Explicit, Size = 16)]
internal struct FilterMessageHeader
{
    [FieldOffset(0)] public uint  ReplyLength;
    [FieldOffset(8)] public ulong MessageId;
}

[StructLayout(LayoutKind.Explicit, Size = 16)]
internal struct FilterReplyHeader
{
    [FieldOffset(0)] public int   Status;
    [FieldOffset(8)] public ulong MessageId;
}
