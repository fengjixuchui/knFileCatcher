/*
 * knFcQueue.cpp
 * Backup request queue + fixed system worker pool.
 *
 * Producers: PostCleanup callback (PASSIVE_LEVEL) via knFcQueueEnqueue.
 * Consumers: KNFC_QUEUE_WORKER_COUNT system threads dequeue and call
 *            knFcCommSendMessage (which wraps FltSendMessage) with a
 *            bounded wait time. User mode has exactly one port reader.
 *
 * Design notes:
 *   - Multiple kernel workers can have FltSendMessage calls in flight,
 *     but only one C# reader drains FilterGetMessage to avoid fltmgr
 *     redelivery on the shared port.
 *   - On queue full, the item is dropped and a counter is bumped. The
 *     producer is not stalled on a tracked process cleanup path.
 *   - On Stop, workers drain and free remaining items without sending.
 */

#include "knFcFlt.h"

#define KNFC_QUEUE_MAX_DEPTH        16384       /* M8: was 4096 */
#define KNFC_QUEUE_WORKER_COUNT     4           /* M6: was 1 */

/* Async-path reply timeout: 10 seconds per item. */
#define KNFC_REPLY_TIMEOUT_100NS    (-((LONGLONG)10 * 10000000))

/* Sync-cleanup timeout grows with hinted file size so a multi-GB
 * DELETE_ON_CLOSE file gets enough budget for the user-mode reader to
 * drain a full read+write. Base 10s + 1s per 50 MB, capped at 60s.
 */
#define KNFC_SYNC_BASE_100NS        ((LONGLONG)10 * 10000000)
#define KNFC_SYNC_PER_50MB_100NS    ((LONGLONG)1  * 10000000)
#define KNFC_SYNC_MAX_EXTRA_SEC     50

typedef struct _KNFC_QUEUE_ITEM
{
    LIST_ENTRY      Link;
    ULONGLONG       RequestId;
    HANDLE          OwnerPid;
    HANDLE          RootPid;
    ULONG           Flags;
    ULONGLONG       FileSizeHint;
    UNICODE_STRING  Path;       /* Buffer points to tail of this allocation */
} KNFC_QUEUE_ITEM;

typedef struct _KNFC_QUEUE
{
    KSPIN_LOCK      Lock;
    LIST_ENTRY      List;
    KEVENT          Event;
    PVOID           Threads[KNFC_QUEUE_WORKER_COUNT];
    ULONG           ThreadCount;
    volatile LONG   Depth;
    volatile LONG   Dropped;
    volatile LONG64 Sent;
    volatile LONG64 SendFailed;
    volatile LONG64 SyncSent;
    volatile LONG64 SyncFailed;
    LONG64          NextId;
    BOOLEAN         Stop;
} KNFC_QUEUE;

static KNFC_QUEUE g_Queue;

static VOID knFcQueueWorker(_In_ PVOID Context);

NTSTATUS
knFcQueueInitialize(VOID)
{
    NTSTATUS status = STATUS_SUCCESS;
    OBJECT_ATTRIBUTES oa;
    ULONG i;

    RtlZeroMemory(&g_Queue, sizeof(g_Queue));
    KeInitializeSpinLock(&g_Queue.Lock);
    InitializeListHead(&g_Queue.List);
    KeInitializeEvent(&g_Queue.Event, NotificationEvent, FALSE);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    for (i = 0; i < KNFC_QUEUE_WORKER_COUNT; ++i)
    {
        HANDLE threadHandle = NULL;
        status = PsCreateSystemThread(
            &threadHandle,
            THREAD_ALL_ACCESS,
            &oa,
            NULL,
            NULL,
            knFcQueueWorker,
            NULL);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: worker[%lu] PsCreateSystemThread failed 0x%08x\n", i, status);
            break;
        }
        status = ObReferenceObjectByHandle(
            threadHandle,
            THREAD_ALL_ACCESS,
            *PsThreadType,
            KernelMode,
            &g_Queue.Threads[i],
            NULL);
        ZwClose(threadHandle);
        if (!NT_SUCCESS(status))
        {
            /* Thread is running but we cannot reference it. Signal
             * stop, best-effort wait, then fail the init - without the
             * delay knFcQueueUninitialize would skip this slot (it
             * loops only up to ThreadCount) and the orphan worker
             * would UAF on g_Queue at driver unload.
             */
            LARGE_INTEGER delay;

            DbgPrint("knFcFlt: worker[%lu] Ob ref failed 0x%08x\n", i, status);
            g_Queue.Threads[i] = NULL;
            g_Queue.Stop = TRUE;
            KeSetEvent(&g_Queue.Event, IO_NO_INCREMENT, FALSE);

            delay.QuadPart = -((LONGLONG)2 * 10000 * 1000);  /* 2 seconds */
            (VOID)KeDelayExecutionThread(KernelMode, FALSE, &delay);
            break;
        }
        ++g_Queue.ThreadCount;
    }

    if (!NT_SUCCESS(status))
    {
        /* Best-effort wait for whatever threads we did start. */
        knFcQueueUninitialize();
        return status;
    }

    DbgPrint("knFcFlt: queue workers started (count=%lu)\n", g_Queue.ThreadCount);
    return STATUS_SUCCESS;
}

