#!/usr/bin/env bash
# Build the portable Linux tarball:
#   dist/faceforge-<version>-linux-x64.tar.gz
#
# The bundle carries the GUI, the core and the ONNX Runtime it links against,
# plus the model fetch scripts. It expects the OpenCV/FFmpeg/GStreamer runtime
# from the host distribution (see the .deb for a package that declares them).
#
#   packaging/linux/build_portable.sh <version>
set -euo pipefail

VERSION="${1:?usage: build_portable.sh <version>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST="${DIST:-$ROOT/dist}"
PKG="faceforge-${VERSION}-linux-x64"
STAGE="$DIST/$PKG"

if [[ ! -x "$ROOT/build-native/faceforge-gui" ]]; then
  echo "error: build-native/faceforge-gui is missing; build the GUI first." >&2
  exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/scripts"

install -m 0755 "$ROOT/build-native/faceforge-gui" "$STAGE/bin/"
install -m 0644 "$ROOT/build/libfaceforge.so" "$STAGE/lib/"
# -a keeps the .so -> .so.1 -> .so.1.x.y symlink chain instead of copying the
# real library three times.
cp -a "$ROOT"/third_party/onnxruntime/lib/libonnxruntime.so* "$STAGE/lib/"
install -m 0644 "$ROOT/README.md" "$STAGE/"
install -m 0755 "$ROOT/scripts/fetch_models.sh" "$STAGE/scripts/"
install -m 0644 "$ROOT/scripts/extract_emap.py" "$STAGE/scripts/"

# The GUI already searches $ORIGIN/../lib; point the core at the bundled ONNX
# Runtime next to it so the bundle does not depend on build-machine paths.
patchelf --set-rpath '$ORIGIN/../lib' "$STAGE/bin/faceforge-gui"
patchelf --set-rpath '$ORIGIN' "$STAGE/lib/libfaceforge.so"

tar -C "$DIST" -czf "$DIST/$PKG.tar.gz" "$PKG"
rm -rf "$STAGE"
echo "==> $DIST/$PKG.tar.gz"
