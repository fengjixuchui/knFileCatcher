/*
 * knFcExclude.cpp
 * Exclude pattern list. PostCleanup consults this before enqueueing.
 *
 * Pattern syntax:
 *   - Starts with '.': case-insensitive extension suffix match
 *                       (e.g. ".tmp" matches "\Foo\bar.tmp")
 *   - Otherwise:       case-insensitive substring match against the
 *                       NT path (e.g. "\Temp\" matches anywhere)
 *
 * Storage: small fixed array guarded by an EX_PUSH_LOCK.
 */

#include "knFcFlt.h"

typedef struct _KNFC_EXCLUDE_CTX
{
    EX_PUSH_LOCK            Lock;
    ULONG                   Count;
    UNICODE_STRING          Patterns[KNFC_MAX_EXCLUDE_PATTERNS];
    volatile LONG64         MatchedCount;
} KNFC_EXCLUDE_CTX;

static KNFC_EXCLUDE_CTX g_Exclude;

NTSTATUS
knFcExcludeInitialize(VOID)
{
    RtlZeroMemory(&g_Exclude, sizeof(g_Exclude));
    FltInitializePushLock(&g_Exclude.Lock);
    return STATUS_SUCCESS;
}

VOID
knFcExcludeUninitialize(VOID)
{
    knFcExcludeClear();
    FltDeletePushLock(&g_Exclude.Lock);
}

static VOID
knFcExcludeFreeLocked(_Inout_ PUNICODE_STRING Pat)
{
    if (Pat->Buffer != NULL)
    {
        ExFreePoolWithTag(Pat->Buffer, KNFC_POOL_TAG);
        Pat->Buffer = NULL;
    }
    Pat->Length = 0;
    Pat->MaximumLength = 0;
}

NTSTATUS
knFcExcludeAdd(_In_ PCUNICODE_STRING Pattern)
{
    NTSTATUS status;
    PWCH copy = NULL;
    USHORT bytes;

    if (Pattern == NULL || Pattern->Buffer == NULL || Pattern->Length == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (Pattern->Length > (KNFC_MAX_EXCLUDE_CHARS * sizeof(WCHAR)))
    {
        return STATUS_NAME_TOO_LONG;
    }

    bytes = Pattern->Length;
    copy = (PWCH)knFcAllocateNonPaged(bytes);
    if (copy == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(copy, Pattern->Buffer, bytes);

    KeEnterCriticalRegion();
    FltAcquirePushLockExclusive(&g_Exclude.Lock);

    do
    {
        if (g_Exclude.Count >= KNFC_MAX_EXCLUDE_PATTERNS)
        {
            status = STATUS_QUOTA_EXCEEDED;
            break;
        }
        g_Exclude.Patterns[g_Exclude.Count].Buffer        = copy;
        g_Exclude.Patterns[g_Exclude.Count].Length        = bytes;
        g_Exclude.Patterns[g_Exclude.Count].MaximumLength = bytes;
        ++g_Exclude.Count;
        copy = NULL;
        status = STATUS_SUCCESS;
    }
    while (FALSE);

    FltReleasePushLock(&g_Exclude.Lock);
    KeLeaveCriticalRegion();

    if (copy != NULL)
    {
        ExFreePoolWithTag(copy, KNFC_POOL_TAG);
    }
    return status;
}

VOID
knFcExcludeClear(VOID)
{
    ULONG i;

    KeEnterCriticalRegion();
    FltAcquirePushLockExclusive(&g_Exclude.Lock);
    for (i = 0; i < g_Exclude.Count; ++i)
    {
        knFcExcludeFreeLocked(&g_Exclude.Patterns[i]);
    }
    g_Exclude.Count = 0;
    FltReleasePushLock(&g_Exclude.Lock);
    KeLeaveCriticalRegion();
}

ULONG
knFcExcludeGetCount(VOID)
{
    /* Read without lock: stale by at most one update; UX only. */
    return g_Exclude.Count;
}

ULONGLONG
knFcExcludeGetMatchedCount(VOID)
{
    return (ULONGLONG)g_Exclude.MatchedCount;
}

/* ----- matching primitives ----- */

static BOOLEAN
knFcSuffixCi(_In_ PCUNICODE_STRING Hay, _In_ PCUNICODE_STRING Needle)
{
    UNICODE_STRING tail;

    if (Needle->Length == 0)
    {
        return TRUE;
    }
    if (Hay->Length < Needle->Length)
    {
        return FALSE;
    }
    tail.Buffer = (PWCH)((PUCHAR)Hay->Buffer + (Hay->Length - Needle->Length));
    tail.Length = Needle->Length;
    tail.MaximumLength = Needle->Length;
    return RtlEqualUnicodeString(&tail, Needle, TRUE);
}

static BOOLEAN
knFcSubstringCi(_In_ PCUNICODE_STRING Hay, _In_ PCUNICODE_STRING Needle)
{
    USHORT i;
    USHORT span;

    if (Needle->Length == 0)
    {
        return TRUE;
    }
    if (Hay->Length < Needle->Length)
    {
        return FALSE;
    }
    span = (USHORT)(Hay->Length - Needle->Length);
    for (i = 0; i <= span; i += sizeof(WCHAR))
    {
        UNICODE_STRING window;
        window.Buffer = (PWCH)((PUCHAR)Hay->Buffer + i);
        window.Length = Needle->Length;
        window.MaximumLength = Needle->Length;
        if (RtlEqualUnicodeString(&window, Needle, TRUE))
        {
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN
knFcExcludeMatches(_In_ PCUNICODE_STRING Path)
{
    BOOLEAN matched = FALSE;
    ULONG i;

    if (Path == NULL || Path->Buffer == NULL || Path->Length == 0)
    {
        return FALSE;
    }

    KeEnterCriticalRegion();
    FltAcquirePushLockShared(&g_Exclude.Lock);

    for (i = 0; i < g_Exclude.Count; ++i)
    {
        PCUNICODE_STRING p = &g_Exclude.Patterns[i];
        BOOLEAN isExt = (p->Length >= sizeof(WCHAR)) && (p->Buffer[0] == L'.');
        BOOLEAN hit = isExt
            ? knFcSuffixCi(Path, p)
            : knFcSubstringCi(Path, p);
        if (hit)
        {
            matched = TRUE;
            break;
        }
    }

    FltReleasePushLock(&g_Exclude.Lock);
    KeLeaveCriticalRegion();

    if (matched)
    {
        InterlockedIncrement64(&g_Exclude.MatchedCount);
    }
    return matched;
}