VOID
knFcQueueUninitialize(VOID)
{
    ULONG i;

    g_Queue.Stop = TRUE;
    KeSetEvent(&g_Queue.Event, IO_NO_INCREMENT, FALSE);

    /* Wait for every worker we successfully created. The set is small
     * (<=4) so per-thread KeWaitForSingleObject is fine - it sidesteps
     * the 64-object cap of KeWaitForMultipleObjects without a stack
     * buffer.
     */
    for (i = 0; i < g_Queue.ThreadCount; ++i)
    {
        if (g_Queue.Threads[i] != NULL)
        {
            (VOID)KeWaitForSingleObject(
                g_Queue.Threads[i], Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(g_Queue.Threads[i]);
            g_Queue.Threads[i] = NULL;
        }
    }
    g_Queue.ThreadCount = 0;

    /* Drain anything left. */
    for (;;)
    {
        KIRQL irql;
        PLIST_ENTRY le = NULL;

        KeAcquireSpinLock(&g_Queue.Lock, &irql);
        if (!IsListEmpty(&g_Queue.List))
        {
            le = RemoveHeadList(&g_Queue.List);
        }
        KeReleaseSpinLock(&g_Queue.Lock, irql);

        if (le == NULL)
        {
            break;
        }
        ExFreePoolWithTag(CONTAINING_RECORD(le, KNFC_QUEUE_ITEM, Link), KNFC_POOL_TAG);
    }

    DbgPrint("knFcFlt: queue uninit  sent=%lld failed=%lld dropped=%d sync_sent=%lld sync_fail=%lld\n",
        g_Queue.Sent, g_Queue.SendFailed, g_Queue.Dropped,
        g_Queue.SyncSent, g_Queue.SyncFailed);
}

NTSTATUS
knFcQueueEnqueue(
    _In_ HANDLE OwnerPid,
    _In_ HANDLE RootPid,
    _In_ ULONG  Flags,
    _In_ ULONGLONG FileSizeHint,
    _In_ PCUNICODE_STRING Path
    )
{
    SIZE_T total;
    USHORT pathBytes;
    KNFC_QUEUE_ITEM* item;
    KIRQL irql;
    LONG depth;

    if (Path == NULL || Path->Buffer == NULL || Path->Length == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (g_Queue.Stop)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    depth = InterlockedIncrement(&g_Queue.Depth);
    if (depth > KNFC_QUEUE_MAX_DEPTH)
    {
        InterlockedDecrement(&g_Queue.Depth);
        InterlockedIncrement(&g_Queue.Dropped);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pathBytes = Path->Length;
    total = sizeof(KNFC_QUEUE_ITEM) + pathBytes;

    item = (KNFC_QUEUE_ITEM*)knFcAllocateNonPaged(total);
    if (item == NULL)
    {
        InterlockedDecrement(&g_Queue.Depth);
        InterlockedIncrement(&g_Queue.Dropped);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(item, sizeof(*item));
    item->RequestId    = (ULONGLONG)InterlockedIncrement64(&g_Queue.NextId);
    item->OwnerPid     = OwnerPid;
    item->RootPid      = RootPid;
    item->Flags        = Flags;
    item->FileSizeHint = FileSizeHint;
    item->Path.Buffer        = (PWCH)(item + 1);
    item->Path.Length        = pathBytes;
    item->Path.MaximumLength = pathBytes;
    RtlCopyMemory(item->Path.Buffer, Path->Buffer, pathBytes);

    KeAcquireSpinLock(&g_Queue.Lock, &irql);
    InsertTailList(&g_Queue.List, &item->Link);
    KeReleaseSpinLock(&g_Queue.Lock, irql);

    KeSetEvent(&g_Queue.Event, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

static VOID
knFcQueueSendOne(_In_ KNFC_QUEUE_ITEM* Item)
{
    NTSTATUS status;
    ULONG msgLen;
    KNFC_BACKUP_REQUEST* req;
    KNFC_BACKUP_REPLY reply;
    ULONG replyLen;
    LARGE_INTEGER timeout;
    PVOID buf;

    msgLen = (ULONG)(sizeof(KNFC_BACKUP_REQUEST) + Item->Path.Length);
    if (msgLen < sizeof(KNFC_BACKUP_REQUEST))
    {
        InterlockedIncrement64(&g_Queue.SendFailed);
        return;
    }

    buf = knFcAllocateNonPaged(msgLen);
    if (buf == NULL)
    {
        InterlockedIncrement64(&g_Queue.SendFailed);
        return;
    }

    req = (KNFC_BACKUP_REQUEST*)buf;
    RtlZeroMemory(req, sizeof(*req));
    req->Header.Type      = (unsigned int)KnFcMsgBackupRequest;
    req->Header.Size      = msgLen;
    req->RequestId        = Item->RequestId;
    req->OwnerPid         = (unsigned long long)(ULONG_PTR)Item->OwnerPid;
    req->RootPid          = (unsigned long long)(ULONG_PTR)Item->RootPid;
    req->FileSizeHint     = Item->FileSizeHint;
    req->Flags            = Item->Flags;
    req->PathLengthBytes  = Item->Path.Length;
    RtlCopyMemory((PUCHAR)buf + sizeof(*req), Item->Path.Buffer, Item->Path.Length);

    RtlZeroMemory(&reply, sizeof(reply));
    replyLen = sizeof(reply);
    timeout.QuadPart = KNFC_REPLY_TIMEOUT_100NS;

    status = knFcCommSendMessage(buf, msgLen, &reply, &replyLen, &timeout);

    if (NT_SUCCESS(status))
    {
        if (reply.Status == 0)
        {
            InterlockedIncrement64(&g_Queue.Sent);
        }
        else
        {
            InterlockedIncrement64(&g_Queue.SendFailed);
            DbgPrint("knFcFlt: backup id=%llu user-status=%u path=%wZ\n",
                Item->RequestId, reply.Status, &Item->Path);
        }
    }
    else
    {
        InterlockedIncrement64(&g_Queue.SendFailed);
        DbgPrint("knFcFlt: backup id=%llu send-status=0x%08x path=%wZ\n",
            Item->RequestId, status, &Item->Path);
    }

    ExFreePoolWithTag(buf, KNFC_POOL_TAG);
}

static VOID
knFcQueueWorker(_In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    while (TRUE)
    {
        KIRQL irql;
        PLIST_ENTRY le = NULL;

        if (g_Queue.Stop)
        {
            break;
        }

        KeWaitForSingleObject(&g_Queue.Event, Executive, KernelMode, FALSE, NULL);

        for (;;)
        {
            le = NULL;

            KeAcquireSpinLock(&g_Queue.Lock, &irql);
            if (!IsListEmpty(&g_Queue.List))
            {
                le = RemoveHeadList(&g_Queue.List);
                InterlockedDecrement(&g_Queue.Depth);
            }
            else
            {
                /* Empty - clear event so next Set wakes us. */
                KeClearEvent(&g_Queue.Event);
            }
            KeReleaseSpinLock(&g_Queue.Lock, irql);

            if (le == NULL)
            {
                break;
            }
            if (g_Queue.Stop)
            {
                ExFreePoolWithTag(CONTAINING_RECORD(le, KNFC_QUEUE_ITEM, Link), KNFC_POOL_TAG);
                continue;
            }

            {
                KNFC_QUEUE_ITEM* item = CONTAINING_RECORD(le, KNFC_QUEUE_ITEM, Link);
                knFcQueueSendOne(item);
                ExFreePoolWithTag(item, KNFC_POOL_TAG);
            }
        }
    }

    DbgPrint("knFcFlt: queue worker exiting\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
knFcQueueSendSyncFromCleanup(
    _In_ HANDLE OwnerPid,
    _In_ HANDLE RootPid,
    _In_ ULONG  Flags,
    _In_ ULONGLONG FileSizeHint,
    _In_ PCUNICODE_STRING Path
    )
{
    NTSTATUS status;
    ULONG msgLen;
    PVOID buf = NULL;
    KNFC_BACKUP_REQUEST* req;
    KNFC_BACKUP_REPLY reply;
    ULONG replyLen;
    LARGE_INTEGER timeout;

    if (Path == NULL || Path->Buffer == NULL || Path->Length == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (g_Queue.Stop)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    msgLen = (ULONG)(sizeof(KNFC_BACKUP_REQUEST) + Path->Length);
    buf = knFcAllocateNonPaged(msgLen);
    if (buf == NULL)
    {
        InterlockedIncrement64(&g_Queue.SyncFailed);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    req = (KNFC_BACKUP_REQUEST*)buf;
    RtlZeroMemory(req, sizeof(*req));
    req->Header.Type      = (unsigned int)KnFcMsgBackupRequest;
    req->Header.Size      = msgLen;
    req->RequestId        = (ULONGLONG)InterlockedIncrement64(&g_Queue.NextId);
    req->OwnerPid         = (unsigned long long)(ULONG_PTR)OwnerPid;
    req->RootPid          = (unsigned long long)(ULONG_PTR)RootPid;
    req->FileSizeHint     = FileSizeHint;
    req->Flags            = Flags;
    req->PathLengthBytes  = Path->Length;
    RtlCopyMemory((PUCHAR)buf + sizeof(*req), Path->Buffer, Path->Length);

    RtlZeroMemory(&reply, sizeof(reply));
    replyLen = sizeof(reply);

    {
        LONGLONG extraSec = (LONGLONG)(FileSizeHint / (50ULL * 1024 * 1024));
        if (extraSec > KNFC_SYNC_MAX_EXTRA_SEC)
        {
            extraSec = KNFC_SYNC_MAX_EXTRA_SEC;
        }
        timeout.QuadPart = -(KNFC_SYNC_BASE_100NS + extraSec * KNFC_SYNC_PER_50MB_100NS);
    }

    status = knFcCommSendMessage(buf, msgLen, &reply, &replyLen, &timeout);

    if (NT_SUCCESS(status) && reply.Status == 0)
    {
        InterlockedIncrement64(&g_Queue.SyncSent);
    }
    else
    {
        InterlockedIncrement64(&g_Queue.SyncFailed);
        DbgPrint("knFcFlt: sync-cleanup backup id=%llu sendNt=0x%08x userStatus=%u path=%wZ\n",
            req->RequestId,
            (ULONG)status,
            NT_SUCCESS(status) ? reply.Status : 0u,
            Path);

        /* Best-effort fallback: hand the request off to the async queue
         * so the worker pool gets one more chance. If the file has
         * already disappeared (cleanup completed), the async copy will
         * fail visibly in the manifest. Either way the user sees the
         * attempt instead of a silent loss.
         */
        (VOID)knFcQueueEnqueue(OwnerPid, RootPid, Flags, FileSizeHint, Path);
    }

    ExFreePoolWithTag(buf, KNFC_POOL_TAG);
    return status;
}

VOID
knFcQueueGetStats(
    _Out_ PULONG QueueDepth,
    _Out_ PULONGLONG Sent,
    _Out_ PULONGLONG SendFailed,
    _Out_ PULONGLONG Dropped,
    _Out_ PULONGLONG SyncSent,
    _Out_ PULONGLONG SyncFailed
    )
{
    LONG d = g_Queue.Depth;
    *QueueDepth = (d < 0) ? 0u : (ULONG)d;
    *Sent       = (ULONGLONG)g_Queue.Sent;
    *SendFailed = (ULONGLONG)g_Queue.SendFailed;
    *Dropped    = (ULONGLONG)(LONG64)g_Queue.Dropped;
    *SyncSent   = (ULONGLONG)g_Queue.SyncSent;
    *SyncFailed = (ULONGLONG)g_Queue.SyncFailed;
}
