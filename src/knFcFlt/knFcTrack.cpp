/*
 * knFcTrack.cpp
 * Per-PID tracking. Hash table keyed by PID, chained buckets, guarded
 * by an EX_PUSH_LOCK. Also owns the PsSetCreateProcessNotifyRoutineEx
 * registration and the boot-time snapshot pass.
 *
 * M2 semantics:
 *   - On Start: register Ps notify callback + take a snapshot of
 *     currently running processes, classifying them just like a fresh
 *     create event.
 *   - On Stop: unregister Ps notify, free all entries.
 *   - knFcTrackIsTracked(): O(1)-ish lookup used by PreCreate.
 *
 * Limitations (per Design sec 3):
 *   - Snapshot orphans (parent already exited) cannot be classified
 *     as Child. They become untracked unless their image path matches
 *     a watch root.
 *   - PID reuse: terminated entries are flagged Exited and ignored;
 *     a new entry is created for any new arrival with the same PID.
 */

#include "knFcFlt.h"

#ifndef SystemProcessInformation
#define SystemProcessInformation 5
#endif

typedef struct _KNFC_SYSTEM_PROCESS_INFORMATION
{
    ULONG           NextEntryOffset;
    ULONG           NumberOfThreads;
    LARGE_INTEGER   WorkingSetPrivateSize;
    ULONG           HardFaultCount;
    ULONG           NumberOfThreadsHighWatermark;
    ULONGLONG       CycleTime;
    LARGE_INTEGER   CreateTime;
    LARGE_INTEGER   UserTime;
    LARGE_INTEGER   KernelTime;
    UNICODE_STRING  ImageName;
    KPRIORITY       BasePriority;
    HANDLE          UniqueProcessId;
    HANDLE          InheritedFromUniqueProcessId;
    /* trailing fields intentionally omitted; we never access them */
} KNFC_SYSTEM_PROCESS_INFORMATION;

extern "C"
NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_      ULONG  SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_      ULONG  SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

#define KNFC_TRACK_BUCKETS  256

typedef struct _KNFC_TRACK_ENTRY
{
    LIST_ENTRY      Link;
    HANDLE          Pid;
    HANDLE          ParentPid;
    HANDLE          RootPid;
    ULONG           Flags;          /* KNFC_TRACK_FLAGS */
    UNICODE_STRING  ImagePath;      /* may be empty */
} KNFC_TRACK_ENTRY;

typedef struct _KNFC_TRACK_CTX
{
    EX_PUSH_LOCK    Lock;
    LIST_ENTRY      Buckets[KNFC_TRACK_BUCKETS];
    LONG            EntryCount;
    BOOLEAN         Active;
    BOOLEAN         NotifyRegistered;
} KNFC_TRACK_CTX;

static KNFC_TRACK_CTX g_Track;

/* ---------- deferred EXITED push -----------------------------------
 *
 * The PsSetCreateProcessNotifyRoutineEx callback for process exit runs
 * in the context of the terminating thread (the last thread of the
 * dying process). Even FltSendMessage with no reply touches enough
 * thread state that it returns STATUS_THREAD_IS_TERMINATING (0xc000004b)
 * - the EXITED notification never reaches user mode and the Process
 * Tree shows a ghost ROOT that never goes away.
 *
 * Workaround: stash the event metadata in a list and let a dedicated
 * system thread (running in a healthy thread context) call
 * knFcCommPushProcessEvent on our behalf.
 */
typedef struct _KNFC_EXIT_DEFER
{
    LIST_ENTRY Link;
    HANDLE     Pid;
    HANDLE     Ppid;
    HANDLE     Root;
    ULONG      Flags;
} KNFC_EXIT_DEFER;

typedef struct _KNFC_EXIT_CTX
{
    KSPIN_LOCK   Lock;
    LIST_ENTRY   List;
    KEVENT       Event;
    PETHREAD     Thread;
    volatile BOOLEAN Stop;
    BOOLEAN      Running;
} KNFC_EXIT_CTX;

static KNFC_EXIT_CTX g_ExitDefer;

