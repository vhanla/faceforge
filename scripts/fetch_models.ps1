#Requires -Version 5.1
<#
.SYNOPSIS
    Download every model used by faceforge into .\models and build emap.bin.

.DESCRIPTION
    SCRFD det_10g + ArcFace w600k_r50 come from the InsightFace buffalo_l
    release; inswapper_128, GFPGAN 1.4 and Real-ESRGAN x2/x4 come from the
    FaceFusion model mirror on Hugging Face.

    The weights are NOT redistributed with this repository. InsightFace
    weights and inswapper are non-commercial; check the licenses before use.

    Requires Python (for emap extraction) with the `onnx` and `numpy` packages:
        python -m pip install onnx numpy

.EXAMPLE
    .\scripts\fetch_models.ps1
    .\scripts\fetch_models.ps1 -Only inswapper
#>
[CmdletBinding()]
param(
    [string]$Only = $env:ONLY
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$models = Join-Path $root "models"
New-Item -ItemType Directory -Force -Path $models | Out-Null

$buffaloUrl = "https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_l.zip"
$facefusion = "https://huggingface.co/facefusion/models-3.0.0/resolve/main"

function Test-Wanted {
    param([string]$Name)
    return (-not $Only) -or ($Only -eq $Name)
}

function Get-File {
    param([string]$Url, [string]$OutFile)
    if ((Test-Path $OutFile) -and ((Get-Item $OutFile).Length -gt 0)) {
        Write-Host ("    ok {0} ({1:N1} MB)" -f (Split-Path $OutFile -Leaf), ((Get-Item $OutFile).Length / 1MB))
        return
    }
    Write-Host "    get $Url"
    $oldProgress = $ProgressPreference
    $ProgressPreference = "SilentlyContinue"
    try {
        Invoke-WebRequest -Uri $Url -OutFile "$OutFile.part" -UseBasicParsing
    } finally {
        $ProgressPreference = $oldProgress
    }
    Move-Item -Path "$OutFile.part" -Destination $OutFile -Force
}

Write-Host "==> models -> $models"

$needInsightFace = (Test-Wanted "scrfd") -or (Test-Wanted "arcface") -or (Test-Wanted "insightface")
if ($needInsightFace -and
    (-not (Test-Path (Join-Path $models "det_10g.onnx")) -or
     -not (Test-Path (Join-Path $models "w600k_r50.onnx")))) {
    $zip = Join-Path ([System.IO.Path]::GetTempPath()) "buffalo_l.zip"
    $extract = Join-Path ([System.IO.Path]::GetTempPath()) "faceforge-buffalo_l"
    Write-Host "==> InsightFace buffalo_l (SCRFD + ArcFace)"
    if (-not (Test-Path $zip)) {
        Get-File -Url $buffaloUrl -OutFile $zip
    }
    if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
    Expand-Archive -Path $zip -DestinationPath $extract -Force
    Copy-Item -Path (Join-Path $extract "det_10g.onnx") -Destination $models -Force
    Copy-Item -Path (Join-Path $extract "w600k_r50.onnx") -Destination $models -Force
    Remove-Item -Recurse -Force $extract
}

Write-Host "==> FaceFusion ONNX exports"
if (Test-Wanted "inswapper") {
    Get-File -Url "$facefusion/inswapper_128.onnx" -OutFile (Join-Path $models "inswapper_128.onnx")
}
if (Test-Wanted "gfpgan") {
    Get-File -Url "$facefusion/gfpgan_1.4.onnx" -OutFile (Join-Path $models "gfpgan_1.4.onnx")
}
if (Test-Wanted "esrgan2") {
    Get-File -Url "$facefusion/real_esrgan_x2.onnx" -OutFile (Join-Path $models "real_esrgan_x2.onnx")
}
if (Test-Wanted "esrgan4") {
    Get-File -Url "$facefusion/real_esrgan_x4.onnx" -OutFile (Join-Path $models "real_esrgan_x4.onnx")
}

$inswapper = Join-Path $models "inswapper_128.onnx"
if ((Test-Path $inswapper) -and ((Get-Item $inswapper).Length -gt 0)) {
    Write-Host "==> extracting emap.bin from inswapper_128.onnx"
    $python = if (Get-Command python -ErrorAction SilentlyContinue) { "python" } else { "py" }
    & $python (Join-Path $root "scripts\extract_emap.py") $inswapper -o (Join-Path $models "emap.bin")
    if ($LASTEXITCODE -ne 0) { throw "extract_emap.py failed" }
}

Write-Host ""
Write-Host "==> done. Model directory:"
Get-ChildItem $models | Format-Table Name, Length -AutoSize