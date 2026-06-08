/*
 * knFcCallbacks.cpp
 * Pre/post operation callbacks (M4).
 *
 *   IRP_MJ_CREATE                              Pre  : early skip
 *                                              Post : classify intent, install SHC
 *   IRP_MJ_WRITE                               Post : MODIFIED
 *   IRP_MJ_SET_INFORMATION                     Post : EOF/alloc -> MODIFIED;
 *                                                     rename -> capture new name;
 *                                                     dispo -> DELETE_ON_CLOSE
 *   IRP_MJ_CLEANUP                             Post : pick final name and enqueue
 *   IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION Pre  : writable section -> MODIFIED
 *
 * Section sync rationale (Design sec 5):
 *   Memory-mapped writes do not flow through IRP_MJ_WRITE. The acquire-for-
 *   section callback fires when a section object is being created over the
 *   file; conservative policy: any writable protection (RW / WC / EXEC_RW /
 *   EXEC_WC) flips the MODIFIED flag.
 */

#include "knFcFlt.h"

/* ----- helpers ----- */

static BOOLEAN
knFcIsTrackedCaller(_Out_ HANDLE* RootPid)
{
    HANDLE pid = PsGetCurrentProcessId();
    return knFcTrackIsTracked(pid, RootPid, NULL);
}

static ULONG
knFcDecideWriteIntent(_In_ PFLT_CALLBACK_DATA Data)
{
    ACCESS_MASK access = 0;
    ULONG opts;
    ULONG dispo;
    ULONG flags = 0;

    if (Data->Iopb->Parameters.Create.SecurityContext != NULL)
    {
        access = Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
    }
    opts  = Data->Iopb->Parameters.Create.Options & 0x00FFFFFF;
    dispo = (Data->Iopb->Parameters.Create.Options >> 24) & 0xFF;

    if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES
                  | FILE_WRITE_EA | DELETE | GENERIC_WRITE | GENERIC_ALL
                  | MAXIMUM_ALLOWED))
    {
        flags |= KNFC_SHC_WRITE_INTENT;
    }
    if (dispo == FILE_CREATE
        || dispo == FILE_SUPERSEDE
        || dispo == FILE_OVERWRITE
        || dispo == FILE_OVERWRITE_IF)
    {
        /* Creation/replace itself is a write-modifying event. We mark
         * MODIFIED immediately so that even a zero-byte file that the
         * caller never WriteFile's into is backed up at cleanup time.
         * (Sec M5: user-requested - new-file-and-close path.)
         */
        flags |= KNFC_SHC_WRITE_INTENT | KNFC_SHC_MODIFIED | KNFC_SHC_CREATED;
    }
    if (opts & FILE_DELETE_ON_CLOSE)
    {
        /* Even pure delete-only opens (no WriteFile in between, e.g.
         * ZwDeleteFile through CREATE+DELETE_ON_CLOSE) deserve a backup -
         * the file's pre-delete content is what we want to preserve.
         * Mark MODIFIED so PreCleanup sync-send fires.
         */
        flags |= KNFC_SHC_DELETE_ON_CLOSE | KNFC_SHC_WRITE_INTENT | KNFC_SHC_MODIFIED;
    }
    return flags;
}

static VOID
knFcShcSetOriginalName(_Inout_ PKNFC_SHC Shc, _In_ PCUNICODE_STRING Name)
{
    if (Name == NULL || Name->Buffer == NULL || Name->Length == 0)
    {
        return;
    }
    Shc->OriginalName.Buffer = (PWCH)knFcAllocateNonPaged(Name->Length);
    if (Shc->OriginalName.Buffer == NULL)
    {
        return;
    }
    RtlCopyMemory(Shc->OriginalName.Buffer, Name->Buffer, Name->Length);
    Shc->OriginalName.Length        = Name->Length;
    Shc->OriginalName.MaximumLength = Name->Length;
}

/* ----- PreCreate ----- */

FLT_PREOP_CALLBACK_STATUS
knFcPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    UNREFERENCED_PARAMETER(FltObjects);

    *CompletionContext = NULL;

    if (Data->RequestorMode == KernelMode)
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

/* ----- PostCreate ----- */

