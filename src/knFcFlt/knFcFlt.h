/*
 * knFcFlt.h
 * Internal header for the knFcFlt minifilter driver (M5).
 */

#pragma once

#include <fltKernel.h>
#include <ntddk.h>

#include "../common/knFcProto.h"

#define KNFC_POOL_TAG   'cFnk'
#define KNFC_POOL_FLAG_UNINITIALIZED   0x0000000000000002ULL
#define KNFC_POOL_FLAG_NON_PAGED       0x0000000000000040ULL

typedef PVOID (NTAPI* KNFC_EX_ALLOCATE_POOL2_FN)(
    _In_ ULONG64 Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

/* ----- knFcFlt.cpp ----- */
extern PFLT_FILTER  g_FilterHandle;
extern KNFC_EX_ALLOCATE_POOL2_FN g_KnFcExAllocatePool2;

VOID knFcPoolInitialize(VOID);

_IRQL_requires_max_(DISPATCH_LEVEL)
static __forceinline PVOID
knFcAllocateNonPaged(_In_ SIZE_T NumberOfBytes)
{
    PVOID ptr;

    if (NumberOfBytes == 0)
    {
        return NULL;
    }

    if (g_KnFcExAllocatePool2 != NULL)
    {
        return g_KnFcExAllocatePool2(
            KNFC_POOL_FLAG_NON_PAGED | KNFC_POOL_FLAG_UNINITIALIZED,
            NumberOfBytes,
            KNFC_POOL_TAG);
    }

#pragma warning(push)
#pragma warning(disable: 4996)
    ptr = ExAllocatePoolWithTag(NonPagedPoolNx, NumberOfBytes, KNFC_POOL_TAG);
#pragma warning(pop)
    return ptr;
}

/* ----- knFcComm.cpp ----- */
NTSTATUS knFcCommInitialize(_In_ PFLT_FILTER Filter);
VOID     knFcCommUninitialize(VOID);

NTSTATUS knFcCommSendMessage(
    _In_ PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_writes_bytes_opt_(*ReplyLength) PVOID Reply,
    _Inout_opt_ PULONG ReplyLength,
    _In_opt_ PLARGE_INTEGER Timeout
    );

/* Push a KnFcMsgProcessEvent and wait for a small ack reply. Used
 * from the Ps notify path or the deferred exit worker so user mode
 * sees create/exit transitions promptly without fltmgr replay.
 */
NTSTATUS knFcCommPushProcessEvent(
    _In_ unsigned int EventType,
    _In_ HANDLE Pid,
    _In_ HANDLE ParentPid,
    _In_ HANDLE RootPid,
    _In_ ULONG  Flags,
    _In_opt_ PCUNICODE_STRING Image
    );

/* ----- knFcCallbacks.cpp ----- */
FLT_PREOP_CALLBACK_STATUS knFcPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

FLT_PREOP_CALLBACK_STATUS knFcPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

FLT_PREOP_CALLBACK_STATUS knFcPreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

FLT_PREOP_CALLBACK_STATUS knFcPreAcquireForSection(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

FLT_POSTOP_CALLBACK_STATUS knFcPostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    );

FLT_POSTOP_CALLBACK_STATUS knFcPostWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    );

FLT_POSTOP_CALLBACK_STATUS knFcPostSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    );

FLT_POSTOP_CALLBACK_STATUS knFcPostCleanup(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    );

