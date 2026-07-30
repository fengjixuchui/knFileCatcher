<#
.SYNOPSIS
    Checks minifilter callback source for known IRQL contract regressions.

.PARAMETER SelfTest
    Verifies that the checker detects a synthetic unsafe post callback.
#>

[CmdletBinding()]
param(
    [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'

function Get-FunctionBody
{
    param(
        [Parameter(Mandatory)] [string] $Text,
        [Parameter(Mandatory)] [string] $Name
    )

    $match = [regex]::Match($Text, "(?m)^$([regex]::Escape($Name))\s*\(")
    if (-not $match.Success)
    {
        throw "Function not found: $Name"
    }

    $open = $Text.IndexOf('{', $match.Index)
    if ($open -lt 0)
    {
        throw "Function body not found: $Name"
    }

    $depth = 0
    for ($i = $open; $i -lt $Text.Length; ++$i)
    {
        if ($Text[$i] -eq '{')
        {
            ++$depth
        }
        elseif ($Text[$i] -eq '}')
        {
            --$depth
            if ($depth -eq 0)
            {
                return $Text.Substring($open, $i - $open + 1)
            }
        }
    }

    throw "Unbalanced function body: $Name"
}

function Find-ForbiddenCall
{
    param(
        [Parameter(Mandatory)] [string] $Body,
        [Parameter(Mandatory)] [string[]] $Calls
    )

    foreach ($call in $Calls)
    {
        if ($Body -match "\b$([regex]::Escape($call))\s*\(")
        {
            return $call
        }
    }
    return $null
}

if ($SelfTest)
{
    $fixture = @'
knFcPostWrite(
    void
    )
{
    FltGetStreamHandleContext(0, 0, 0);
}
'@
    $body = Get-FunctionBody -Text $fixture -Name 'knFcPostWrite'
    $hit = Find-ForbiddenCall -Body $body -Calls @('FltGetStreamHandleContext')
    if ($hit -ne 'FltGetStreamHandleContext')
    {
        throw 'Self-test failed to detect the unsafe fixture'
    }
    Write-Host 'IRQL contract checker self-test: PASS'
    exit 0
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$callbackPath = Join-Path $repoRoot 'src\knFcFlt\knFcCallbacks.cpp'
$filterPath = Join-Path $repoRoot 'src\knFcFlt\knFcFlt.cpp'
$queuePath = Join-Path $repoRoot 'src\knFcFlt\knFcQueue.cpp'
$trackPath = Join-Path $repoRoot 'src\knFcFlt\knFcTrack.cpp'

$callbacks = Get-Content -LiteralPath $callbackPath -Raw
$filter = Get-Content -LiteralPath $filterPath -Raw
$queue = Get-Content -LiteralPath $queuePath -Raw
$track = Get-Content -LiteralPath $trackPath -Raw
$failures = [System.Collections.Generic.List[string]]::new()

$rules = @(
    @('knFcPostWrite', @('FltGetStreamHandleContext', 'FltGetFileNameInformation', 'FltQueryInformationFile')),
    @('knFcPostSetInformation', @('FltGetStreamHandleContext', 'FltGetFileNameInformation', 'FltQueryInformationFile')),
    @('knFcPostCleanup', @('FltGetStreamHandleContext', 'FltGetFileNameInformation', 'FltQueryInformationFile', 'FltAcquirePushLockShared', 'FltAcquirePushLockExclusive'))
)

foreach ($rule in $rules)
{
    $body = Get-FunctionBody -Text $callbacks -Name $rule[0]
    $hit = Find-ForbiddenCall -Body $body -Calls $rule[1]
    if ($null -ne $hit)
    {
        $failures.Add("$($rule[0]) contains forbidden call $hit")
    }
}

if ($filter -notmatch 'IRP_MJ_WRITE\s*,\s*0\s*,\s*knFcPreWrite\s*,\s*knFcPostWrite')
{
    $failures.Add('IRP_MJ_WRITE is not wired through knFcPreWrite')
}
if ($filter -notmatch 'IRP_MJ_SET_INFORMATION\s*,\s*0\s*,\s*knFcPreSetInformation\s*,\s*knFcPostSetInformation')
{
    $failures.Add('IRP_MJ_SET_INFORMATION is not wired through knFcPreSetInformation')
}

$postWrite = Get-FunctionBody -Text $callbacks -Name 'knFcPostWrite'
if ($postWrite -notmatch '\bFltReleaseContext\s*\(')
{
    $failures.Add('knFcPostWrite does not release its pre-captured context')
}
if ($postWrite -notmatch 'FLTFL_POST_OPERATION_DRAINING')
{
    $failures.Add('knFcPostWrite does not handle draining')
}
if ($postWrite -match '\bDbgPrint\s*\(')
{
    $failures.Add('knFcPostWrite contains high-frequency debug logging')
}

$postSetInformation = Get-FunctionBody -Text $callbacks -Name 'knFcPostSetInformation'
if (($callbacks -notmatch 'KNFC_SETINFO_CLEAR_DELETE') -or
    ($postSetInformation -notmatch '\bInterlockedAnd\s*\('))
{
    $failures.Add('SetInformation delete cancellation is not handled')
}

$preCleanup = Get-FunctionBody -Text $callbacks -Name 'knFcPreCleanup'
if ($preCleanup -notmatch 'KNFC_SHC_MODIFIED\s*\|\s*KNFC_SHC_DELETE_ON_CLOSE')
{
    $failures.Add('knFcPreCleanup can miss delete-only disposition handles')
}
if (($preCleanup -notmatch '\bDeletePending\b') -or
    ($preCleanup -notmatch '\bDeleteDispositionObserved\b'))
{
    $failures.Add('knFcPreCleanup does not reconcile racing disposition state')
}

$postCleanup = Get-FunctionBody -Text $callbacks -Name 'knFcPostCleanup'
if ($postCleanup -notmatch '\bknFcQueueEnqueue\s*\(')
{
    $failures.Add('knFcPostCleanup is not enqueue-only')
}
if ($postCleanup -match '\bDbgPrint\s*\(')
{
    $failures.Add('knFcPostCleanup contains high-frequency debug logging')
}
if ($queue -notmatch 'knFcExcludeMatches\s*\(&Item->Path\)')
{
    $failures.Add('Async queue worker does not perform exclude matching')
}

$queueUninitialize = Get-FunctionBody -Text $queue -Name 'knFcQueueUninitialize'
if ($queueUninitialize -notmatch '\bExWaitForRundownProtectionRelease\s*\(')
{
    $failures.Add('Queue teardown does not wait for producers')
}

foreach ($producerName in @('knFcQueueEnqueue', 'knFcQueueSendSyncFromCleanup'))
{
    $producerBody = Get-FunctionBody -Text $queue -Name $producerName
    if (($producerBody -notmatch '\bExAcquireRundownProtection\s*\(') -or
        ($producerBody -notmatch '\bExReleaseRundownProtection\s*\('))
    {
        $failures.Add("$producerName is not protected against queue teardown")
    }
}

if (($queue -match '\bKeDelayExecutionThread\s*\(') -or
    ($track -match '\bKeDelayExecutionThread\s*\('))
{
    $failures.Add('Worker startup failure still relies on a timed delay')
}
if (($queue -notmatch '\bZwWaitForSingleObject\s*\(threadHandle') -or
    ($track -notmatch '\bZwWaitForSingleObject\s*\(threadHandle'))
{
    $failures.Add('Worker startup failure does not join by thread handle')
}

if ($failures.Count -ne 0)
{
    foreach ($failure in $failures)
    {
        Write-Error $failure
    }
    exit 1
}

Write-Host 'Minifilter IRQL contract checks: PASS'
