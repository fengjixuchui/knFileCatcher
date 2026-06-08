/*
 * knFcContexts.cpp
 * StreamHandleContext registration + cleanup + name swap helper.
 *
 * One context per (Instance, FileObject). Lifetime is tied to the handle:
 * FltMgr automatically releases the context on IRP_MJ_CLOSE (or rundown).
 * The Cleanup callback frees pool-allocated name buffers and tears down
 * the NameLock.
 */

#include "knFcFlt.h"

VOID FLTAPI
knFcShcCleanup(_In_ PFLT_CONTEXT Context, _In_ FLT_CONTEXT_TYPE ContextType)
{
    PKNFC_SHC shc = (PKNFC_SHC)Context;

    UNREFERENCED_PARAMETER(ContextType);

    if (shc == NULL)
    {
        return;
    }
    if (shc->OriginalName.Buffer != NULL)
    {
        ExFreePoolWithTag(shc->OriginalName.Buffer, KNFC_POOL_TAG);
        shc->OriginalName.Buffer = NULL;
        shc->OriginalName.Length = 0;
        shc->OriginalName.MaximumLength = 0;
    }
    if (shc->CurrentName.Buffer != NULL)
    {
        ExFreePoolWithTag(shc->CurrentName.Buffer, KNFC_POOL_TAG);
        shc->CurrentName.Buffer = NULL;
        shc->CurrentName.Length = 0;
        shc->CurrentName.MaximumLength = 0;
    }
    FltDeletePushLock(&shc->NameLock);
}

VOID
knFcShcSetCurrentName(_Inout_ PKNFC_SHC Shc, _In_opt_ PCUNICODE_STRING Name)
{
    PWCH newBuf = NULL;
    USHORT newLen = 0;
    PWCH oldBuf = NULL;

    if (Shc == NULL)
    {
        return;
    }

    if (Name != NULL && Name->Buffer != NULL && Name->Length > 0)
    {
        newLen = Name->Length;
        newBuf = (PWCH)knFcAllocateNonPaged(newLen);
        if (newBuf == NULL)
        {
            /* Best effort - leave existing name untouched on OOM. */
            return;
        }
        RtlCopyMemory(newBuf, Name->Buffer, newLen);
    }

    KeEnterCriticalRegion();
    FltAcquirePushLockExclusive(&Shc->NameLock);
    {
        oldBuf = Shc->CurrentName.Buffer;
        Shc->CurrentName.Buffer        = newBuf;
        Shc->CurrentName.Length        = newLen;
        Shc->CurrentName.MaximumLength = newLen;
    }
    FltReleasePushLock(&Shc->NameLock);
    KeLeaveCriticalRegion();

    if (oldBuf != NULL)
    {
        ExFreePoolWithTag(oldBuf, KNFC_POOL_TAG);
    }
}

CONST FLT_CONTEXT_REGISTRATION g_ContextRegistration[] =
{
    {
        FLT_STREAMHANDLE_CONTEXT,
        0,
        knFcShcCleanup,
        sizeof(KNFC_SHC),
        KNFC_POOL_TAG
    },
    { FLT_CONTEXT_END }
};
