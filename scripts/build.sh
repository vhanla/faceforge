#!/usr/bin/env bash
# Build the C++ core (ONNX Runtime + OpenCV) and the Zig/DVUI GUI.
#
#   scripts/build.sh            # release core + GUI
#   scripts/build.sh --core     # only the core + ffbench
#   scripts/build.sh --gui      # only the GUI (assumes the core is built)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

MODE="${1:-all}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -f third_party/onnxruntime/include/onnxruntime_cxx_api.h ]]; then
  echo "error: ONNX Runtime is missing. Run scripts/fetch_deps.sh first." >&2
  exit 1
fi
if [[ ! -s models/det_10g.onnx || ! -s models/inswapper_128.onnx ]]; then
  echo "warning: models are missing. Run scripts/fetch_models.sh first." >&2
fi

if [[ "${MODE}" == "all" || "${MODE}" == "--core" ]]; then
  echo "==> configuring core (CMake)"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  echo "==> building core + ffbench"
  cmake --build build -j "${JOBS}"
fi

if [[ "${MODE}" == "all" || "${MODE}" == "--gui" ]]; then
  echo "==> building GUI (Zig + DVUI)"
  zig build
  echo "==> binary: zig-out/bin/faceforge-gui"
fi

echo "==> build complete"
