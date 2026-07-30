/*
 * knFcCallbacks.cpp
 * Pre/post operation callbacks (M4).
 *
 *   IRP_MJ_CREATE                              Pre  : early skip
 *                                              Post : classify intent, install SHC
 *   IRP_MJ_WRITE                               Pre  : capture SHC reference
 *                                              Post : MODIFIED
 *   IRP_MJ_SET_INFORMATION                     Pre  : capture SHC reference
 *                                              Post : EOF/alloc -> MODIFIED;
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
    shc->DeleteOnCloseAtCreate =
        BooleanFlagOn(Data->Iopb->Parameters.Create.Options, FILE_DELETE_ON_CLOSE);
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

/* ----- PreWrite / PostWrite ----- */

FLT_PREOP_CALLBACK_STATUS
knFcPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    NTSTATUS status;
    PKNFC_SHC shc = NULL;

    UNREFERENCED_PARAMETER(Data);

    *CompletionContext = NULL;

    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&shc);
    if (!NT_SUCCESS(status))
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    *CompletionContext = shc;
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS
knFcPostWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    PKNFC_SHC shc = (PKNFC_SHC)CompletionContext;

    UNREFERENCED_PARAMETER(FltObjects);

    if (shc == NULL)
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (!FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING)
        && NT_SUCCESS(Data->IoStatus.Status)
        && Data->IoStatus.Information != 0)
    {
        InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_MODIFIED);
    }

    FltReleaseContext(shc);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

/* ----- PreSetInformation / PostSetInformation ----- */

#define KNFC_SETINFO_MARK_MODIFIED    0x00000001u
#define KNFC_SETINFO_SET_DELETE       0x00000002u
#define KNFC_SETINFO_CLEAR_DELETE     0x00000004u
#define KNFC_SETINFO_CAPTURE_RENAME   0x00000008u

typedef struct _KNFC_SETINFO_COMPLETION
{
    PKNFC_SHC  Shc;
    ULONG      Actions;
} KNFC_SETINFO_COMPLETION, *PKNFC_SETINFO_COMPLETION;

static LONG
knFcSetInformationExceptionFilter(_In_ NTSTATUS Status)
{
    return FsRtlIsNtstatusExpected(Status)
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH;
}

static ULONG
knFcGetSetInformationActions(_In_ PFLT_CALLBACK_DATA Data)
{
    FILE_INFORMATION_CLASS cls =
        Data->Iopb->Parameters.SetFileInformation.FileInformationClass;
    ULONG length = Data->Iopb->Parameters.SetFileInformation.Length;
    PVOID buffer = Data->Iopb->Parameters.SetFileInformation.InfoBuffer;

    switch (cls)
    {
    case FileEndOfFileInformation:
    case FileAllocationInformation:
    case FileValidDataLengthInformation:
        return KNFC_SETINFO_MARK_MODIFIED;

    case FileDispositionInformation:
        if (buffer != NULL && length >= sizeof(FILE_DISPOSITION_INFORMATION))
        {
            BOOLEAN deleteFile = FALSE;
            __try
            {
                deleteFile = ((PFILE_DISPOSITION_INFORMATION)buffer)->DeleteFile;
            }
            __except(knFcSetInformationExceptionFilter(GetExceptionCode()))
            {
                return 0;
            }
            return deleteFile
                ? KNFC_SETINFO_SET_DELETE
                : KNFC_SETINFO_CLEAR_DELETE;
        }
        return 0;

    case FileDispositionInformationEx:
        if (buffer != NULL && length >= sizeof(FILE_DISPOSITION_INFORMATION_EX))
        {
            ULONG dispositionFlags = 0;
            __try
            {
                dispositionFlags = ((PFILE_DISPOSITION_INFORMATION_EX)buffer)->Flags;
            }
            __except(knFcSetInformationExceptionFilter(GetExceptionCode()))
            {
                return 0;
            }
            return FlagOn(dispositionFlags, FILE_DISPOSITION_DELETE)
                ? KNFC_SETINFO_SET_DELETE
                : KNFC_SETINFO_CLEAR_DELETE;
        }
        return 0;

    case FileRenameInformation:
    case FileRenameInformationEx:
        return KNFC_SETINFO_MARK_MODIFIED | KNFC_SETINFO_CAPTURE_RENAME;

    default:
        return 0;
    }
}

