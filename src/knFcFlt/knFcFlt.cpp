/*
 * knFcFlt.cpp
 * DriverEntry + minifilter registration (M3).
 */

#include "knFcFlt.h"

PFLT_FILTER g_FilterHandle = NULL;
KNFC_EX_ALLOCATE_POOL2_FN g_KnFcExAllocatePool2 = NULL;

extern "C"
DRIVER_INITIALIZE DriverEntry;

static NTSTATUS FLTAPI knFcInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
    );

static NTSTATUS FLTAPI knFcInstanceQueryTeardown(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    );

static NTSTATUS FLTAPI knFcFilterUnload(_In_ FLT_FILTER_UNLOAD_FLAGS Flags);

CONST FLT_OPERATION_REGISTRATION g_Callbacks[] =
{
    { IRP_MJ_CREATE,                              0, knFcPreCreate,             knFcPostCreate },
    { IRP_MJ_WRITE,                               0, NULL,                      knFcPostWrite },
    { IRP_MJ_SET_INFORMATION,                     0, NULL,                      knFcPostSetInformation },
    { IRP_MJ_CLEANUP,                             0, knFcPreCleanup,            knFcPostCleanup },
    { IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION, 0, knFcPreAcquireForSection,  NULL },
    { IRP_MJ_OPERATION_END }
};

CONST FLT_REGISTRATION g_FilterRegistration =
{
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    g_ContextRegistration,
    g_Callbacks,
    knFcFilterUnload,
    knFcInstanceSetup,
    knFcInstanceQueryTeardown,
    NULL, NULL,
    NULL, NULL, NULL
};

VOID
knFcPoolInitialize(VOID)
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
    g_KnFcExAllocatePool2 =
        (KNFC_EX_ALLOCATE_POOL2_FN)MmGetSystemRoutineAddress(&routineName);

    if (g_KnFcExAllocatePool2 != NULL)
    {
        DbgPrint("knFcFlt: pool allocator ExAllocatePool2\n");
    }
    else
    {
        DbgPrint("knFcFlt: pool allocator ExAllocatePoolWithTag fallback\n");
    }
}

extern "C"
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    BOOLEAN configReady  = FALSE;
    BOOLEAN excludeReady = FALSE;
    BOOLEAN trackReady   = FALSE;
    BOOLEAN queueReady   = FALSE;
    BOOLEAN filterReady  = FALSE;
    BOOLEAN commReady    = FALSE;

    UNREFERENCED_PARAMETER(RegistryPath);

    knFcPoolInitialize();

    DbgPrint("knFcFlt: DriverEntry (proto 0x%08x)\n", KNFC_PROTOCOL_VERSION);

    do
    {
        status = knFcConfigInitialize();
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: knFcConfigInitialize failed 0x%08x\n", status);
            break;
        }
        configReady = TRUE;

        status = knFcExcludeInitialize();
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: knFcExcludeInitialize failed 0x%08x\n", status);
            break;
        }
        excludeReady = TRUE;

        status = knFcTrackInitialize();
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: knFcTrackInitialize failed 0x%08x\n", status);
            break;
        }
        trackReady = TRUE;

        status = FltRegisterFilter(DriverObject, &g_FilterRegistration, &g_FilterHandle);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: FltRegisterFilter failed 0x%08x\n", status);
            break;
        }
        filterReady = TRUE;

        status = knFcCommInitialize(g_FilterHandle);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: knFcCommInitialize failed 0x%08x\n", status);
            break;
        }
        commReady = TRUE;

        status = knFcQueueInitialize();
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: knFcQueueInitialize failed 0x%08x\n", status);
            break;
        }
        queueReady = TRUE;

        status = FltStartFiltering(g_FilterHandle);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("knFcFlt: FltStartFiltering failed 0x%08x\n", status);
            break;
        }

        DbgPrint("knFcFlt: started\n");
        return STATUS_SUCCESS;
    }
    while (FALSE);

    /* Same ordering as the normal FilterUnload path so we never tear
     * down a port (comm) or the filter object while a worker thread
     * (queue, track exit-defer) may still call into it. */
    if (queueReady)
    {
        knFcQueueUninitialize();
    }
    if (trackReady)
    {
        knFcTrackUninitialize();
    }
    if (commReady)
    {
        knFcCommUninitialize();
    }
    if (filterReady)
    {
        FltUnregisterFilter(g_FilterHandle);
        g_FilterHandle = NULL;
    }
    if (excludeReady)
    {
        knFcExcludeUninitialize();
    }
    if (configReady)
    {
        knFcConfigUninitialize();
    }
    return status;
}

static NTSTATUS FLTAPI
knFcInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
    )
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeDeviceType);

    if (VolumeFilesystemType == FLT_FSTYPE_RAW
        || VolumeFilesystemType == FLT_FSTYPE_UNKNOWN)
    {
        return STATUS_FLT_DO_NOT_ATTACH;
    }
    DbgPrint("knFcFlt: instance attach fsType=%u\n", (ULONG)VolumeFilesystemType);
    return STATUS_SUCCESS;
}

static NTSTATUS FLTAPI
knFcInstanceQueryTeardown(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    return STATUS_SUCCESS;
}

static NTSTATUS FLTAPI
knFcFilterUnload(_In_ FLT_FILTER_UNLOAD_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);

    DbgPrint("knFcFlt: FilterUnload\n");

    /* Order matters: stop queue first (it owns a thread that calls
     * knFcCommSendMessage); then comm; then filter; then track/config.
     */
    /* Teardown order matters:
     *   1) queue workers stop first - they may have FltSendMessage in
     *      flight and need a live g_FilterHandle / g_ClientPort.
     *   2) track teardown stops the PsSet... callbacks and the
     *      exit-defer worker. The worker also calls FltSendMessage so
     *      this MUST happen before comm uninit.
     *   3) comm uninit closes the client and server ports.
     *   4) FltUnregisterFilter only after all message senders are gone,
     *      otherwise it can hang/fail waiting on outstanding work.
     *   5) exclude/config last - they hold pure metadata. */
    knFcQueueUninitialize();
    knFcTrackUninitialize();
    knFcCommUninitialize();
    FltUnregisterFilter(g_FilterHandle);
    g_FilterHandle = NULL;
    knFcExcludeUninitialize();
    knFcConfigUninitialize();

    return STATUS_SUCCESS;
}