static VOID NTAPI knFcExitDeferWorker(_In_ PVOID Context);

static VOID
knFcExitDeferEnqueue(
    _In_ HANDLE Pid,
    _In_ HANDLE Ppid,
    _In_ HANDLE Root,
    _In_ ULONG  Flags)
{
    KNFC_EXIT_DEFER* e;
    KIRQL irql;

    if (!g_ExitDefer.Running)
    {
        return;
    }

    e = (KNFC_EXIT_DEFER*)knFcAllocateNonPaged(sizeof(*e));
    if (e == NULL)
    {
        return;
    }
    e->Pid   = Pid;
    e->Ppid  = Ppid;
    e->Root  = Root;
    e->Flags = Flags;

    KeAcquireSpinLock(&g_ExitDefer.Lock, &irql);
    if (!g_ExitDefer.Running || g_ExitDefer.Stop)
    {
        KeReleaseSpinLock(&g_ExitDefer.Lock, irql);
        ExFreePoolWithTag(e, KNFC_POOL_TAG);
        return;
    }
    InsertTailList(&g_ExitDefer.List, &e->Link);
    KeReleaseSpinLock(&g_ExitDefer.Lock, irql);
    KeSetEvent(&g_ExitDefer.Event, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS
knFcExitDeferStart(VOID)
{
    NTSTATUS status;
    HANDLE   threadHandle = NULL;
    OBJECT_ATTRIBUTES oa;

    RtlZeroMemory(&g_ExitDefer, sizeof(g_ExitDefer));
    KeInitializeSpinLock(&g_ExitDefer.Lock);
    InitializeListHead(&g_ExitDefer.List);
    KeInitializeEvent(&g_ExitDefer.Event, NotificationEvent, FALSE);
    g_ExitDefer.Running = TRUE;

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(
        &threadHandle,
        THREAD_ALL_ACCESS,
        &oa,
        NULL, NULL,
        knFcExitDeferWorker,
        NULL);
    if (!NT_SUCCESS(status))
    {
        g_ExitDefer.Running = FALSE;
        return status;
    }
    status = ObReferenceObjectByHandle(
        threadHandle, THREAD_ALL_ACCESS, *PsThreadType,
        KernelMode, (PVOID*)&g_ExitDefer.Thread, NULL);
    ZwClose(threadHandle);
    if (!NT_SUCCESS(status))
    {
        /* Worker thread is already running but we have no ref to wait
         * on. Tell it to exit, best-effort wait, then fail the init so
         * the caller doesn't continue with a phantom worker that would
         * UAF on g_ExitDefer at driver unload. */
        LARGE_INTEGER delay;

        g_ExitDefer.Stop    = TRUE;
        g_ExitDefer.Running = FALSE;
        KeSetEvent(&g_ExitDefer.Event, IO_NO_INCREMENT, FALSE);

        delay.QuadPart = -((LONGLONG)2 * 10000 * 1000);  /* 2 seconds */
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &delay);

        g_ExitDefer.Thread = NULL;
        return status;
    }
    return STATUS_SUCCESS;
}

static VOID
knFcExitDeferStop(VOID)
{
    LIST_ENTRY* le;
    KIRQL irql;

    if (!g_ExitDefer.Running)
    {
        return;
    }
    /* Close the producer door FIRST so no new entry arrives between the
     * thread join and the final drain. knFcExitDeferEnqueue guards on
     * Running, so flipping it before the join is what creates a clean
     * cutoff. */
    KeAcquireSpinLock(&g_ExitDefer.Lock, &irql);
    g_ExitDefer.Stop    = TRUE;
    g_ExitDefer.Running = FALSE;
    KeReleaseSpinLock(&g_ExitDefer.Lock, irql);
    KeSetEvent(&g_ExitDefer.Event, IO_NO_INCREMENT, FALSE);

    if (g_ExitDefer.Thread != NULL)
    {
        (VOID)KeWaitForSingleObject(
            g_ExitDefer.Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_ExitDefer.Thread);
        g_ExitDefer.Thread = NULL;
    }

    /* Drain any leftover entries. */
    KeAcquireSpinLock(&g_ExitDefer.Lock, &irql);
    while (!IsListEmpty(&g_ExitDefer.List))
    {
        le = RemoveHeadList(&g_ExitDefer.List);
        KeReleaseSpinLock(&g_ExitDefer.Lock, irql);
        ExFreePoolWithTag(
            CONTAINING_RECORD(le, KNFC_EXIT_DEFER, Link), KNFC_POOL_TAG);
        KeAcquireSpinLock(&g_ExitDefer.Lock, &irql);
    }
    KeReleaseSpinLock(&g_ExitDefer.Lock, irql);
    /* Running was already cleared at the top of this function. */
}

static VOID NTAPI
knFcExitDeferWorker(_In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    while (!g_ExitDefer.Stop)
    {
        KIRQL irql;
        LIST_ENTRY* le = NULL;

        (VOID)KeWaitForSingleObject(
            &g_ExitDefer.Event, Executive, KernelMode, FALSE, NULL);

        for (;;)
        {
            KNFC_EXIT_DEFER* e;
            NTSTATUS pushSt;

            KeAcquireSpinLock(&g_ExitDefer.Lock, &irql);
            if (IsListEmpty(&g_ExitDefer.List))
            {
                KeClearEvent(&g_ExitDefer.Event);
                KeReleaseSpinLock(&g_ExitDefer.Lock, irql);
                break;
            }
            le = RemoveHeadList(&g_ExitDefer.List);
            KeReleaseSpinLock(&g_ExitDefer.Lock, irql);

            e = CONTAINING_RECORD(le, KNFC_EXIT_DEFER, Link);
            if (!g_ExitDefer.Stop)
            {
                pushSt = knFcCommPushProcessEvent(
                    KNFC_PROC_EVENT_EXITED,
                    e->Pid, e->Ppid, e->Root, e->Flags, NULL);
                DbgPrint(
                    "knFcFlt: deferred EXITED push pid=%llu root=%llu st=0x%08x\n",
                    (ULONGLONG)(ULONG_PTR)e->Pid,
                    (ULONGLONG)(ULONG_PTR)e->Root,
                    pushSt);
            }
            ExFreePoolWithTag(e, KNFC_POOL_TAG);
        }
    }

    DbgPrint("knFcFlt: exit-defer worker exiting\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ----- private helpers ----- */

static ULONG
knFcHashPid(HANDLE Pid)
{
    ULONG_PTR v = (ULONG_PTR)Pid;
    v ^= (v >> 16);
    v *= 0x9E3779B1u;
    v ^= (v >> 13);
    return (ULONG)(v & (KNFC_TRACK_BUCKETS - 1));
}

/* Caller holds lock (any mode). Returns first non-Exited entry for Pid. */
static KNFC_TRACK_ENTRY*
knFcTrackFindLocked(HANDLE Pid)
{
    ULONG h = knFcHashPid(Pid);
    PLIST_ENTRY list = &g_Track.Buckets[h];
    PLIST_ENTRY it;

    for (it = list->Flink; it != list; it = it->Flink)
    {
        KNFC_TRACK_ENTRY* e = CONTAINING_RECORD(it, KNFC_TRACK_ENTRY, Link);
        if (e->Pid == Pid && (e->Flags & KnFcTrackExited) == 0)
        {
            return e;
        }
    }
    return NULL;
}

/* Caller holds exclusive lock. Allocates + inserts. */
static NTSTATUS
knFcTrackInsertLocked(
    HANDLE Pid,
    HANDLE ParentPid,
    HANDLE RootPid,
    ULONG  Flags,
    PCUNICODE_STRING ImagePath)
{
    KNFC_TRACK_ENTRY* entry;
    PWCHAR pathCopy = NULL;
    USHORT pathBytes = 0;

    if (ImagePath != NULL && ImagePath->Buffer != NULL && ImagePath->Length > 0)
    {
        pathBytes = ImagePath->Length;
        pathCopy = (PWCHAR)knFcAllocateNonPaged(pathBytes);
        if (pathCopy == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(pathCopy, ImagePath->Buffer, pathBytes);
    }

    entry = (KNFC_TRACK_ENTRY*)knFcAllocateNonPaged(sizeof(*entry));
    if (entry == NULL)
    {
        if (pathCopy != NULL)
        {
            ExFreePoolWithTag(pathCopy, KNFC_POOL_TAG);
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(entry, sizeof(*entry));
    entry->Pid       = Pid;
    entry->ParentPid = ParentPid;
    entry->RootPid   = RootPid;
    entry->Flags     = Flags;
    entry->ImagePath.Buffer        = pathCopy;
    entry->ImagePath.Length        = pathBytes;
    entry->ImagePath.MaximumLength = pathBytes;

    InsertHeadList(&g_Track.Buckets[knFcHashPid(Pid)], &entry->Link);
    InterlockedIncrement(&g_Track.EntryCount);
    return STATUS_SUCCESS;
}

static VOID
knFcTrackFreeEntry(KNFC_TRACK_ENTRY* entry)
{
    if (entry->ImagePath.Buffer != NULL)
    {
        ExFreePoolWithTag(entry->ImagePath.Buffer, KNFC_POOL_TAG);
    }
    ExFreePoolWithTag(entry, KNFC_POOL_TAG);
}

/* Removes every entry. Caller holds exclusive lock. */
static VOID
knFcTrackPurgeAllLocked(VOID)
{
    ULONG i;
    for (i = 0; i < KNFC_TRACK_BUCKETS; ++i)
    {
        PLIST_ENTRY head = &g_Track.Buckets[i];
        while (!IsListEmpty(head))
        {
            PLIST_ENTRY le = RemoveHeadList(head);
            KNFC_TRACK_ENTRY* e = CONTAINING_RECORD(le, KNFC_TRACK_ENTRY, Link);
            knFcTrackFreeEntry(e);
        }
    }
    g_Track.EntryCount = 0;
}

/* ----- public API ----- */

NTSTATUS
knFcTrackInitialize(VOID)
{
    ULONG i;
    NTSTATUS status;

    RtlZeroMemory(&g_Track, sizeof(g_Track));
    FltInitializePushLock(&g_Track.Lock);
    for (i = 0; i < KNFC_TRACK_BUCKETS; ++i)
    {
        InitializeListHead(&g_Track.Buckets[i]);
    }

    status = knFcExitDeferStart();
    if (!NT_SUCCESS(status))
    {
        /* Treat as fatal: a phantom worker (orphan thread without a ref
         * we can wait on) would BugCheck at driver unload by touching
         * g_ExitDefer after the driver image is gone. */
        DbgPrint("knFcFlt: exit-defer start failed 0x%08x (fatal)\n", status);
        FltDeletePushLock(&g_Track.Lock);
        return status;
    }
    return STATUS_SUCCESS;
}

VOID
knFcTrackUninitialize(VOID)
{
    knFcTrackStop();
    knFcExitDeferStop();
    FltDeletePushLock(&g_Track.Lock);
}

BOOLEAN
knFcTrackIsActive(VOID)
{
    return g_Track.Active;
}

ULONG
knFcTrackGetCount(VOID)
{
    LONG v = g_Track.EntryCount;
    return (v < 0) ? 0u : (ULONG)v;
}

VOID
knFcTrackDump(
    _Out_writes_(MaxEntries) KNFC_PROC* OutArr,
    _In_ ULONG MaxEntries,
    _Out_ PULONG OutCount,
    _Out_ PBOOLEAN OutTruncated)
{
    ULONG written = 0;
    BOOLEAN truncated = FALSE;
    ULONG i;

    *OutCount = 0;
    *OutTruncated = FALSE;

    if (OutArr == NULL || MaxEntries == 0)
    {
        return;
    }

    KeEnterCriticalRegion();
    FltAcquirePushLockShared(&g_Track.Lock);

    for (i = 0; i < KNFC_TRACK_BUCKETS; ++i)
    {
        PLIST_ENTRY head = &g_Track.Buckets[i];
        PLIST_ENTRY it;
        for (it = head->Flink; it != head; it = it->Flink)
        {
            KNFC_TRACK_ENTRY* e = CONTAINING_RECORD(it, KNFC_TRACK_ENTRY, Link);
            if (e->Flags & KnFcTrackExited)
            {
                continue;
            }
            if (written >= MaxEntries)
            {
                truncated = TRUE;
                goto done;
            }

            KNFC_PROC* out = &OutArr[written];
            RtlZeroMemory(out, sizeof(*out));
            out->Pid       = (unsigned long long)(ULONG_PTR)e->Pid;
            out->ParentPid = (unsigned long long)(ULONG_PTR)e->ParentPid;
            out->RootPid   = (unsigned long long)(ULONG_PTR)e->RootPid;
            out->Flags     = e->Flags;

            if (e->ImagePath.Buffer != NULL && e->ImagePath.Length > 0)
            {
                USHORT cch = (USHORT)(e->ImagePath.Length / sizeof(WCHAR));
                if (cch > KNFC_TREE_IMAGE_CHARS)
                {
                    /* Keep the trailing portion - that's where the basename
                     * lives and what the UX wants for display.
                     */
                    USHORT skip = (USHORT)(cch - KNFC_TREE_IMAGE_CHARS);
                    RtlCopyMemory(
                        out->Image,
                        e->ImagePath.Buffer + skip,
                        KNFC_TREE_IMAGE_CHARS * sizeof(WCHAR));
                    out->ImageLenChars = KNFC_TREE_IMAGE_CHARS;
                }
                else
                {
                    RtlCopyMemory(out->Image, e->ImagePath.Buffer, e->ImagePath.Length);
                    out->ImageLenChars = cch;
                }
            }
            ++written;
        }
    }

done:
    FltReleasePushLock(&g_Track.Lock);
    KeLeaveCriticalRegion();

    *OutCount     = written;
    *OutTruncated = truncated;
}

BOOLEAN
knFcTrackIsTracked(_In_ HANDLE Pid, _Out_opt_ HANDLE* RootPid, _Out_opt_ ULONG* Flags)
{
    BOOLEAN found = FALSE;

    if (RootPid != NULL)
    {
        *RootPid = NULL;
    }
    if (Flags != NULL)
    {
        *Flags = 0;
    }

    if (!g_Track.Active || Pid == NULL)
    {
        return FALSE;
    }

    KeEnterCriticalRegion();
    FltAcquirePushLockShared(&g_Track.Lock);
    {
        KNFC_TRACK_ENTRY* e = knFcTrackFindLocked(Pid);
        if (e != NULL)
        {
            if (RootPid != NULL)
            {
                *RootPid = e->RootPid;
            }
            if (Flags != NULL)
            {
                *Flags = e->Flags;
            }
            found = TRUE;
        }
    }
    FltReleasePushLock(&g_Track.Lock);
    KeLeaveCriticalRegion();

    return found;
}

/* ----- process notify callback ----- */

static VOID
knFcClassifyAndInsert(
    HANDLE Pid,
    HANDLE ParentPid,
    PCUNICODE_STRING ImagePath,
    ULONG ExtraFlags)
{
    BOOLEAN isRoot;
    HANDLE parentRoot = NULL;
    BOOLEAN parentTracked = FALSE;
    NTSTATUS status;
    ULONG flags;
    BOOLEAN didInsertRoot  = FALSE;
    BOOLEAN didInsertChild = FALSE;
    HANDLE  emitRoot = NULL;
    ULONG   emitFlags = 0;

    isRoot = (ImagePath != NULL) && knFcConfigPathStartsWithAnyRoot(ImagePath);

    KeEnterCriticalRegion();
    FltAcquirePushLockExclusive(&g_Track.Lock);

    do
    {
        /* If already present (e.g. snapshot then notify), skip. */
        if (knFcTrackFindLocked(Pid) != NULL)
        {
            break;
        }

        if (isRoot)
        {
            flags = KnFcTrackRoot | ExtraFlags;
            status = knFcTrackInsertLocked(Pid, ParentPid, Pid, flags, ImagePath);
            if (NT_SUCCESS(status))
            {
                DbgPrint(
                    "knFcFlt: ROOT  pid=%llu ppid=%llu image=%wZ\n",
                    (ULONGLONG)(ULONG_PTR)Pid,
                    (ULONGLONG)(ULONG_PTR)ParentPid,
                    ImagePath);
                didInsertRoot = TRUE;
                emitRoot = Pid;
                emitFlags = flags;
            }
            break;
        }

        {
            KNFC_TRACK_ENTRY* p = knFcTrackFindLocked(ParentPid);
            parentTracked = (p != NULL);
            if (parentTracked)
            {
                parentRoot = p->RootPid;
            }
        }

        if (parentTracked)
        {
            flags = KnFcTrackChild | ExtraFlags;
            status = knFcTrackInsertLocked(Pid, ParentPid, parentRoot, flags, ImagePath);
            if (NT_SUCCESS(status))
            {
                DbgPrint(
                    "knFcFlt: CHILD pid=%llu ppid=%llu root=%llu image=%wZ\n",
                    (ULONGLONG)(ULONG_PTR)Pid,
                    (ULONGLONG)(ULONG_PTR)ParentPid,
                    (ULONGLONG)(ULONG_PTR)parentRoot,
                    ImagePath);
                didInsertChild = TRUE;
                emitRoot = parentRoot;
                emitFlags = flags;
            }
        }
        else
        {
            UNICODE_STRING emptyImg = { 0, 0, NULL };
            DbgPrint(
                "knFcFlt: classify-miss pid=%llu ppid=%llu image=%wZ  (roots=%lu, parentTracked=%d)\n",
                (ULONGLONG)(ULONG_PTR)Pid,
                (ULONGLONG)(ULONG_PTR)ParentPid,
                ImagePath != NULL ? ImagePath : &emptyImg,
                knFcConfigGetRootCount(),
                (int)parentTracked);
        }
    }
    while (FALSE);

    FltReleasePushLock(&g_Track.Lock);
    KeLeaveCriticalRegion();

    /* Push the create event AFTER releasing the track lock so that user
     * mode receives it the moment the entry becomes visible, without
     * blocking the lock on a kernel-to-user round trip. */
    if (didInsertRoot || didInsertChild)
    {
        NTSTATUS pushSt = knFcCommPushProcessEvent(
            KNFC_PROC_EVENT_CREATED, Pid, ParentPid, emitRoot, emitFlags, ImagePath);
        DbgPrint(
            "knFcFlt: push CREATED pid=%llu root=%llu flags=0x%x st=0x%08x\n",
            (ULONGLONG)(ULONG_PTR)Pid,
            (ULONGLONG)(ULONG_PTR)emitRoot,
            emitFlags,
            pushSt);
    }
}

static VOID
knFcMarkExited(HANDLE Pid)
{
    BOOLEAN didExit = FALSE;
    HANDLE  emitPpid = NULL;
    HANDLE  emitRoot = NULL;
    ULONG   emitFlags = 0;

    KeEnterCriticalRegion();
    FltAcquirePushLockExclusive(&g_Track.Lock);
    {
        KNFC_TRACK_ENTRY* e = knFcTrackFindLocked(Pid);
        if (e != NULL)
        {
            e->Flags |= KnFcTrackExited;
            /* Snapshot tracking metadata before we free the entry so we
             * can push an EXITED notification after releasing the lock.
             */
            emitPpid  = e->ParentPid;
            emitRoot  = e->RootPid;
            emitFlags = e->Flags;
            didExit   = TRUE;

            /* For M2 we can free immediately. M3 will keep entries
             * until in-flight backups release them.
             */
            RemoveEntryList(&e->Link);
            InterlockedDecrement(&g_Track.EntryCount);
            knFcTrackFreeEntry(e);
            DbgPrint("knFcFlt: EXIT  pid=%llu\n", (ULONGLONG)(ULONG_PTR)Pid);
        }
    }
    FltReleasePushLock(&g_Track.Lock);
    KeLeaveCriticalRegion();

    if (didExit)
    {
        /* Cannot call FltSendMessage directly here - we're on the
         * terminating-thread context of the exiting process and the
         * call returns STATUS_THREAD_IS_TERMINATING. Hand the event
         * off to a dedicated system thread instead. */
        knFcExitDeferEnqueue(Pid, emitPpid, emitRoot, emitFlags);
        DbgPrint(
            "knFcFlt: EXIT  pid=%llu root=%llu flags=0x%x (deferred push)\n",
            (ULONGLONG)(ULONG_PTR)Pid,
            (ULONGLONG)(ULONG_PTR)emitRoot,
            emitFlags);
    }
}

static VOID NTAPI
knFcProcessNotify(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    UNREFERENCED_PARAMETER(Process);

    /* Visibility: every callback fires here. If you don't see this
     * line in DbgView, the Ps notify wasn't actually registered.
     */
    DbgPrint("knFcFlt: notify pid=%llu active=%d create=%p\n",
        (ULONGLONG)(ULONG_PTR)ProcessId,
        (int)(g_Track.Active ? 1 : 0),
        CreateInfo);

    if (!g_Track.Active)
    {
        return;
    }

    if (CreateInfo != NULL)
    {
        if (CreateInfo->ImageFileName != NULL)
        {
            DbgPrint("knFcFlt: notify-create pid=%llu ppid=%llu image=%wZ\n",
                (ULONGLONG)(ULONG_PTR)ProcessId,
                (ULONGLONG)(ULONG_PTR)CreateInfo->ParentProcessId,
                CreateInfo->ImageFileName);
        }
        else
        {
            DbgPrint("knFcFlt: notify-create pid=%llu ppid=%llu image=<null>\n",
                (ULONGLONG)(ULONG_PTR)ProcessId,
                (ULONGLONG)(ULONG_PTR)CreateInfo->ParentProcessId);
        }
        knFcClassifyAndInsert(
            ProcessId,
            CreateInfo->ParentProcessId,
            CreateInfo->ImageFileName,
            0);
    }
    else
    {
        knFcMarkExited(ProcessId);
    }
}

/* ----- snapshot ----- */

static NTSTATUS
knFcTrackSnapshotPass(_In_ PVOID Buffer, _In_ BOOLEAN ChildPass, _Out_ PULONG ChangesOut)
{
    KNFC_SYSTEM_PROCESS_INFORMATION* sp = (KNFC_SYSTEM_PROCESS_INFORMATION*)Buffer;
    ULONG changes = 0;
    UNICODE_STRING imagePath = { 0 };

    for (;;)
    {
        HANDLE pid  = sp->UniqueProcessId;
        HANDLE ppid = sp->InheritedFromUniqueProcessId;

        if (pid != NULL)
        {
            BOOLEAN already = knFcTrackIsTracked(pid, NULL, NULL);
            if (!already)
            {
                BOOLEAN parentTracked = ChildPass ? knFcTrackIsTracked(ppid, NULL, NULL) : FALSE;
                NTSTATUS s;

                imagePath.Buffer = NULL;
                imagePath.Length = 0;
                imagePath.MaximumLength = 0;

                s = knFcUtilGetImagePathByPid(pid, &imagePath);
                /* It's OK for image lookup to fail (System, Idle, exited races). */

                if (ChildPass)
                {
                    if (parentTracked)
                    {
                        knFcClassifyAndInsert(pid, ppid,
                            NT_SUCCESS(s) ? &imagePath : NULL,
                            KnFcTrackFromSnap);
                        ++changes;
                    }
                }
                else
                {
                    if (NT_SUCCESS(s)
                        && knFcConfigPathStartsWithAnyRoot(&imagePath))
                    {
                        knFcClassifyAndInsert(pid, ppid, &imagePath, KnFcTrackFromSnap);
                        ++changes;
                    }
                }

                knFcUtilFreeImagePath(&imagePath);
            }
        }

        if (sp->NextEntryOffset == 0)
        {
            break;
        }
        sp = (KNFC_SYSTEM_PROCESS_INFORMATION*)((PUCHAR)sp + sp->NextEntryOffset);
    }

    *ChangesOut = changes;
    return STATUS_SUCCESS;
}

static NTSTATUS
knFcTrackSnapshot(VOID)
{
    NTSTATUS status;
    PVOID buffer = NULL;
    ULONG bufSize = 0;
    ULONG retSize = 0;
    int attempt;
    int iter;

    /* Discover size */
    status = ZwQuerySystemInformation(SystemProcessInformation, NULL, 0, &retSize);
    if (status != STATUS_INFO_LENGTH_MISMATCH)
    {
        /* Some builds return BUFFER_TOO_SMALL */
        if (status != STATUS_BUFFER_TOO_SMALL)
        {
            return status;
        }
    }

    for (attempt = 0; attempt < 5; ++attempt)
    {
        bufSize = retSize + (64 * 1024);  /* pad for races */
        if (buffer != NULL)
        {
            ExFreePoolWithTag(buffer, KNFC_POOL_TAG);
            buffer = NULL;
        }
        buffer = knFcAllocateNonPaged(bufSize);
        if (buffer == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        status = ZwQuerySystemInformation(SystemProcessInformation, buffer, bufSize, &retSize);
        if (NT_SUCCESS(status))
        {
            break;
        }
        if (status != STATUS_INFO_LENGTH_MISMATCH && status != STATUS_BUFFER_TOO_SMALL)
        {
            ExFreePoolWithTag(buffer, KNFC_POOL_TAG);
            return status;
        }
    }
    if (!NT_SUCCESS(status))
    {
        if (buffer != NULL)
        {
            ExFreePoolWithTag(buffer, KNFC_POOL_TAG);
        }
        return status;
    }

    {
        ULONG changes = 0;
        /* Pass 1: roots by image-path prefix */
        knFcTrackSnapshotPass(buffer, FALSE, &changes);
        DbgPrint("knFcFlt: snapshot pass-root changes=%u\n", changes);

        /* Pass 2+: children, iterate until stable (tree depth bound) */
        for (iter = 0; iter < 32; ++iter)
        {
            changes = 0;
            knFcTrackSnapshotPass(buffer, TRUE, &changes);
            DbgPrint("knFcFlt: snapshot pass-child[%d] changes=%u\n", iter, changes);
            if (changes == 0)
            {
                break;
            }
        }
    }

    ExFreePoolWithTag(buffer, KNFC_POOL_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS
knFcTrackStart(VOID)
{
    NTSTATUS status;

    if (g_Track.Active)
    {
        return STATUS_SUCCESS;
    }

    if (!g_Track.NotifyRegistered)
    {
        status = PsSetCreateProcessNotifyRoutineEx(knFcProcessNotify, FALSE);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: PsSetCreateProcessNotifyRoutineEx failed 0x%08x\n", status);
            return status;
        }
        g_Track.NotifyRegistered = TRUE;
    }

    g_Track.Active = TRUE;

    /* Snapshot existing processes after Active=TRUE so that any
     * concurrent create-notify also lands in the table.
     */
    status = knFcTrackSnapshot();
    if (!NT_SUCCESS(status))
    {
        DbgPrint("knFcFlt: snapshot failed 0x%08x (continuing)\n", status);
        /* Non-fatal - go-forward tracking still works. */
        status = STATUS_SUCCESS;
    }

    DbgPrint("knFcFlt: tracking started, entries=%d\n", g_Track.EntryCount);
    return status;
}

VOID
knFcTrackStop(VOID)
{
    g_Track.Active = FALSE;

    if (g_Track.NotifyRegistered)
    {
        /* TRUE = remove the routine */
        (VOID)PsSetCreateProcessNotifyRoutineEx(knFcProcessNotify, TRUE);
        g_Track.NotifyRegistered = FALSE;
    }

    KeEnterCriticalRegion();
    FltAcquirePushLockExclusive(&g_Track.Lock);
    knFcTrackPurgeAllLocked();
    FltReleasePushLock(&g_Track.Lock);
    KeLeaveCriticalRegion();

    DbgPrint("knFcFlt: tracking stopped\n");
}