FLT_PREOP_CALLBACK_STATUS
knFcPreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    NTSTATUS status;
    PKNFC_SHC shc = NULL;
    PKNFC_SETINFO_COMPLETION completion = NULL;
    ULONG actions;

    *CompletionContext = NULL;
    actions = knFcGetSetInformationActions(Data);

    if (actions == 0)
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&shc);
    if (!NT_SUCCESS(status))
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    completion = (PKNFC_SETINFO_COMPLETION)knFcAllocateNonPaged(sizeof(*completion));
    if (completion == NULL)
    {
        FltReleaseContext(shc);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    completion->Shc = shc;
    completion->Actions = actions;
    *CompletionContext = completion;
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

static FLT_POSTOP_CALLBACK_STATUS
knFcPostSetInformationSafe(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    PKNFC_SHC shc = (PKNFC_SHC)CompletionContext;
    PFLT_FILE_NAME_INFORMATION ni = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);

    if (shc == NULL)
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &ni);
    if (NT_SUCCESS(status))
    {
        if (ni->Name.Length > 0)
        {
            knFcShcSetCurrentName(shc, &ni->Name);
        }
        FltReleaseFileNameInformation(ni);
    }

    FltReleaseContext(shc);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

FLT_POSTOP_CALLBACK_STATUS
knFcPostSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    PKNFC_SETINFO_COMPLETION completion =
        (PKNFC_SETINFO_COMPLETION)CompletionContext;
    PKNFC_SHC shc;
    ULONG actions;
    FLT_POSTOP_CALLBACK_STATUS postStatus;

    if (completion == NULL)
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    shc = completion->Shc;
    actions = completion->Actions;
    ExFreePoolWithTag(completion, KNFC_POOL_TAG);

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
    {
        FltReleaseContext(shc);
        return FLT_POSTOP_FINISHED_PROCESSING;
    }
    if (!NT_SUCCESS(Data->IoStatus.Status))
    {
        FltReleaseContext(shc);
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (FlagOn(actions, KNFC_SETINFO_MARK_MODIFIED))
    {
        InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_MODIFIED);
    }

    if (FlagOn(actions, KNFC_SETINFO_SET_DELETE))
    {
        InterlockedExchange(&shc->DeleteDispositionObserved, TRUE);
        InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_DELETE_ON_CLOSE);
    }
    else if (FlagOn(actions, KNFC_SETINFO_CLEAR_DELETE)
        && !shc->DeleteOnCloseAtCreate)
    {
        InterlockedExchange(&shc->DeleteDispositionObserved, TRUE);
        InterlockedAnd(&shc->Flags, ~((LONG)KNFC_SHC_DELETE_ON_CLOSE));
    }
    else if (FlagOn(actions, KNFC_SETINFO_CLEAR_DELETE))
    {
        InterlockedExchange(&shc->DeleteDispositionObserved, TRUE);
    }

    if (FlagOn(actions, KNFC_SETINFO_CAPTURE_RENAME))
    {
        InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_RENAMED);
        if (KeGetCurrentIrql() <= APC_LEVEL)
        {
            return knFcPostSetInformationSafe(
                Data,
                FltObjects,
                shc,
                Flags);
        }
        if (FLT_IS_IRP_OPERATION(Data)
            && FltDoCompletionProcessingWhenSafe(
                Data,
                FltObjects,
                shc,
                Flags,
                knFcPostSetInformationSafe,
                &postStatus))
        {
            return postStatus;
        }
        FltReleaseContext(shc);
        return FLT_POSTOP_FINISHED_PROCESSING;
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

/* ----- final-name selection helper ----- */

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

typedef struct _KNFC_CLEANUP_COMPLETION
{
    HANDLE          OwnerPid;
    HANDLE          RootPid;
    ULONG           Flags;
    ULONGLONG       FileSizeHint;
    UNICODE_STRING  Path;
    WCHAR           PathBuffer[1];
} KNFC_CLEANUP_COMPLETION, *PKNFC_CLEANUP_COMPLETION;

