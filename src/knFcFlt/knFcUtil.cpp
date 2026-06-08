/*
 * knFcUtil.cpp
 * Kernel helpers. M2 only needs PID -> NT image path resolution
 * for the boot-time snapshot pass.
 */

#include "knFcFlt.h"

/* ProcessImageFileName returns a UNICODE_STRING followed by the
 * path data in the same buffer. Value is 27.
 */
#ifndef ProcessImageFileName
#define ProcessImageFileName ((PROCESSINFOCLASS)27)
#endif

extern "C"
NTSYSAPI
NTSTATUS
NTAPI
ZwQueryInformationProcess(
    _In_      HANDLE           ProcessHandle,
    _In_      PROCESSINFOCLASS ProcessInformationClass,
    _Out_writes_bytes_(ProcessInformationLength) PVOID ProcessInformation,
    _In_      ULONG            ProcessInformationLength,
    _Out_opt_ PULONG           ReturnLength
    );

NTSTATUS
knFcUtilGetImagePathByPid(_In_ HANDLE Pid, _Out_ PUNICODE_STRING OutPath)
{
    NTSTATUS status;
    PEPROCESS process = NULL;
    HANDLE pHandle = NULL;
    PVOID buffer = NULL;
    ULONG needed = 0;

    OutPath->Buffer = NULL;
    OutPath->Length = 0;
    OutPath->MaximumLength = 0;

    if (Pid == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    do
    {
        status = PsLookupProcessByProcessId(Pid, &process);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        status = ObOpenObjectByPointer(
            process,
            OBJ_KERNEL_HANDLE,
            NULL,
            0,
            *PsProcessType,
            KernelMode,
            &pHandle);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        /* Probe size */
        status = ZwQueryInformationProcess(pHandle, ProcessImageFileName, NULL, 0, &needed);
        if (status != STATUS_INFO_LENGTH_MISMATCH
            && status != STATUS_BUFFER_TOO_SMALL
            && status != STATUS_BUFFER_OVERFLOW)
        {
            /* Truly failed (or returned success with zero-length, also unusable) */
            if (NT_SUCCESS(status))
            {
                status = STATUS_UNSUCCESSFUL;
            }
            break;
        }

        if (needed == 0 || needed > (64 * 1024))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        buffer = knFcAllocateNonPaged(needed);
        if (buffer == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        status = ZwQueryInformationProcess(pHandle, ProcessImageFileName, buffer, needed, &needed);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        {
            PUNICODE_STRING us = (PUNICODE_STRING)buffer;
            if (us->Length == 0 || us->Buffer == NULL)
            {
                status = STATUS_NOT_FOUND;
                break;
            }

            OutPath->Buffer = (PWCHAR)knFcAllocateNonPaged(us->Length);
            if (OutPath->Buffer == NULL)
            {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            RtlCopyMemory(OutPath->Buffer, us->Buffer, us->Length);
            OutPath->Length        = us->Length;
            OutPath->MaximumLength = us->Length;
        }
        status = STATUS_SUCCESS;
    }
    while (FALSE);

    if (buffer != NULL)
    {
        ExFreePoolWithTag(buffer, KNFC_POOL_TAG);
    }
    if (pHandle != NULL)
    {
        ZwClose(pHandle);
    }
    if (process != NULL)
    {
        ObDereferenceObject(process);
    }
    return status;
}

VOID
knFcUtilFreeImagePath(_Inout_ PUNICODE_STRING InOutPath)
{
    if (InOutPath == NULL)
    {
        return;
    }
    if (InOutPath->Buffer != NULL)
    {
        ExFreePoolWithTag(InOutPath->Buffer, KNFC_POOL_TAG);
        InOutPath->Buffer = NULL;
    }
    InOutPath->Length = 0;
    InOutPath->MaximumLength = 0;
}
