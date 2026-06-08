<#
.SYNOPSIS
    knFcFlt build-time signing helper (testsigning / dev cert).

.DESCRIPTION
    Signs the driver .sys (and .cat if present) with a self-signed
    dev certificate so the binary can be loaded on a machine with
    'bcdedit /set testsigning on'.
    Creates the certificate on first use; reuses it afterwards.

    Intended to be invoked from the VS post-build event:
      powershell.exe -ExecutionPolicy Bypass `
        -File "$(ProjectDir)..\sign.ps1" `
        -Sys "$(TargetPath)" `
        -Cat "$(TargetDir)knFcFlt.cat"

.PARAMETER Sys
    Full path to the .sys file to sign.

.PARAMETER Cat
    Optional path to the .cat file. Signed only if it exists.

.PARAMETER Subject
    Subject of the self-signed cert. Defaults to "CN=knFcFlt-Dev".
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Sys,
    [Parameter()]          [string] $Cat,
    [string] $Subject = "CN=knFcFlt-Dev"
)

$ErrorActionPreference = "Stop"

function Import-KnFcWindowsModule
{
    param([string] $ModuleName)

    $roots = @(
        (Join-Path $PSHOME 'Modules'),
        (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'),
        (Join-Path $env:SystemRoot 'SysWOW64\WindowsPowerShell\v1.0\Modules')
    )

    foreach ($root in $roots)
    {
        $manifest = Join-Path $root "$ModuleName\$ModuleName.psd1"
        if (Test-Path -LiteralPath $manifest)
        {
            Import-Module $manifest -ErrorAction SilentlyContinue
            return
        }
    }

    Import-Module $ModuleName -ErrorAction SilentlyContinue
}

Import-KnFcWindowsModule -ModuleName 'Microsoft.PowerShell.Security'
Import-KnFcWindowsModule -ModuleName 'PKI'

if ($null -eq (Get-PSDrive -Name Cert -ErrorAction SilentlyContinue))
{
    throw "knFcFlt[sign]: PowerShell Cert provider is not available"
}

function Get-OrCreate-DevCert
{
    param([string] $Subject)

    $cert = Get-ChildItem Cert:\CurrentUser\My `
        | Where-Object { $_.Subject -eq $Subject } `
        | Sort-Object NotAfter -Descending `
        | Select-Object -First 1

    if ($null -ne $cert)
    {
        return $cert
    }

    Write-Host "knFcFlt[sign]: creating self-signed dev cert $Subject"
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -CertStoreLocation Cert:\CurrentUser\My `
        -KeyExportPolicy Exportable `
        -KeyUsage DigitalSignature `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -NotAfter (Get-Date).AddYears(5)
    return $cert
}

function Find-SignTool
{
    $bases = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    ) | Where-Object { Test-Path $_ }

    foreach ($base in $bases)
    {
        $hits = Get-ChildItem -Path $base -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue `
            | Where-Object { $_.FullName -match '\\x64\\' } `
            | Sort-Object { $_.Directory.Parent.Name } -Descending

        if ($null -ne $hits -and $hits.Count -gt 0)
        {
            return $hits[0].FullName
        }
    }
    throw "knFcFlt[sign]: signtool.exe (x64) not found under Windows Kits"
}

if (-not (Test-Path -LiteralPath $Sys))
{
    throw "knFcFlt[sign]: file not found: $Sys"
}

$cert = Get-OrCreate-DevCert -Subject $Subject
$signtool = Find-SignTool

$thumb = $cert.Thumbprint
Write-Host "knFcFlt[sign]: signing $Sys"
& $signtool sign /fd SHA256 /sha1 $thumb $Sys
if ($LASTEXITCODE -ne 0)
{
    throw "knFcFlt[sign]: signtool failed on $Sys (exit $LASTEXITCODE)"
}

if ($PSBoundParameters.ContainsKey('Cat') -and (Test-Path -LiteralPath $Cat))
{
    Write-Host "knFcFlt[sign]: signing $Cat"
    & $signtool sign /fd SHA256 /sha1 $thumb $Cat
    if ($LASTEXITCODE -ne 0)
    {
        throw "knFcFlt[sign]: signtool failed on $Cat (exit $LASTEXITCODE)"
    }
}

Write-Host "knFcFlt[sign]: done  (cert thumbprint $thumb)"