FLT_PREOP_CALLBACK_STATUS knFcPreCleanup(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

/* ----- knFcContexts.cpp ----- */
typedef struct _KNFC_SHC
{
    HANDLE          OwnerPid;
    HANDLE          RootPid;
    volatile LONG   Flags;
    volatile LONG   DeleteDispositionObserved;
    BOOLEAN         DeleteOnCloseAtCreate;
    EX_PUSH_LOCK    NameLock;
    UNICODE_STRING  OriginalName;
    UNICODE_STRING  CurrentName;
} KNFC_SHC, *PKNFC_SHC;

extern CONST FLT_CONTEXT_REGISTRATION g_ContextRegistration[];
VOID FLTAPI knFcShcCleanup(_In_ PFLT_CONTEXT Context, _In_ FLT_CONTEXT_TYPE ContextType);
VOID        knFcShcSetCurrentName(_Inout_ PKNFC_SHC Shc, _In_opt_ PCUNICODE_STRING Name);

/* ----- knFcConfig.cpp ----- */
NTSTATUS knFcConfigInitialize(VOID);
VOID     knFcConfigUninitialize(VOID);
NTSTATUS knFcConfigAddRoot(_In_ PCUNICODE_STRING Path);
VOID     knFcConfigClearRoots(VOID);
BOOLEAN  knFcConfigPathStartsWithAnyRoot(_In_ PCUNICODE_STRING Path);
ULONG    knFcConfigGetRootCount(VOID);

/* ----- knFcExclude.cpp (M5) ----- */
NTSTATUS knFcExcludeInitialize(VOID);
VOID     knFcExcludeUninitialize(VOID);
NTSTATUS knFcExcludeAdd(_In_ PCUNICODE_STRING Pattern);
VOID     knFcExcludeClear(VOID);
BOOLEAN  knFcExcludeMatches(_In_ PCUNICODE_STRING Path);
ULONG    knFcExcludeGetCount(VOID);
ULONGLONG knFcExcludeGetMatchedCount(VOID);

/* ----- knFcTrack.cpp ----- */
typedef enum _KNFC_TRACK_FLAGS
{
    KnFcTrackRoot       = 0x0001,
    KnFcTrackChild      = 0x0002,
    KnFcTrackExited     = 0x0004,
    KnFcTrackFromSnap   = 0x0008
} KNFC_TRACK_FLAGS;

NTSTATUS knFcTrackInitialize(VOID);
VOID     knFcTrackUninitialize(VOID);
NTSTATUS knFcTrackStart(VOID);
VOID     knFcTrackStop(VOID);
BOOLEAN  knFcTrackIsTracked(_In_ HANDLE Pid, _Out_opt_ HANDLE* RootPid, _Out_opt_ ULONG* Flags);
BOOLEAN  knFcTrackIsActive(VOID);
ULONG    knFcTrackGetCount(VOID);

/* Snapshot the tracking table into a caller-supplied KNFC_PROC array.
 * Fills up to MaxEntries; *OutTruncated = nonzero if there were more.
 */
VOID     knFcTrackDump(
    _Out_writes_to_(MaxEntries, *OutCount) KNFC_PROC* OutArr,
    _In_ ULONG MaxEntries,
    _Out_ PULONG OutCount,
    _Out_ PBOOLEAN OutTruncated);

/* ----- knFcUtil.cpp ----- */
NTSTATUS knFcUtilGetImagePathByPid(_In_ HANDLE Pid, _Out_ PUNICODE_STRING OutPath);
VOID     knFcUtilFreeImagePath(_Inout_ PUNICODE_STRING InOutPath);

/* ----- knFcQueue.cpp ----- */
NTSTATUS knFcQueueInitialize(VOID);
VOID     knFcQueueUninitialize(VOID);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS knFcQueueEnqueue(
    _In_ HANDLE OwnerPid,
    _In_ HANDLE RootPid,
    _In_ ULONG  Flags,
    _In_ ULONGLONG FileSizeHint,
    _In_ PCUNICODE_STRING Path
    );

/* Synchronous variant used by PreCleanup for DELETE_ON_CLOSE files.
 * Blocks the caller until the user-mode app replies or the dynamic
 * size-aware timeout elapses. Same caller constraints as
 * knFcCommSendMessage (PASSIVE_LEVEL).
 */
NTSTATUS knFcQueueSendSyncFromCleanup(
    _In_ HANDLE OwnerPid,
    _In_ HANDLE RootPid,
    _In_ ULONG  Flags,
    _In_ ULONGLONG FileSizeHint,
    _In_ PCUNICODE_STRING Path
    );

VOID knFcQueueGetStats(
    _Out_ PULONG QueueDepth,
    _Out_ PULONGLONG Sent,
    _Out_ PULONGLONG SendFailed,
    _Out_ PULONGLONG Dropped,
    _Out_ PULONGLONG SyncSent,
    _Out_ PULONGLONG SyncFailed
    );