static PKNFC_CLEANUP_COMPLETION
knFcAllocateCleanupCompletion(
    _In_ PKNFC_SHC Shc,
    _In_ ULONG Flags,
    _In_ ULONGLONG FileSizeHint,
    _In_ PCUNICODE_STRING Path
    )
{
    SIZE_T total;
    PKNFC_CLEANUP_COMPLETION completion;

    if (Path == NULL || Path->Buffer == NULL || Path->Length == 0)
    {
        return NULL;
    }

    total = FIELD_OFFSET(KNFC_CLEANUP_COMPLETION, PathBuffer) + Path->Length;
    completion = (PKNFC_CLEANUP_COMPLETION)knFcAllocateNonPaged(total);
    if (completion == NULL)
    {
        return NULL;
    }

    RtlZeroMemory(completion, FIELD_OFFSET(KNFC_CLEANUP_COMPLETION, PathBuffer));
    completion->OwnerPid = Shc->OwnerPid;
    completion->RootPid = Shc->RootPid;
    completion->Flags = Flags;
    completion->FileSizeHint = FileSizeHint;
    completion->Path.Buffer = completion->PathBuffer;
    completion->Path.Length = Path->Length;
    completion->Path.MaximumLength = Path->Length;
    RtlCopyMemory(completion->PathBuffer, Path->Buffer, Path->Length);
    return completion;
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
    PKNFC_CLEANUP_COMPLETION completion = NULL;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PWCH currentCopy = NULL;
    USHORT currentLen = 0;
    UNICODE_STRING currentSnap;
    PCUNICODE_STRING finalName = NULL;
    ULONGLONG sizeHint = 0;
    BOOLEAN passiveSafe;
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
    if ((snap & KNFC_SHC_BACKED_UP) != 0)
    {
        FltReleaseContext(shc);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    passiveSafe =
        (KeGetCurrentIrql() == PASSIVE_LEVEL && !KeAreAllApcsDisabled());

    if ((snap & (KNFC_SHC_MODIFIED | KNFC_SHC_DELETE_ON_CLOSE)) == 0
        && InterlockedCompareExchange(
            &shc->DeleteDispositionObserved, 0, 0) == 0)
    {
        FltReleaseContext(shc);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    knFcResolveFinalName(Data, shc, &nameInfo, &currentCopy, &currentLen,
        &finalName, &currentSnap);

    if (finalName == NULL)
    {
        goto Cleanup;
    }

    if (passiveSafe && knFcExcludeMatches(finalName))
    {
        goto Cleanup;
    }

    if (passiveSafe && IoGetTopLevelIrp() == NULL)
    {
        FILE_STANDARD_INFORMATION fsi;
        NTSTATUS queryStatus = FltQueryInformationFile(
            FltObjects->Instance,
            FltObjects->FileObject,
            &fsi,
            sizeof(fsi),
            FileStandardInformation,
            NULL);
        if (NT_SUCCESS(queryStatus))
        {
            sizeHint = (ULONGLONG)fsi.EndOfFile.QuadPart;
            if (InterlockedCompareExchange(
                &shc->DeleteDispositionObserved, 0, 0) != 0)
            {
                if (fsi.DeletePending)
                {
                    snap |= KNFC_SHC_DELETE_ON_CLOSE;
                }
                else
                {
                    snap &= ~((LONG)KNFC_SHC_DELETE_ON_CLOSE);
                }
            }
        }
    }

    if ((snap & (KNFC_SHC_MODIFIED | KNFC_SHC_DELETE_ON_CLOSE)) == 0)
    {
        goto Cleanup;
    }

    if ((snap & KNFC_SHC_DELETE_ON_CLOSE) != 0)
    {
        if (passiveSafe)
        {
            (VOID)knFcQueueSendSyncFromCleanup(
                shc->OwnerPid, shc->RootPid, (ULONG)snap, sizeHint, finalName);
            DbgPrint("knFcFlt: pre-cleanup sync pid=%llu flags=0x%x size=%llu name=%wZ\n",
                (ULONGLONG)(ULONG_PTR)shc->OwnerPid,
                (ULONG)snap,
                sizeHint,
                finalName);
            InterlockedOr(&shc->Flags, (LONG)KNFC_SHC_BACKED_UP);
        }
        else
        {
            (VOID)knFcQueueEnqueue(
                shc->OwnerPid, shc->RootPid, (ULONG)snap, sizeHint, finalName);
            DbgPrint("knFcFlt: pre-cleanup elevated fallback pid=%llu flags=0x%x\n",
                (ULONGLONG)(ULONG_PTR)shc->OwnerPid,
                (ULONG)snap);
        }
        goto Cleanup;
    }

    completion = knFcAllocateCleanupCompletion(
        shc, (ULONG)snap, sizeHint, finalName);

Cleanup:
    if (currentCopy != NULL)
    {
        ExFreePoolWithTag(currentCopy, KNFC_POOL_TAG);
    }
    if (nameInfo != NULL)
    {
        FltReleaseFileNameInformation(nameInfo);
    }
    FltReleaseContext(shc);

    if (completion != NULL)
    {
        *CompletionContext = completion;
        return FLT_PREOP_SUCCESS_WITH_CALLBACK;
    }

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
    PKNFC_CLEANUP_COMPLETION completion =
        (PKNFC_CLEANUP_COMPLETION)CompletionContext;

    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);

    if (completion != NULL)
    {
        if (!FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
        {
            (VOID)knFcQueueEnqueue(
                completion->OwnerPid,
                completion->RootPid,
                completion->Flags,
                completion->FileSizeHint,
                &completion->Path);
        }
        ExFreePoolWithTag(completion, KNFC_POOL_TAG);
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}
