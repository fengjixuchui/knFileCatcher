/*
 * knFcProto.h
 * Shared protocol definitions between the kernel driver (knFcFlt.sys)
 * and the single user-mode app (knFcUxWpf.exe). The legacy
 * knFcSvc/knFcCli/knFcUx binaries are no longer built; only stale
 * staging artifacts may exist under ignored build output.
 * ASCII only. Stable wire format - bump KNFC_PROTOCOL_VERSION on change.
 */

#pragma once

#define KNFC_PORT_NAME                  L"\\knFcFltPort"
#define KNFC_PROTOCOL_VERSION           0x00070000  /* 7.0 - M7 process tree + PreCleanup */

#define KNFC_MAX_TREE_ENTRIES           128
#define KNFC_TREE_IMAGE_CHARS           120     /* per-process image, no NUL */

#define KNFC_MAX_WATCH_ROOTS            16
#define KNFC_MAX_WATCH_PATH_CHARS       520
#define KNFC_MAX_BACKUP_PATH_CHARS      1024

#define KNFC_MAX_EXCLUDE_PATTERNS       64
#define KNFC_MAX_EXCLUDE_CHARS          128

/* StreamHandle context flags (kernel SHC + KNFC_BACKUP_REQUEST::Flags) */
#define KNFC_SHC_WRITE_INTENT           0x00000001
#define KNFC_SHC_MODIFIED               0x00000002
#define KNFC_SHC_DELETE_ON_CLOSE        0x00000004
#define KNFC_SHC_RENAMED                0x00000008
#define KNFC_SHC_TEMPORARY              0x00000010
#define KNFC_SHC_CREATED                0x00000020   /* file was created (CREATE/SUPERSEDE/OVERWRITE*) */
#define KNFC_SHC_BACKED_UP              0x00000040   /* already sent (PreCleanup sync) - PostCleanup skip */

/* Message type IDs */
typedef enum _KNFC_MSG_TYPE
{
    KnFcMsgPing                 = 1,
    KnFcMsgClearWatchRoots      = 5,
    KnFcMsgAddWatchRoot         = 6,
    KnFcMsgStart                = 7,
    KnFcMsgStop                 = 8,
    KnFcMsgClearExcludes        = 9,    /* client -> kernel */
    KnFcMsgAddExclude           = 10,   /* client -> kernel */
    KnFcMsgGetStats             = 11,   /* client -> kernel, reply = KNFC_STATS_REPLY */
    KnFcMsgGetProcessTree       = 12,   /* client -> kernel, reply = KNFC_PROCESS_TREE_REPLY */
    KnFcMsgBackupRequest        = 100,  /* kernel -> client (async via Enqueue OR sync from PreCleanup) */
    KnFcMsgEventLog             = 101,  /* kernel -> client : debug log (unused) */
    KnFcMsgProcessEvent         = 102   /* kernel -> client : process create/exit push, reply required */
} KNFC_MSG_TYPE;

/* KNFC_PROCESS_EVENT::EventType */
#define KNFC_PROC_EVENT_CREATED     1
#define KNFC_PROC_EVENT_EXITED      2

#pragma pack(push, 1)

typedef struct _KNFC_MSG_HEADER
{
    unsigned int Type;
    unsigned int Size;
} KNFC_MSG_HEADER;

typedef struct _KNFC_PING_REPLY
{
    KNFC_MSG_HEADER Header;
    unsigned long long TickCount;
    unsigned int       Version;
    unsigned int       Reserved;
} KNFC_PING_REPLY;

typedef struct _KNFC_ADD_WATCH_ROOT_REQ
{
    KNFC_MSG_HEADER Header;
    unsigned int    PathLengthBytes;
    /* WCHAR Path[PathLengthBytes/2] */
} KNFC_ADD_WATCH_ROOT_REQ;

/* Exclude pattern syntax:
 *   - starts with '.'   -> case-insensitive extension suffix match (e.g. ".tmp")
 *   - otherwise         -> case-insensitive substring match against the NT path
 *                          (e.g. "\Temp\" or "\AppData\Local\Temp")
 */
typedef struct _KNFC_ADD_EXCLUDE_REQ
{
    KNFC_MSG_HEADER Header;
    unsigned int    PatternLengthBytes;
    /* WCHAR Pattern[PatternLengthBytes/2] */
} KNFC_ADD_EXCLUDE_REQ;

typedef struct _KNFC_STATS
{
    unsigned int       Active;             /* 0 / 1  - tracking enabled */
    unsigned int       TrackedProcessCount;
    unsigned int       WatchRootCount;
    unsigned int       ExcludeCount;
    unsigned long long QueueDepth;
    unsigned long long QueueSent;
    unsigned long long QueueSendFailed;
    unsigned long long QueueDropped;
    unsigned long long ExcludeMatched;
    unsigned long long SyncCleanupSent;    /* DELETE_ON_CLOSE sync paths */
    unsigned long long SyncCleanupFailed;
} KNFC_STATS;

typedef struct _KNFC_STATS_REPLY
{
    KNFC_MSG_HEADER Header;
    KNFC_STATS      Stats;
} KNFC_STATS_REPLY;

typedef struct _KNFC_BACKUP_REQUEST
{
    KNFC_MSG_HEADER Header;
    unsigned long long RequestId;
    unsigned long long OwnerPid;
    unsigned long long RootPid;
    unsigned long long FileSizeHint;
    unsigned int       Flags;
    unsigned int       PathLengthBytes;
    /* WCHAR Path[PathLengthBytes/2] */
} KNFC_BACKUP_REQUEST;

typedef struct _KNFC_BACKUP_REPLY
{
    KNFC_MSG_HEADER    Header;
    unsigned long long RequestId;
    unsigned int       Status;
    unsigned int       Reserved;
} KNFC_BACKUP_REPLY;

typedef struct _KNFC_PROC
{
    unsigned long long Pid;
    unsigned long long ParentPid;
    unsigned long long RootPid;
    unsigned int       Flags;            /* KnFcTrack* bits */
    unsigned int       ImageLenChars;    /* count, no NUL */
    wchar_t            Image[KNFC_TREE_IMAGE_CHARS];
} KNFC_PROC;

typedef struct _KNFC_PROCESS_TREE_REPLY
{
    KNFC_MSG_HEADER Header;
    unsigned int    Count;        /* entries actually filled */
    unsigned int    Truncated;    /* nonzero if more than KNFC_MAX_TREE_ENTRIES were available */
    KNFC_PROC       Procs[KNFC_MAX_TREE_ENTRIES];
} KNFC_PROCESS_TREE_REPLY;

/* Push-mode notification: driver -> client whenever a tracked process
 * is created or exits. The client must reply (any status) so fltmgr
 * cleans the message slot up - a true no-reply FltSendMessage leaks
 * the message back into the user queue and the client receives it
 * forever.
 */
typedef struct _KNFC_PROCESS_EVENT
{
    KNFC_MSG_HEADER Header;
    unsigned int    EventType;      /* KNFC_PROC_EVENT_CREATED / _EXITED */
    unsigned int    Reserved;
    KNFC_PROC       Proc;
} KNFC_PROCESS_EVENT;

typedef struct _KNFC_PROCESS_EVENT_REPLY
{
    KNFC_MSG_HEADER Header;
    unsigned int    Status;
    unsigned int    Reserved;
} KNFC_PROCESS_EVENT_REPLY;

#pragma pack(pop)
