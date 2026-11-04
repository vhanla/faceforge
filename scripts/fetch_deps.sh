#!/usr/bin/env bash
# Download the ONNX Runtime C/C++ SDK used by the native core.
#
# The GPU package is the "gpu_cuda13" build (ORT 1.31.x). It is dynamically
# linked against the CUDA 13 runtime and cuDNN 9, which must be installed on
# the machine for the CUDA provider to load. For a CPU-only machine, set
# ORT_FLAVOR=cpu (smaller download, no CUDA).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/third_party/onnxruntime"
VERSION="${ORT_VERSION:-1.31.1}"
FLAVOR="${ORT_FLAVOR:-gpu_cuda13}"

if [[ "${FLAVOR}" == "cpu" ]]; then
  PKG="onnxruntime-linux-x64-${VERSION}"
else
  PKG="onnxruntime-linux-x64-${FLAVOR}-${VERSION}"
fi

URL="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/${PKG}.tgz"
TMP="${TMPDIR:-/tmp}/faceforge-${PKG}.tgz"

if [[ -f "${DEST}/include/onnxruntime_cxx_api.h" ]]; then
  echo "ONNX Runtime already present in ${DEST}"
  exit 0
fi

echo "==> downloading ONNX Runtime ${VERSION} (${FLAVOR})"
echo "    ${URL}"
mkdir -p "${ROOT}/third_party"
curl -L --fail --retry 3 --progress-bar -o "${TMP}" "${URL}"

echo "==> extracting"
rm -rf "${ROOT}/third_party/${PKG}"
tar -xzf "${TMP}" -C "${ROOT}/third_party"
rm -rf "${DEST}"
mv "${ROOT}/third_party/${PKG}" "${DEST}"
rm -f "${TMP}"

if [[ ! -f "${DEST}/include/onnxruntime_cxx_api.h" ]]; then
  echo "error: extraction did not produce the expected include/ directory" >&2
  exit 1
fi

echo "==> ONNX Runtime ready at ${DEST}"
ldd "${DEST}/lib/libonnxruntime.so" 2>/dev/null | grep -E "cuda|cudnn" || true
