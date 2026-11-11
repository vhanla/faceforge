#Requires -Version 5.1
<#
.SYNOPSIS
    Build the portable Windows zip from an existing build-native directory.

.EXAMPLE
    .\packaging\windows\build_portable.ps1 -Version 0.2.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root "build-native"
$dist = Join-Path $root "dist"
$pkg = "faceforge-$Version-windows-x64"
$stage = Join-Path $dist $pkg

if (-not (Test-Path (Join-Path $bin "faceforge-gui.exe"))) {
    throw "build-native\faceforge-gui.exe is missing; build the GUI first."
}
New-Item -ItemType Directory -Force -Path $dist | Out-Null
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }

New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item "$bin\*" $stage -Recurse
Copy-Item (Join-Path $root "README.md") $stage
Copy-Item (Join-Path $root "packaging\windows\faceforge.cmd") $stage
New-Item -ItemType Directory -Force -Path (Join-Path $stage "scripts") | Out-Null
Copy-Item (Join-Path $root "scripts\fetch_models.ps1") (Join-Path $stage "scripts")
Copy-Item (Join-Path $root "scripts\extract_emap.py") (Join-Path $stage "scripts")

$zip = Join-Path $dist "$pkg.zip"
Compress-Archive -Path $stage -DestinationPath $zip -Force
Remove-Item -Recurse -Force $stage
Write-Host "==> $zip"
