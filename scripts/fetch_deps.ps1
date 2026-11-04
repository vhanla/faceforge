#Requires -Version 5.1
<#
.SYNOPSIS
    Download the ONNX Runtime C/C++ SDK used by the native core.

.DESCRIPTION
    The default package is the CUDA 13 build (ORT 1.31.x). It is dynamically
    linked against the CUDA 13 runtime and cuDNN 9; those must be installed on
    the machine (or their DLLs on PATH) for the CUDA provider to load.

    For a CPU-only machine use -Flavor cpu (smaller download, no CUDA).

.EXAMPLE
    .\scripts\fetch_deps.ps1
    .\scripts\fetch_deps.ps1 -Flavor cpu
#>
[CmdletBinding()]
param(
    [string]$Version = "",
    [ValidateSet("gpu_cuda13", "gpu_cuda12", "cpu")]
    [string]$Flavor = ""
)

$ErrorActionPreference = "Stop"

if (-not $Version) {
    $Version = if ($env:ORT_VERSION) { $env:ORT_VERSION } else { "1.31.1" }
}
if (-not $Flavor) {
    $Flavor = if ($env:ORT_FLAVOR) { $env:ORT_FLAVOR } else { "gpu_cuda13" }
}

$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\onnxruntime"

if (Test-Path (Join-Path $dest "include\onnxruntime_cxx_api.h")) {
    Write-Host "ONNX Runtime already present in $dest"
    exit 0
}

if ($Flavor -eq "cpu") {
    $pkg = "onnxruntime-win-x64-$Version"
} else {
    $pkg = "onnxruntime-win-x64-$Flavor-$Version"
}

$url = "https://github.com/microsoft/onnxruntime/releases/download/v$Version/$pkg.zip"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "$pkg.zip"
$stage = Join-Path $root "third_party"

Write-Host "==> downloading ONNX Runtime $Version ($Flavor)"
Write-Host "    $url"
# Progress rendering makes Invoke-WebRequest extremely slow for large files.
$oldProgress = $ProgressPreference
$ProgressPreference = "SilentlyContinue"
try {
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
} finally {
    $ProgressPreference = $oldProgress
}

Write-Host "==> extracting"
Expand-Archive -Path $tmp -DestinationPath $stage -Force
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Move-Item -Path (Join-Path $stage $pkg) -Destination $dest
Remove-Item -Path $tmp -Force

if (-not (Test-Path (Join-Path $dest "include\onnxruntime_cxx_api.h"))) {
    throw "extraction did not produce the expected include/ directory"
}

Write-Host "==> ONNX Runtime ready at $dest"