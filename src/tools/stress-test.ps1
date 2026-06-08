<#
.SYNOPSIS
    knFileCatcher load test driver.

.DESCRIPTION
    Spawns N tracked-root processes; each one runs in parallel and
    creates / writes / renames / deletes M files inside the watch
    root and outside it. Useful for exercising:
      - Queue depth and dropped-counter under burst
      - Driver worker pool (M6)
      - manifest.jsonl throughput
      - rename + DELETE_ON_CLOSE paths
      - exclude pattern hit rate

.PARAMETER WatchRoot
    DOS path of the watch root that knFcUxWpf is already monitoring.

.PARAMETER OutsideRoot
    DOS path the child processes will also write to (must be outside
    the watch root) - to exercise lineage tracking.

.PARAMETER Processes
    Number of root processes to spawn in parallel. Default 4.

.PARAMETER FilesPerProcess
    Files each process creates inside the watch root. Default 200.

.EXAMPLE
    .\stress-test.ps1 -WatchRoot C:\Temp\knFc-watch -OutsideRoot C:\Temp\knFc-out
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $WatchRoot,
    [Parameter(Mandatory)] [string] $OutsideRoot,
    [int] $Processes        = 4,
    [int] $FilesPerProcess  = 200
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $WatchRoot))
{
    throw "WatchRoot not found: $WatchRoot"
}
New-Item -ItemType Directory -Force -Path $OutsideRoot | Out-Null

# Stage a runner exe under the watch root so newly-spawned processes
# qualify as tracking ROOTs.
$runner = Join-Path $WatchRoot "stress-runner.exe"
if (-not (Test-Path $runner))
{
    Copy-Item -Path "$env:SystemRoot\System32\cmd.exe" -Destination $runner -Force
}

$workerScript = {
    param($Wid, $WatchRoot, $OutsideRoot, $FilesPerProcess, $Runner)

    # Inside-root, plain create + write + close.
    for ($i = 0; $i -lt $FilesPerProcess; $i++)
    {
        $name = Join-Path $WatchRoot ("w{0}-{1}-{2}.txt" -f $PID, $Wid, $i)
        Set-Content -LiteralPath $name -Value ("line one`r`nline two") -Encoding ASCII
    }

    # Outside-root via direct write (parent is tracked so child writes
    # outside should still be captured).
    for ($i = 0; $i -lt ([int]($FilesPerProcess / 2)); $i++)
    {
        $name = Join-Path $OutsideRoot ("out-{0}-{1}-{2}.bin" -f $PID, $Wid, $i)
        [IO.File]::WriteAllBytes($name, (New-Object byte[] (1024 + $i * 13)))
    }

    # Rename half of the inside files.
    Get-ChildItem -LiteralPath $WatchRoot -Filter ("w{0}-{1}-*.txt" -f $PID, $Wid) `
        | Select-Object -First ([int]($FilesPerProcess / 2)) `
        | ForEach-Object { Rename-Item -LiteralPath $_.FullName -NewName ("ren-" + $_.Name) }

    # Memory-mapped write on a single file.
    $mm = Join-Path $WatchRoot ("mm-{0}-{1}.dat" -f $PID, $Wid)
    [IO.File]::WriteAllBytes($mm, (New-Object byte[] 4096))
    $f = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile($mm, "OpenOrCreate", $null, 4096)
    $acc = $f.CreateViewAccessor()
    $acc.Write(0, [byte]0xAB)
    $acc.Dispose()
    $f.Dispose()

    # A DELETE_ON_CLOSE temp file.
    $tmp = Join-Path $WatchRoot ("temp-{0}-{1}.tmp" -f $PID, $Wid)
    $opts = [IO.FileOptions]::DeleteOnClose
    $fs = [IO.File]::Create($tmp, 4096, $opts)
    $bytes = [Text.Encoding]::ASCII.GetBytes("temp content for delete-on-close path")
    $fs.Write($bytes, 0, $bytes.Length)
    $fs.Dispose()
}

# Each process must itself be a tracked ROOT so the worker actions
# qualify as tracked writes. We launch the script via the staged
# runner.exe (which sits under WatchRoot) so the spawned powershell
# child counts as a ChildTracked of a Root.
$jobs = @()
for ($w = 0; $w -lt $Processes; $w++)
{
    $job = Start-Process -FilePath $runner `
        -ArgumentList @(
            "/c",
            "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command `"& { $($workerScript.ToString()) } $w '$WatchRoot' '$OutsideRoot' $FilesPerProcess '$runner'`""
        ) `
        -PassThru
    $jobs += $job
}

Write-Host "stress-test: $Processes runners spawned (PIDs: $($jobs.Id -join ','))"
$jobs | ForEach-Object { $_.WaitForExit() }
Write-Host "stress-test: done"