FLT_POSTOP_CALLBACK_STATUS
knFcPostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    NTSTATUS status;
    PKNFC_SHC shc = NULL;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    HANDLE rootPid = NULL;
    ULONG  writeFlags;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    if (!NT_SUCCESS(Data->IoStatus.Status))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    if (Data->IoStatus.Status == STATUS_REPARSE)
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (!knFcIsTrackedCaller(&rootPid))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    writeFlags = knFcDecideWriteIntent(Data);

    {
        PFLT_FILE_NAME_INFORMATION ni = NULL;
        UNICODE_STRING emptyName = { 0, 0, NULL };
        PUNICODE_STRING shown = &emptyName;
        NTSTATUS sname = FltGetFileNameInformation(
            Data, FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT, &ni);
        if (NT_SUCCESS(sname))
        {
            shown = &ni->Name;
        }
        DbgPrint("knFcFlt: post-create-trace pid=%llu wflags=0x%x dispo=%u opts=0x%x name=%wZ\n",
            (ULONGLONG)(ULONG_PTR)PsGetCurrentProcessId(),
            writeFlags,
            (ULONG)((Data->Iopb->Parameters.Create.Options >> 24) & 0xFF),
            Data->Iopb->Parameters.Create.Options & 0x00FFFFFF,
            shown);
        if (ni != NULL)
        {
            FltReleaseFileNameInformation(ni);
        }
    }

    if ((writeFlags & KNFC_SHC_WRITE_INTENT) == 0)
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    status = FltAllocateContext(
        g_FilterHandle,
        FLT_STREAMHANDLE_CONTEXT,
        sizeof(KNFC_SHC),
        NonPagedPool,
        (PFLT_CONTEXT*)&shc);
    if (!NT_SUCCESS(status))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    RtlZeroMemory(shc, sizeof(*shc));
    shc->OwnerPid = PsGetCurrentProcessId();
    shc->RootPid  = rootPid;
    shc->Flags    = (LONG)writeFlags;
    FltInitializePushLock(&shc->NameLock);

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);
    if (NT_SUCCESS(status))
    {
        knFcShcSetOriginalName(shc, &nameInfo->Name);
        FltReleaseFileNameInformation(nameInfo);
        nameInfo = NULL;
    }

    status = FltSetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        FLT_SET_CONTEXT_KEEP_IF_EXISTS,
        shc,
        NULL);
    if (!NT_SUCCESS(status) && status != STATUS_FLT_CONTEXT_ALREADY_DEFINED)
    {
        DbgPrint("knFcFlt: SetStreamHandleContext failed 0x%08x\n", status);
    }

    FltReleaseContext(shc);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

/* ----- PostWrite ----- */

FLT_POSTOP_CALLBACK_STATUS
knFcPostWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    NTSTATUS status;
    PKNFC_SHC shc = NULL;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    if (!NT_SUCCESS(Data->IoStatus.Status))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    if (Data->IoStatus.Information == 0)
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&shc);
    if (!NT_SUCCESS(status))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_MODIFIED);
    DbgPrint("knFcFlt: post-write pid=%llu bytes=%llu  -> MODIFIED\n",
        (ULONGLONG)(ULONG_PTR)PsGetCurrentProcessId(),
        (ULONGLONG)Data->IoStatus.Information);
    FltReleaseContext(shc);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

/* ----- PostSetInformation ----- */

FLT_POSTOP_CALLBACK_STATUS
knFcPostSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    NTSTATUS status;
    PKNFC_SHC shc = NULL;
    FILE_INFORMATION_CLASS cls;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    if (!NT_SUCCESS(Data->IoStatus.Status))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&shc);
    if (!NT_SUCCESS(status))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    cls = Data->Iopb->Parameters.SetFileInformation.FileInformationClass;
    switch (cls)
    {
    case FileEndOfFileInformation:
    case FileAllocationInformation:
    case FileValidDataLengthInformation:
        InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_MODIFIED);
        break;

    case FileDispositionInformation:
    case FileDispositionInformationEx:
        InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_DELETE_ON_CLOSE);
        break;

    case FileRenameInformation:
    case FileRenameInformationEx:
    {
        PFLT_FILE_NAME_INFORMATION ni = NULL;
        NTSTATUS s2;

        InterlockedOr(&shc->Flags, (LONG)(KNFC_SHC_RENAMED | KNFC_SHC_MODIFIED));

        /* The FileObject already points at the new path here. */
        s2 = FltGetFileNameInformation(
            Data,
            FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
            &ni);
        if (NT_SUCCESS(s2))
        {
            if (ni->Name.Length > 0)
            {
                knFcShcSetCurrentName(shc, &ni->Name);
            }
            FltReleaseFileNameInformation(ni);
        }
        break;
    }

    default:
        break;
    }

    FltReleaseContext(shc);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

