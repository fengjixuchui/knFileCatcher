<#
.SYNOPSIS
    Build the knFcTester console harness.

.DESCRIPTION
    Enters a VS 2022 dev shell, then calls cl directly to produce a
    single-file C++20 console EXE. Output lands under <repo>\build\tester.

.PARAMETER OutDir
    Where to place the obj + knFcTester.exe.
    Default: <repo>\build\tester.

.PARAMETER VsPath
    Visual Studio 2022 installation root. Auto-detected if omitted.
#>

[CmdletBinding()]
param(
    [string] $OutDir,
    [string] $VsPath
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $OutDir)
{
    $OutDir = Join-Path $here '..\..\build\tester'
}

if (-not $VsPath)
{
    foreach ($cand in @(
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools'))
    {
        if (Test-Path $cand)
        {
            $VsPath = $cand
            break
        }
    }
    if (-not $VsPath)
    {
        throw "Visual Studio 2022 not detected"
    }
}

Import-Module (Join-Path $VsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$src = Join-Path $here 'knFcTester.cpp'
if (-not (Test-Path $src))
{
    throw "source not found: $src"
}

Push-Location $OutDir
try
{
    Remove-Item *.obj -ErrorAction SilentlyContinue

    Write-Host "knFcTester-build: compiling..."
    # UNICODE / _UNICODE / _CRT_SECURE_NO_WARNINGS / WIN32_LEAN_AND_MEAN
    # are defined inside knFcTester.cpp itself so the file compiles cleanly
    # with a plain `cl /TP knFcTester.cpp` too. We just need kernel32.lib for
    # CopyFileW / CreateProcessW / GetTempPathW.
    & cl.exe /nologo /W3 /O2 /MT /TP /std:c++20 `
        $src /link /OUT:knFcTester.exe /SUBSYSTEM:CONSOLE `
        kernel32.lib
    if ($LASTEXITCODE -ne 0)
    {
        throw "cl failed (exit $LASTEXITCODE)"
    }

    $exe = Get-Item knFcTester.exe
    Write-Host ("knFcTester-build: done  {0}  ({1} bytes)" -f $exe.FullName, $exe.Length)
}
finally
{
    Pop-Location
}
