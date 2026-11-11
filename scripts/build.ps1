#Requires -Version 5.1
<#
.SYNOPSIS
    Build the C++ core (ONNX Runtime + OpenCV) and the native C++ GUI on Windows.

.DESCRIPTION
    OpenCV is resolved in this order:
      1. -OpenCVDir <path> (folder containing OpenCVConfig.cmake)
      2. vcpkg, when VCPKG_ROOT or VCPKG_INSTALLATION_ROOT is set (vcpkg.json)
      3. an OpenCV installation already visible to CMake

    The ONNX Runtime SDK must be in third_party\onnxruntime (fetch_deps.ps1).

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\build.ps1 -CoreOnly
    .\scripts\build.ps1 -OpenCVDir C:\opencv\build
#>
[CmdletBinding()]
param(
    [switch]$CoreOnly,
    [switch]$GuiOnly,
    [string]$OpenCVDir = "",
    [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $ortHeader = Join-Path $root "third_party\onnxruntime\include\onnxruntime_cxx_api.h"
    if (-not (Test-Path $ortHeader)) {
        throw "ONNX Runtime is missing. Run .\scripts\fetch_deps.ps1 first."
    }
    if (-not (Test-Path (Join-Path $root "models\det_10g.onnx")) -or
        -not (Test-Path (Join-Path $root "models\inswapper_128.onnx"))) {
        Write-Warning "models are missing. Run .\scripts\fetch_models.ps1 first."
    }

    if ($Jobs -le 0) {
        $Jobs = if ($env:NUMBER_OF_PROCESSORS) { [int]$env:NUMBER_OF_PROCESSORS } else { 4 }
    }

    if (-not $GuiOnly) {
        $configure = @(
            "-S", ".", "-B", "build",
            "-DCMAKE_BUILD_TYPE=Release"
        )
        if ($OpenCVDir) {
            $configure += "-DOpenCV_DIR=$OpenCVDir"
        } elseif ($env:OpenCV_DIR) {
            $configure += "-DOpenCV_DIR=$env:OpenCV_DIR"
        } elseif ($env:VCPKG_ROOT -or $env:VCPKG_INSTALLATION_ROOT) {
            $vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { $env:VCPKG_INSTALLATION_ROOT }
            $toolchain = Join-Path $vcpkg "scripts\buildsystems\vcpkg.cmake"
            if (Test-Path $toolchain) {
                $configure += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
            } else {
                Write-Warning "vcpkg found but $toolchain does not exist; relying on CMake's OpenCV lookup."
            }
        }

        Write-Host "==> configuring core (CMake)"
        & cmake @configure
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

        Write-Host "==> building core + ffbench"
        & cmake --build build --config Release -j $Jobs
        if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
    }

    if (-not $CoreOnly) {
        if (-not $GuiOnly) {
            $coreLib = Join-Path $root "build\faceforge.lib"
            if (-not (Test-Path $coreLib)) {
                throw "The core is not built. Run .\scripts\build.ps1 -CoreOnly first."
            }
        }
        Write-Host "==> configuring GUI (native C++/AMUIE)"
        & cmake -S gui-native -B build-native
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

        Write-Host "==> building GUI"
        & cmake --build build-native --config Release -j $Jobs
        if ($LASTEXITCODE -ne 0) { throw "GUI build failed" }
        Write-Host "==> binary: build-native\faceforge-gui.exe"
    }

    Write-Host "==> build complete"
} finally {
    Pop-Location
}