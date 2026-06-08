<#
.SYNOPSIS
    Build knFcFlt.sys without the WDK Visual Studio integration.

.DESCRIPTION
    Invokes cl /kernel /std:c++20 and link /DRIVER directly. Useful on machines
    that have the WDK + VS 2022 but not the "Windows Driver Kit" VS
    extension that provides the IDE driver project targets.

    Run from any PowerShell window. The script enters a VS dev shell
    via Microsoft.VisualStudio.DevShell.dll, so vcvars64 etc. are not
    needed up front.

.PARAMETER OutDir
    Where to place .obj files and knFcFlt.sys.
    Default: .\build under this script's directory.

.PARAMETER VsPath
    Visual Studio 2022 installation root. Auto-detected if omitted.

.PARAMETER SrcDir
    Directory containing knFcFlt.cpp et al. Default: ..\knFcFlt
    relative to the script.

.PARAMETER CommonDir
    Directory containing knFcProto.h. Default: ..\common
    relative to the script.

.PARAMETER NoSign
    Skip the post-build call to sign.ps1.

.EXAMPLE
    .\build-driver.ps1
    .\build-driver.ps1 -OutDir E:\tmp\knFcFlt-build -NoSign
#>

[CmdletBinding()]
param(
    [string] $OutDir,
    [string] $VsPath,
    [string] $SrcDir,
    [string] $CommonDir,
    [switch] $NoSign
)

$ErrorActionPreference = "Stop"

$here     = Split-Path -Parent $PSCommandPath
$repoRoot = Split-Path -Parent $here   # ..\..  -> src

if (-not $SrcDir)    { $SrcDir    = Join-Path $repoRoot 'knFcFlt' }
if (-not $CommonDir) { $CommonDir = Join-Path $repoRoot 'common'  }
if (-not $OutDir)    { $OutDir    = Join-Path $here     'build'   }

if (-not (Test-Path (Join-Path $SrcDir 'knFcFlt.cpp')))
{
    throw "knFcFlt.cpp not found at $SrcDir"
}

if (-not $VsPath)
{
    foreach ($cand in @(
        'C:\Program Files\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools'))
    {
        if (Test-Path $cand) { $VsPath = $cand; break }
    }
    if (-not $VsPath) { throw "Visual Studio 2022 not detected" }
}

Import-Module (Join-Path $VsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$ver    = $env:WindowsSDKVersion.TrimEnd('\')
$kmInc  = "$env:WindowsSdkDir`Include\$ver\km"
$shInc  = "$env:WindowsSdkDir`Include\$ver\shared"
$crtInc = "$env:WindowsSdkDir`Include\$ver\km\crt"
$kmLib  = "$env:WindowsSdkDir`Lib\$ver\km\x64"

if (-not (Test-Path $kmInc))
{
    throw "WDK km headers not found at $kmInc"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Push-Location $OutDir
try
{
    Remove-Item *.obj -ErrorAction SilentlyContinue

    $cFiles = @(
        'knFcFlt.cpp','knFcCallbacks.cpp','knFcComm.cpp','knFcConfig.cpp',
        'knFcContexts.cpp','knFcExclude.cpp','knFcQueue.cpp','knFcTrack.cpp',
        'knFcUtil.cpp'
    )
    $paths = $cFiles | ForEach-Object { Join-Path $SrcDir $_ }

    Write-Host "knFcFlt-build: compiling $($paths.Count) TUs..."
    & cl.exe /c /kernel /TP /std:c++20 /Zi /W3 /WX- /Od /Oy- /GS- /Gz /nologo `
        /D_AMD64_ /DAMD64 /D_WIN64 `
        /DNTDDI_VERSION=0x0A000007 /D_WIN32_WINNT=0x0A00 `
        /I"$kmInc" /I"$shInc" /I"$crtInc" /I"$CommonDir" `
        @paths
    if ($LASTEXITCODE -ne 0) { throw "cl failed (exit $LASTEXITCODE)" }

    # Compile the Win32 VS_VERSION_INFO resource so the resulting .sys
    # carries CompanyName=kernullist / ProductName=knFileCatcher /
    # FileVersion=x.y.z.0 visible from Explorer "Properties > Details".
    $rcFile  = Join-Path $SrcDir 'knFcFlt.rc'
    $verFile = Join-Path $SrcDir 'knFcVersion.h'
    $resFile = $null
    if (Test-Path $rcFile)
    {
        # When invoked standalone (without build-release.ps1) the auto-
        # generated knFcVersion.h might not exist yet. Drop a 0.0.0
        # placeholder so the rc compile still succeeds; the sys will
        # carry version 0.0.0.0 which is obviously a dev build.
        if (-not (Test-Path $verFile))
        {
            Write-Warning "knFcVersion.h missing - writing 0.0.0 placeholder"
            $placeholder = @(
                '/* auto-generated placeholder by build-driver.ps1 */',
                '#pragma once',
                '#define KNFC_VER_MAJOR 0',
                '#define KNFC_VER_MINOR 0',
                '#define KNFC_VER_PATCH 0',
                '#define KNFC_VER_BUILD 0',
                '#define KNFC_VER_STR   "0.0.0.0"'
            )
            Set-Content -LiteralPath $verFile -Value $placeholder -Encoding ASCII
        }
        Write-Host "knFcFlt-build: compiling resource..."
        & rc.exe /nologo /fo knFcFlt.res /I"$SrcDir" $rcFile
        if ($LASTEXITCODE -ne 0) { throw "rc failed (exit $LASTEXITCODE)" }
        $resFile = 'knFcFlt.res'
    }
    else
    {
        Write-Warning "knFcFlt.rc not found - sys will have no version info"
    }

    Write-Host "knFcFlt-build: linking knFcFlt.sys..."
    $objs = Get-ChildItem *.obj | ForEach-Object FullName
    # /INTEGRITYCHECK is mandatory: it sets the
    # IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY (0x80) bit which
    # PsSetCreateProcessNotifyRoutineEx (and other security-critical
    # APIs) require - otherwise they return STATUS_ACCESS_DENIED.
    $linkInputs = @($objs)
    if ($resFile) { $linkInputs += $resFile }
    & link.exe /nologo /OUT:knFcFlt.sys /SUBSYSTEM:NATIVE,10.0 `
        /ENTRY:DriverEntry /DRIVER /INTEGRITYCHECK `
        /NODEFAULTLIB /OPT:REF /OPT:ICF /MACHINE:X64 /DEBUG /MANIFEST:NO `
        /LIBPATH:"$kmLib" `
        fltMgr.lib ntoskrnl.lib hal.lib `
        @linkInputs
    if ($LASTEXITCODE -ne 0) { throw "link failed (exit $LASTEXITCODE)" }

    if (-not $NoSign)
    {
        $sign = Join-Path (Split-Path -Parent $here) 'sign.ps1'
        if (Test-Path $sign)
        {
            Write-Host "knFcFlt-build: signing..."
            & $sign -Sys (Resolve-Path .\knFcFlt.sys).Path
        }
        else
        {
            Write-Warning "sign.ps1 not found at $sign - skipping signing"
        }
    }

    $sys = Get-ChildItem knFcFlt.sys
    Write-Host ("knFcFlt-build: done  $($sys.FullName)  ($($sys.Length) bytes)")
}
finally
{
    Pop-Location
}
