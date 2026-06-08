/*
 * knFcComm.cpp
 * FilterCommunicationPort - kernel<->user message channel.
 *
 * M2 additions:
 *   - KnFcMsgClearWatchRoots
 *   - KnFcMsgAddWatchRoot (variable-length WCHAR path)
 *   - KnFcMsgStart / KnFcMsgStop
 */

#include "knFcFlt.h"

static PFLT_PORT    g_ServerPort = NULL;
static PFLT_PORT    g_ClientPort = NULL;
static FAST_MUTEX   g_ClientLock;

static NTSTATUS FLTAPI knFcCommConnect(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID* ConnectionPortCookie
    );

static VOID FLTAPI knFcCommDisconnect(_In_opt_ PVOID ConnectionCookie);

static NTSTATUS FLTAPI knFcCommMessage(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    );

NTSTATUS
knFcCommInitialize(_In_ PFLT_FILTER Filter)
{
    NTSTATUS status;
    PSECURITY_DESCRIPTOR sd = NULL;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING portName;

    ExInitializeFastMutex(&g_ClientLock);

    do
    {
        status = FltBuildDefaultSecurityDescriptor(&sd, FLT_PORT_ALL_ACCESS);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: FltBuildDefaultSecurityDescriptor failed 0x%08x\n", status);
            break;
        }

        RtlInitUnicodeString(&portName, KNFC_PORT_NAME);
        InitializeObjectAttributes(
            &oa,
            &portName,
            OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
            NULL,
            sd);

        status = FltCreateCommunicationPort(
            Filter,
            &g_ServerPort,
            &oa,
            NULL,
            knFcCommConnect,
            knFcCommDisconnect,
            knFcCommMessage,
            1);     /* single client per Design sec 15 */
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: FltCreateCommunicationPort failed 0x%08x\n", status);
            break;
        }

        DbgPrint("knFcFlt: comm port: %wZ\n", &portName);
        status = STATUS_SUCCESS;
    }
    while (FALSE);

    if (sd != NULL)
    {
        FltFreeSecurityDescriptor(sd);
    }
    return status;
}

VOID
knFcCommUninitialize(VOID)
{
    /* Disconnect any still-attached client first. Otherwise the
     * subsequent FltUnregisterFilter can hang/fail with the client
     * port still referenced by fltmgr. The disconnect callback only
     * fires when the user-mode handle is released by the client; on
     * an abnormal client exit (process crash) the port can be left
     * open until we explicitly close it here. */
    ExAcquireFastMutex(&g_ClientLock);
    if (g_ClientPort != NULL)
    {
        FltCloseClientPort(g_FilterHandle, &g_ClientPort);
        g_ClientPort = NULL;
    }
    ExReleaseFastMutex(&g_ClientLock);

    if (g_ServerPort != NULL)
    {
        FltCloseCommunicationPort(g_ServerPort);
        g_ServerPort = NULL;
    }
}

NTSTATUS
knFcCommSendMessage(
    _In_ PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_writes_bytes_opt_(*ReplyLength) PVOID Reply,
    _Inout_opt_ PULONG ReplyLength,
    _In_opt_ PLARGE_INTEGER Timeout
    )
{
    NTSTATUS status;
    ULONG rlen;

    ExAcquireFastMutex(&g_ClientLock);

    do
    {
        if (g_ClientPort == NULL)
        {
            if (ReplyLength != NULL)
            {
                *ReplyLength = 0;
            }
            status = STATUS_PORT_DISCONNECTED;
            break;
        }

        /* Hold g_ClientLock across FltSendMessage so disconnect/uninit
         * cannot close or NULL the shared client port pointer while a
         * worker is sending through it. The callers are PASSIVE_LEVEL
         * system threads or PreCleanup, and every send path has a
         * bounded timeout.
         */
        if (Reply == NULL)
        {
            status = FltSendMessage(
                g_FilterHandle,
                &g_ClientPort,
                Buffer,
                BufferLength,
                NULL, NULL,
                Timeout);
            break;
        }

        rlen = (ReplyLength != NULL) ? *ReplyLength : 0;

        status = FltSendMessage(
            g_FilterHandle,
            &g_ClientPort,
            Buffer,
            BufferLength,
            Reply,
            &rlen,
            Timeout);

        if (ReplyLength != NULL)
        {
            *ReplyLength = rlen;
        }
    }
    while (FALSE);

    ExReleaseFastMutex(&g_ClientLock);
    return status;
}

/* ----- push: KnFcMsgProcessEvent (reply required) ----- */

