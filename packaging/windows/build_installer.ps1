#Requires -Version 5.1
<#
.SYNOPSIS
    Build the Windows Inno Setup installer from an existing zig-out\bin.

.DESCRIPTION
    Runs iscc with the version, source and output paths. This is the same
    script the release workflow uses, so it can be reproduced locally.

.EXAMPLE
    .\packaging\windows\build_installer.ps1 -Version 0.2.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Iscc = "iscc"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root "zig-out\bin"
$dist = Join-Path $root "dist"
$iss = Join-Path $PSScriptRoot "faceforge.iss"

if (-not (Test-Path (Join-Path $bin "faceforge-gui.exe"))) {
    throw "zig-out\bin\faceforge-gui.exe is missing; run zig build first."
}
New-Item -ItemType Directory -Force -Path $dist | Out-Null

& $Iscc `
    "/DVersion=$Version" `
    "/DSourceDir=$bin" `
    "/DOutputDir=$dist" `
    "/DRepoRoot=$root" `
    $iss
if ($LASTEXITCODE -ne 0) { throw "iscc failed with exit code $LASTEXITCODE" }

Get-ChildItem (Join-Path $dist "*.exe") | ForEach-Object { Write-Host "==> $($_.FullName)" }
