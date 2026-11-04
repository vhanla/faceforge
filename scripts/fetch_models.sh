#!/usr/bin/env bash
# Download every model used by faceforge into ./models and build emap.bin.
#
#   SCRFD det_10g + ArcFace w600k_r50   <- InsightFace buffalo_l release
#   inswapper_128, GFPGAN 1.4,
#   Real-ESRGAN x2/x4                  <- FaceFusion model mirror on HF
#
# The models are NOT redistributed with this repository. Check the licenses
# before use: InsightFace weights and the inswapper model are non-commercial.
#
# Usage:
#   scripts/fetch_models.sh              # download + extract emap.bin
#   ONLY=inswapper scripts/fetch_models.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODELS="${ROOT}/models"
mkdir -p "${MODELS}"

BUFFALO_URL="https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_l.zip"
FACEFUSION_BASE="https://huggingface.co/facefusion/models-3.0.0/resolve/main"

want() {
  [[ -z "${ONLY:-}" || "${ONLY}" == "$1" ]]
}

fetch() {
  local url="$1" out="$2"
  if [[ -s "${out}" ]]; then
    echo "    ok ${out##*/} ($(du -h "${out}" | cut -f1))"
    return
  fi
  echo "    get ${url}"
  curl -L --fail --retry 3 --progress-bar -o "${out}.part" "${url}"
  mv "${out}.part" "${out}"
}

echo "==> models -> ${MODELS}"

if want scrfd or want arcface or want insightface; then
  if [[ ! -s "${MODELS}/det_10g.onnx" || ! -s "${MODELS}/w600k_r50.onnx" ]]; then
    ZIP="${TMPDIR:-/tmp}/buffalo_l.zip"
    echo "==> InsightFace buffalo_l (SCRFD + ArcFace)"
    if [[ ! -s "${ZIP}" ]]; then
      curl -L --fail --retry 3 --progress-bar -o "${ZIP}" "${BUFFALO_URL}"
    fi
    unzip -o -j "${ZIP}" \
      "det_10g.onnx" "w600k_r50.onnx" -d "${MODELS}" >/dev/null
    rm -f "${ZIP}"
  fi
fi

echo "==> FaceFusion ONNX exports"
want inswapper && fetch "${FACEFUSION_BASE}/inswapper_128.onnx" "${MODELS}/inswapper_128.onnx" || true
want gfpgan    && fetch "${FACEFUSION_BASE}/gfpgan_1.4.onnx"    "${MODELS}/gfpgan_1.4.onnx"    || true
want esrgan2   && fetch "${FACEFUSION_BASE}/real_esrgan_x2.onnx" "${MODELS}/real_esrgan_x2.onnx" || true
want esrgan4   && fetch "${FACEFUSION_BASE}/real_esrgan_x4.onnx" "${MODELS}/real_esrgan_x4.onnx" || true

if [[ -s "${MODELS}/inswapper_128.onnx" ]]; then
  echo "==> extracting emap.bin from inswapper_128.onnx"
  python3 "${ROOT}/scripts/extract_emap.py" \
    "${MODELS}/inswapper_128.onnx" -o "${MODELS}/emap.bin"
fi

echo
echo "==> done. Model directory:"
ls -lh "${MODELS}"