NTSTATUS
knFcCommPushProcessEvent(
    _In_ unsigned int EventType,
    _In_ HANDLE Pid,
    _In_ HANDLE ParentPid,
    _In_ HANDLE RootPid,
    _In_ ULONG  Flags,
    _In_opt_ PCUNICODE_STRING Image
    )
{
    KNFC_PROCESS_EVENT ev;

    RtlZeroMemory(&ev, sizeof(ev));
    ev.Header.Type = (unsigned int)KnFcMsgProcessEvent;
    ev.Header.Size = sizeof(ev);
    ev.EventType   = EventType;
    ev.Proc.Pid       = (unsigned long long)(ULONG_PTR)Pid;
    ev.Proc.ParentPid = (unsigned long long)(ULONG_PTR)ParentPid;
    ev.Proc.RootPid   = (unsigned long long)(ULONG_PTR)RootPid;
    ev.Proc.Flags     = Flags;

    if (Image != NULL && Image->Buffer != NULL && Image->Length > 0)
    {
        USHORT cch = (USHORT)(Image->Length / sizeof(WCHAR));
        if (cch > KNFC_TREE_IMAGE_CHARS)
        {
            USHORT skip = (USHORT)(cch - KNFC_TREE_IMAGE_CHARS);
            RtlCopyMemory(ev.Proc.Image,
                Image->Buffer + skip,
                KNFC_TREE_IMAGE_CHARS * sizeof(WCHAR));
            ev.Proc.ImageLenChars = KNFC_TREE_IMAGE_CHARS;
        }
        else
        {
            RtlCopyMemory(ev.Proc.Image, Image->Buffer, Image->Length);
            ev.Proc.ImageLenChars = cch;
        }
    }

    /* Send WITH reply so fltmgr can free the message slot once the
     * client acks. NULL-reply (true fire-and-forget) leaves the message
     * pinned in the user-side queue and FilterGetMessage returns it
     * every second forever. Timeout is 500 ms - long enough for a
     * responsive client but short enough that one slow ack does not
     * back up the single deferred-push worker behind it. */
    {
        KNFC_PROCESS_EVENT_REPLY reply;
        ULONG replyLen = sizeof(reply);
        LARGE_INTEGER timeout;

        RtlZeroMemory(&reply, sizeof(reply));
        timeout.QuadPart = -((LONGLONG)500 * 10000);  /* 500 ms */
        return knFcCommSendMessage(&ev, sizeof(ev), &reply, &replyLen, &timeout);
    }
}

static NTSTATUS FLTAPI
knFcCommConnect(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID* ConnectionPortCookie
    )
{
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);

    ExAcquireFastMutex(&g_ClientLock);
    g_ClientPort = ClientPort;
    ExReleaseFastMutex(&g_ClientLock);

    if (ConnectionPortCookie != NULL)
    {
        *ConnectionPortCookie = NULL;
    }

    DbgPrint("knFcFlt: client connected\n");
    return STATUS_SUCCESS;
}

static VOID FLTAPI
knFcCommDisconnect(_In_opt_ PVOID ConnectionCookie)
{
    UNREFERENCED_PARAMETER(ConnectionCookie);

    ExAcquireFastMutex(&g_ClientLock);
    if (g_ClientPort != NULL)
    {
        FltCloseClientPort(g_FilterHandle, &g_ClientPort);
        g_ClientPort = NULL;
    }
    ExReleaseFastMutex(&g_ClientLock);

    DbgPrint("knFcFlt: client disconnected\n");
}

/* ----- per-type handlers ----- */

static NTSTATUS
knFcHandlePing(
    _Out_writes_bytes_to_(OutLen, *Returned) PVOID Out,
    _In_ ULONG OutLen,
    _Out_ PULONG Returned)
{
    KNFC_PING_REPLY reply;
    LARGE_INTEGER tick;

    if (Out == NULL || OutLen < sizeof(KNFC_PING_REPLY))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    KeQueryTickCount(&tick);
    RtlZeroMemory(&reply, sizeof(reply));
    reply.Header.Type = (unsigned int)KnFcMsgPing;
    reply.Header.Size = sizeof(reply);
    reply.TickCount   = (unsigned long long)tick.QuadPart;
    reply.Version     = KNFC_PROTOCOL_VERSION;

    __try
    {
        ProbeForWrite(Out, sizeof(reply), 1);
        RtlCopyMemory(Out, &reply, sizeof(reply));
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return GetExceptionCode();
    }
    *Returned = sizeof(reply);
    return STATUS_SUCCESS;
}

