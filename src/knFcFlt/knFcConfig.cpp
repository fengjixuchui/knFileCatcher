/*
 * knFcConfig.cpp
 * Watch-root list management. Roots are stored as NT paths (e.g.
 * "\Device\HarddiskVolume3\Games\Foo\"). Path matching is
 * case-insensitive prefix match.
 */

#include "knFcFlt.h"

typedef struct _KNFC_CONFIG
{
    EX_PUSH_LOCK    Lock;
    ULONG           RootCount;
    UNICODE_STRING  Roots[KNFC_MAX_WATCH_ROOTS];
} KNFC_CONFIG;

static KNFC_CONFIG g_Config;

NTSTATUS
knFcConfigInitialize(VOID)
{
    RtlZeroMemory(&g_Config, sizeof(g_Config));
    FltInitializePushLock(&g_Config.Lock);
    return STATUS_SUCCESS;
}

VOID
knFcConfigUninitialize(VOID)
{
    knFcConfigClearRoots();
    FltDeletePushLock(&g_Config.Lock);
}

static VOID
knFcConfigFreeRootLocked(_Inout_ PUNICODE_STRING Root)
{
    if (Root->Buffer != NULL)
    {
        ExFreePoolWithTag(Root->Buffer, KNFC_POOL_TAG);
        Root->Buffer = NULL;
    }
    Root->Length = 0;
    Root->MaximumLength = 0;
}

NTSTATUS
knFcConfigAddRoot(_In_ PCUNICODE_STRING Path)
{
    NTSTATUS status;
    PWCHAR copy = NULL;
    USHORT bytes;

    if (Path == NULL || Path->Buffer == NULL || Path->Length == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (Path->Length > (KNFC_MAX_WATCH_PATH_CHARS * sizeof(WCHAR)))
    {
        return STATUS_NAME_TOO_LONG;
    }

    bytes = Path->Length;
    copy = (PWCHAR)knFcAllocateNonPaged(bytes);
    if (copy == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(copy, Path->Buffer, bytes);

    KeEnterCriticalRegion();
    FltAcquirePushLockExclusive(&g_Config.Lock);

    do
    {
        if (g_Config.RootCount >= KNFC_MAX_WATCH_ROOTS)
        {
            status = STATUS_QUOTA_EXCEEDED;
            break;
        }

        g_Config.Roots[g_Config.RootCount].Buffer        = copy;
        g_Config.Roots[g_Config.RootCount].Length        = bytes;
        g_Config.Roots[g_Config.RootCount].MaximumLength = bytes;
        ++g_Config.RootCount;
        copy = NULL;  /* now owned by g_Config */
        status = STATUS_SUCCESS;
    }
    while (FALSE);

    FltReleasePushLock(&g_Config.Lock);
    KeLeaveCriticalRegion();

    if (copy != NULL)
    {
        ExFreePoolWithTag(copy, KNFC_POOL_TAG);
    }
    return status;
}

VOID
knFcConfigClearRoots(VOID)
{
    ULONG i;

    KeEnterCriticalRegion();
    FltAcquirePushLockExclusive(&g_Config.Lock);

    for (i = 0; i < g_Config.RootCount; ++i)
    {
        knFcConfigFreeRootLocked(&g_Config.Roots[i]);
    }
    g_Config.RootCount = 0;

    FltReleasePushLock(&g_Config.Lock);
    KeLeaveCriticalRegion();
}

ULONG
knFcConfigGetRootCount(VOID)
{
    /* Stale-by-one read; for stats UI only. */
    return g_Config.RootCount;
}

BOOLEAN
knFcConfigPathStartsWithAnyRoot(_In_ PCUNICODE_STRING Path)
{
    BOOLEAN result = FALSE;
    ULONG i;

    if (Path == NULL || Path->Buffer == NULL || Path->Length == 0)
    {
        return FALSE;
    }

    KeEnterCriticalRegion();
    FltAcquirePushLockShared(&g_Config.Lock);

    for (i = 0; i < g_Config.RootCount; ++i)
    {
        PCUNICODE_STRING root = &g_Config.Roots[i];
        USHORT rootBytes;
        USHORT rootCch;
        WCHAR  rootLastChar;

        rootBytes = root->Length;
        if (rootBytes == 0 || Path->Length < rootBytes)
        {
            continue;
        }
        if (!RtlPrefixUnicodeString(root, Path, TRUE))
        {
            continue;
        }

        /* Prefix matched; enforce a path boundary so that a root of
         *   \Device\HarddiskVolume3\Foo
         * does not falsely match
         *   \Device\HarddiskVolume3\Foobar.txt
         * Two acceptable forms:
         *   (a) root itself ends with '\'           -> any suffix OK
         *   (b) Path[rootCch] is '\'  OR  Path ends exactly at rootBytes
         */
        rootCch = (USHORT)(rootBytes / sizeof(WCHAR));
        rootLastChar = root->Buffer[rootCch - 1];
        if (rootLastChar == L'\\')
        {
            result = TRUE;
            break;
        }
        if (Path->Length == rootBytes)
        {
            result = TRUE;
            break;
        }
        if (Path->Buffer[rootCch] == L'\\')
        {
            result = TRUE;
            break;
        }
        /* else: false-positive prefix, keep scanning */
    }

    FltReleasePushLock(&g_Config.Lock);
    KeLeaveCriticalRegion();
    return result;
}