/* ----- PreAcquireForSectionSynchronization ----- */

FLT_PREOP_CALLBACK_STATUS
knFcPreAcquireForSection(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    NTSTATUS status;
    PKNFC_SHC shc = NULL;
    ULONG protect;

    UNREFERENCED_PARAMETER(CompletionContext);

    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&shc);
    if (!NT_SUCCESS(status))
    {
        /* No write-intent SHC -> not from a tracked write open. */
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    protect = Data->Iopb->Parameters.AcquireForSectionSynchronization.PageProtection;
    if (protect & (PAGE_READWRITE | PAGE_WRITECOPY
                   | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
    {
        InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_MODIFIED);
    }

    FltReleaseContext(shc);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

/* ----- final-name selection helper (shared between Pre and PostCleanup) ----- */

static NTSTATUS
knFcResolveFinalName(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PKNFC_SHC Shc,
    _Out_ PFLT_FILE_NAME_INFORMATION* OutNameInfo,
    _Out_ PWCH* OutCopyBuffer,
    _Out_ USHORT* OutCopyLen,
    _Out_ PCUNICODE_STRING* OutFinalName,
    _Out_ PUNICODE_STRING OutSnap)
{
    NTSTATUS status = STATUS_SUCCESS;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PWCH currentCopy = NULL;
    USHORT currentLen = 0;
    PCUNICODE_STRING finalName = NULL;

    *OutNameInfo  = NULL;
    *OutCopyBuffer = NULL;
    *OutCopyLen   = 0;
    *OutFinalName = NULL;
    OutSnap->Buffer = NULL;
    OutSnap->Length = 0;
    OutSnap->MaximumLength = 0;

    /* 1) SHC->CurrentName snapshot (rename-captured). */
    KeEnterCriticalRegion();
    FltAcquirePushLockShared(&Shc->NameLock);
    if (Shc->CurrentName.Length > 0)
    {
        currentLen = Shc->CurrentName.Length;
        currentCopy = (PWCH)knFcAllocateNonPaged(currentLen);
        if (currentCopy != NULL)
        {
            RtlCopyMemory(currentCopy, Shc->CurrentName.Buffer, currentLen);
        }
        else
        {
            currentLen = 0;
        }
    }
    FltReleasePushLock(&Shc->NameLock);
    KeLeaveCriticalRegion();

    if (currentCopy != NULL)
    {
        OutSnap->Buffer        = currentCopy;
        OutSnap->Length        = currentLen;
        OutSnap->MaximumLength = currentLen;
        finalName = OutSnap;
    }

    /* 2) Live query as fallback. */
    if (finalName == NULL)
    {
        status = FltGetFileNameInformation(
            Data,
            FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
            &nameInfo);
        if (NT_SUCCESS(status) && nameInfo->Name.Length > 0)
        {
            finalName = &nameInfo->Name;
        }
    }

    /* 3) OriginalName. */
    if (finalName == NULL && Shc->OriginalName.Length > 0)
    {
        finalName = &Shc->OriginalName;
    }

    *OutNameInfo   = nameInfo;
    *OutCopyBuffer = currentCopy;
    *OutCopyLen    = currentLen;
    *OutFinalName  = finalName;
    return STATUS_SUCCESS;
}

/* ----- PreCleanup -----
 * DELETE_ON_CLOSE files are processed here, BEFORE the file system
 * starts tearing down the FileObject. The driver blocks the caller
 * with a bounded timeout until knFcUxWpf has read the stream. For
 * all other write-modified handles we just fall through to PostCleanup
 * which queues an async backup.
 */
FLT_PREOP_CALLBACK_STATUS
knFcPreCleanup(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    NTSTATUS status;
    PKNFC_SHC shc = NULL;
    LONG snap;

    *CompletionContext = NULL;

    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&shc);
    if (!NT_SUCCESS(status))
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    snap = shc->Flags;
    if ((snap & KNFC_SHC_MODIFIED) == 0
        || (snap & KNFC_SHC_DELETE_ON_CLOSE) == 0)
    {
        /* Hand off to PostCleanup. */
        FltReleaseContext(shc);
        return FLT_PREOP_SUCCESS_WITH_CALLBACK;
    }

    /* DELETE_ON_CLOSE: sync send while the file is still cleanly
     * accessible by another opener (knFcUxWpf with SHARE_RWD).
     */
    {
        PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
        PWCH currentCopy = NULL;
        USHORT currentLen = 0;
        UNICODE_STRING currentSnap;
        PCUNICODE_STRING finalName = NULL;
        ULONGLONG sizeHint = 0;

        knFcResolveFinalName(Data, shc, &nameInfo, &currentCopy, &currentLen,
            &finalName, &currentSnap);

        if (finalName != NULL && !knFcExcludeMatches(finalName))
        {
            FILE_STANDARD_INFORMATION fsi;
            NTSTATUS s2 = FltQueryInformationFile(
                FltObjects->Instance,
                FltObjects->FileObject,
                &fsi,
                sizeof(fsi),
                FileStandardInformation,
                NULL);
            if (NT_SUCCESS(s2))
            {
                sizeHint = (ULONGLONG)fsi.EndOfFile.QuadPart;
            }

            (VOID)knFcQueueSendSyncFromCleanup(
                shc->OwnerPid, shc->RootPid, (ULONG)snap, sizeHint, finalName);
            DbgPrint("knFcFlt: pre-cleanup sync pid=%llu flags=0x%x size=%llu name=%wZ\n",
                (ULONGLONG)(ULONG_PTR)shc->OwnerPid, (ULONG)snap, sizeHint, finalName);

            /* Mark so PostCleanup does not re-process. */
            InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_BACKED_UP);
        }

        if (currentCopy != NULL)
        {
            ExFreePoolWithTag(currentCopy, KNFC_POOL_TAG);
        }
        if (nameInfo != NULL)
        {
            FltReleaseFileNameInformation(nameInfo);
        }
    }

    FltReleaseContext(shc);
    /* No PostCleanup needed; the file is about to disappear. */
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

/* ----- PostCleanup ----- */

FLT_POSTOP_CALLBACK_STATUS
knFcPostCleanup(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    NTSTATUS status;
    PKNFC_SHC shc = NULL;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PWCH currentCopy = NULL;
    USHORT currentLen = 0;
    UNICODE_STRING currentSnap;
    PCUNICODE_STRING finalName = NULL;
    LONG snap;
    ULONGLONG sizeHint = 0;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&shc);
    if (!NT_SUCCESS(status))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    do
    {
        snap = shc->Flags;
        DbgPrint("knFcFlt: post-cleanup pid=%llu flags=0x%x\n",
            (ULONGLONG)(ULONG_PTR)shc->OwnerPid, (ULONG)snap);
        if ((snap & KNFC_SHC_MODIFIED) == 0)
        {
            break;
        }
        if (snap & KNFC_SHC_BACKED_UP)
        {
            /* PreCleanup already handled this (DELETE_ON_CLOSE path). */
            break;
        }

        knFcResolveFinalName(Data, shc, &nameInfo, &currentCopy, &currentLen,
            &finalName, &currentSnap);
        if (finalName == NULL)
        {
            break;
        }

        if (knFcExcludeMatches(finalName))
        {
            DbgPrint("knFcFlt: exclude  pid=%llu name=%wZ\n",
                (ULONGLONG)(ULONG_PTR)shc->OwnerPid, finalName);
            break;
        }

        {
            FILE_STANDARD_INFORMATION fsi;
            NTSTATUS s2 = FltQueryInformationFile(
                FltObjects->Instance,
                FltObjects->FileObject,
                &fsi,
                sizeof(fsi),
                FileStandardInformation,
                NULL);
            if (NT_SUCCESS(s2))
            {
                sizeHint = (ULONGLONG)fsi.EndOfFile.QuadPart;
            }
        }

        (VOID)knFcQueueEnqueue(
            shc->OwnerPid, shc->RootPid, (ULONG)snap, sizeHint, finalName);
        DbgPrint("knFcFlt: enqueue pid=%llu root=%llu flags=0x%x size=%llu name=%wZ\n",
            (ULONGLONG)(ULONG_PTR)shc->OwnerPid,
            (ULONGLONG)(ULONG_PTR)shc->RootPid,
            (ULONG)snap,
            sizeHint,
            finalName);
    }
    while (FALSE);

    if (currentCopy != NULL)
    {
        ExFreePoolWithTag(currentCopy, KNFC_POOL_TAG);
    }
    if (nameInfo != NULL)
    {
        FltReleaseFileNameInformation(nameInfo);
    }
    FltReleaseContext(shc);
    return FLT_POSTOP_FINISHED_PROCESSING;
}