static NTSTATUS
knFcHandleGetStats(
    _Out_writes_bytes_to_(OutLen, *Returned) PVOID Out,
    _In_ ULONG OutLen,
    _Out_ PULONG Returned)
{
    KNFC_STATS_REPLY reply;
    ULONG depth;
    ULONGLONG sent, sendFailed, dropped, syncSent, syncFailed;

    if (Out == NULL || OutLen < sizeof(KNFC_STATS_REPLY))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(&reply, sizeof(reply));
    reply.Header.Type  = (unsigned int)KnFcMsgGetStats;
    reply.Header.Size  = sizeof(reply);

    knFcQueueGetStats(&depth, &sent, &sendFailed, &dropped, &syncSent, &syncFailed);

    reply.Stats.Active              = knFcTrackIsActive() ? 1u : 0u;
    reply.Stats.TrackedProcessCount = knFcTrackGetCount();
    reply.Stats.WatchRootCount      = knFcConfigGetRootCount();
    reply.Stats.ExcludeCount        = knFcExcludeGetCount();
    reply.Stats.QueueDepth          = depth;
    reply.Stats.QueueSent           = sent;
    reply.Stats.QueueSendFailed     = sendFailed;
    reply.Stats.QueueDropped        = dropped;
    reply.Stats.ExcludeMatched      = knFcExcludeGetMatchedCount();
    reply.Stats.SyncCleanupSent     = syncSent;
    reply.Stats.SyncCleanupFailed   = syncFailed;

    __try
    {
        ProbeForWrite(Out, sizeof(reply), 1);
        RtlCopyMemory(Out, &reply, sizeof(reply));
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return GetExceptionCode();
    }
    *Returned = sizeof(reply);
    return STATUS_SUCCESS;
}

static NTSTATUS
knFcHandleGetProcessTree(
    _Out_writes_bytes_to_(OutLen, *Returned) PVOID Out,
    _In_ ULONG OutLen,
    _Out_ PULONG Returned)
{
    KNFC_PROCESS_TREE_REPLY* reply;
    ULONG count = 0;
    BOOLEAN truncated = FALSE;

    if (Out == NULL || OutLen < sizeof(KNFC_PROCESS_TREE_REPLY))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* The reply is ~100 KB - too large for stack. Build in pool, then
     * single ProbeForWrite + RtlCopyMemory under SEH.
     */
    reply = (KNFC_PROCESS_TREE_REPLY*)knFcAllocateNonPaged(sizeof(KNFC_PROCESS_TREE_REPLY));
    if (reply == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(reply, sizeof(KNFC_PROCESS_TREE_REPLY));
    reply->Header.Type = (unsigned int)KnFcMsgGetProcessTree;
    reply->Header.Size = sizeof(KNFC_PROCESS_TREE_REPLY);

    knFcTrackDump(reply->Procs, KNFC_MAX_TREE_ENTRIES, &count, &truncated);
    reply->Count     = count;
    reply->Truncated = truncated ? 1u : 0u;

    __try
    {
        ProbeForWrite(Out, sizeof(KNFC_PROCESS_TREE_REPLY), 1);
        RtlCopyMemory(Out, reply, sizeof(KNFC_PROCESS_TREE_REPLY));
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        ExFreePoolWithTag(reply, KNFC_POOL_TAG);
        return GetExceptionCode();
    }
    *Returned = sizeof(KNFC_PROCESS_TREE_REPLY);
    ExFreePoolWithTag(reply, KNFC_POOL_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS
knFcHandleAddExclude(
    _In_reads_bytes_(InLen) PVOID In,
    _In_ ULONG InLen)
{
    KNFC_ADD_EXCLUDE_REQ hdr;
    PVOID localCopy = NULL;
    UNICODE_STRING pat;
    NTSTATUS status;

    if (In == NULL || InLen < sizeof(KNFC_ADD_EXCLUDE_REQ))
    {
        return STATUS_INVALID_PARAMETER;
    }
    __try
    {
        ProbeForRead(In, InLen, 1);
        RtlCopyMemory(&hdr, In, sizeof(hdr));
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return GetExceptionCode();
    }

    if (hdr.PatternLengthBytes == 0
        || (hdr.PatternLengthBytes % sizeof(WCHAR)) != 0
        || hdr.PatternLengthBytes > (KNFC_MAX_EXCLUDE_CHARS * sizeof(WCHAR)))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (InLen < (sizeof(hdr) + hdr.PatternLengthBytes))
    {
        return STATUS_INVALID_PARAMETER;
    }

    localCopy = knFcAllocateNonPaged(hdr.PatternLengthBytes);
    if (localCopy == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    __try
    {
        RtlCopyMemory(localCopy, (PUCHAR)In + sizeof(hdr), hdr.PatternLengthBytes);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        ExFreePoolWithTag(localCopy, KNFC_POOL_TAG);
        return GetExceptionCode();
    }

    pat.Buffer        = (PWCH)localCopy;
    pat.Length        = (USHORT)hdr.PatternLengthBytes;
    pat.MaximumLength = (USHORT)hdr.PatternLengthBytes;

    status = knFcExcludeAdd(&pat);
    if (NT_SUCCESS(status))
    {
        DbgPrint("knFcFlt: exclude added: %wZ\n", &pat);
    }
    else
    {
        DbgPrint("knFcFlt: exclude add failed 0x%08x: %wZ\n", status, &pat);
    }
    ExFreePoolWithTag(localCopy, KNFC_POOL_TAG);
    return status;
}

static NTSTATUS
knFcHandleAddRoot(
    _In_reads_bytes_(InLen) PVOID In,
    _In_ ULONG InLen)
{
    KNFC_ADD_WATCH_ROOT_REQ hdr;
    PVOID localCopy = NULL;
    UNICODE_STRING path;
    NTSTATUS status;

    if (In == NULL || InLen < sizeof(KNFC_ADD_WATCH_ROOT_REQ))
    {
        return STATUS_INVALID_PARAMETER;
    }

    __try
    {
        ProbeForRead(In, InLen, 1);
        RtlCopyMemory(&hdr, In, sizeof(hdr));
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return GetExceptionCode();
    }

    if (hdr.PathLengthBytes == 0
        || (hdr.PathLengthBytes % sizeof(WCHAR)) != 0
        || hdr.PathLengthBytes > (KNFC_MAX_WATCH_PATH_CHARS * sizeof(WCHAR)))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (InLen < (sizeof(hdr) + hdr.PathLengthBytes))
    {
        return STATUS_INVALID_PARAMETER;
    }

    localCopy = knFcAllocateNonPaged(hdr.PathLengthBytes);
    if (localCopy == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try
    {
        RtlCopyMemory(
            localCopy,
            (PUCHAR)In + sizeof(hdr),
            hdr.PathLengthBytes);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        ExFreePoolWithTag(localCopy, KNFC_POOL_TAG);
        return GetExceptionCode();
    }

    path.Buffer        = (PWCH)localCopy;
    path.Length        = (USHORT)hdr.PathLengthBytes;
    path.MaximumLength = (USHORT)hdr.PathLengthBytes;

    status = knFcConfigAddRoot(&path);
    if (NT_SUCCESS(status))
    {
        DbgPrint("knFcFlt: watch root added: %wZ\n", &path);
    }
    else
    {
        DbgPrint("knFcFlt: watch root add failed 0x%08x: %wZ\n", status, &path);
    }

    ExFreePoolWithTag(localCopy, KNFC_POOL_TAG);
    return status;
}

static NTSTATUS FLTAPI
knFcCommMessage(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    KNFC_MSG_HEADER hdr;

    UNREFERENCED_PARAMETER(PortCookie);

    if (ReturnOutputBufferLength != NULL)
    {
        *ReturnOutputBufferLength = 0;
    }

    if (InputBuffer == NULL || InputBufferLength < sizeof(KNFC_MSG_HEADER))
    {
        return STATUS_INVALID_PARAMETER;
    }

    __try
    {
        ProbeForRead(InputBuffer, sizeof(KNFC_MSG_HEADER), 1);
        RtlCopyMemory(&hdr, InputBuffer, sizeof(KNFC_MSG_HEADER));
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return GetExceptionCode();
    }

    if (hdr.Size < sizeof(KNFC_MSG_HEADER)
        || hdr.Size > InputBufferLength)
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch ((KNFC_MSG_TYPE)hdr.Type)
    {
    case KnFcMsgPing:
        status = knFcHandlePing(OutputBuffer, OutputBufferLength, ReturnOutputBufferLength);
        break;

    case KnFcMsgAddWatchRoot:
        status = knFcHandleAddRoot(InputBuffer, hdr.Size);
        break;

    case KnFcMsgClearWatchRoots:
        knFcConfigClearRoots();
        DbgPrint("knFcFlt: watch roots cleared\n");
        status = STATUS_SUCCESS;
        break;

    case KnFcMsgStart:
        status = knFcTrackStart();
        break;

    case KnFcMsgStop:
        knFcTrackStop();
        status = STATUS_SUCCESS;
        break;

    case KnFcMsgAddExclude:
        status = knFcHandleAddExclude(InputBuffer, hdr.Size);
        break;

    case KnFcMsgClearExcludes:
        knFcExcludeClear();
        DbgPrint("knFcFlt: excludes cleared\n");
        status = STATUS_SUCCESS;
        break;

    case KnFcMsgGetStats:
        status = knFcHandleGetStats(OutputBuffer, OutputBufferLength, ReturnOutputBufferLength);
        break;

    case KnFcMsgGetProcessTree:
        status = knFcHandleGetProcessTree(OutputBuffer, OutputBufferLength, ReturnOutputBufferLength);
        break;

    default:
        DbgPrint("knFcFlt: unknown msg type=%u\n", hdr.Type);
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }

    return status;
}
