#!/usr/bin/env bash
# Build a Debian package:
#   dist/faceforge_<version>_<arch>.deb
#
# The package bundles the GUI, the core and the ONNX Runtime it links against.
# OpenCV, FFmpeg and the GL stack come from the distribution; their runtime
# dependencies are derived automatically with dpkg-shlibdeps.
#
#   packaging/linux/build_deb.sh <version>
set -euo pipefail

VERSION="${1:?usage: build_deb.sh <version>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST="${DIST:-$ROOT/dist}"
ARCH="${ARCH:-amd64}"
PKG="faceforge_${VERSION}_${ARCH}"
STAGE="$DIST/$PKG"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [[ ! -x "$ROOT/zig-out/bin/faceforge-gui" ]]; then
  echo "error: zig-out/bin/faceforge-gui is missing; build first." >&2
  exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/usr/bin" \
         "$STAGE/usr/lib/faceforge" \
         "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/icons/hicolor/256x256/apps" \
         "$STAGE/usr/share/faceforge/scripts"

install -m 0755 "$ROOT/zig-out/bin/faceforge-gui" "$STAGE/usr/bin/faceforge-gui"
install -m 0644 "$ROOT/zig-out/lib/libfaceforge.so" "$STAGE/usr/lib/faceforge/libfaceforge.so"
# -a keeps the .so -> .so.1 -> .so.1.x.y symlink chain instead of copying the
# real library three times.
cp -a "$ROOT"/third_party/onnxruntime/lib/libonnxruntime.so* "$STAGE/usr/lib/faceforge/"
install -m 0755 "$ROOT/packaging/linux/faceforge-launcher" "$STAGE/usr/bin/faceforge-launcher"
install -m 0755 "$ROOT/packaging/linux/faceforge-fetch-models" "$STAGE/usr/bin/faceforge-fetch-models"
install -m 0644 "$ROOT/packaging/linux/faceforge.desktop" "$STAGE/usr/share/applications/faceforge.desktop"
install -m 0644 "$ROOT/packaging/icons/faceforge.png" \
  "$STAGE/usr/share/icons/hicolor/256x256/apps/faceforge.png"
install -m 0644 "$ROOT/scripts/fetch_models.sh" "$STAGE/usr/share/faceforge/scripts/"
install -m 0644 "$ROOT/scripts/extract_emap.py" "$STAGE/usr/share/faceforge/scripts/"

# The GUI finds the core in ../lib/faceforge; the core finds the bundled ONNX
# Runtime next to itself.
patchelf --set-rpath '$ORIGIN/../lib/faceforge' "$STAGE/usr/bin/faceforge-gui"
patchelf --set-rpath '$ORIGIN' "$STAGE/usr/lib/faceforge/libfaceforge.so"

# Derive Depends from the ELF headers. shlibs.local tells dpkg-shlibdeps that
# the versioned bundled library (libonnxruntime.so.1) is provided by this
# package itself and needs no external dependency; the unversioned
# libfaceforge.so is ignored automatically. A line without a dependency field
# is intentional: it suppresses the "no dependency information" error without
# adding a self-dependency.
mkdir -p "$WORK/debian"
cat > "$WORK/debian/control" <<'CTRL'
Source: faceforge
Section: graphics
Priority: optional
Maintainer: vhanla <vhanla@users.noreply.github.com>
Standards-Version: 4.6.0
Package: faceforge
Architecture: any
Description: placeholder
CTRL
cat > "$WORK/debian/shlibs.local" <<'SHLIBS'
libonnxruntime 1
SHLIBS

DEPS="$(cd "$WORK" && LD_LIBRARY_PATH="$STAGE/usr/lib/faceforge" \
  dpkg-shlibdeps -O \
    "$STAGE/usr/bin/faceforge-gui" \
    "$STAGE/usr/lib/faceforge/libfaceforge.so" \
  | sed -n 's/^shlibs:Depends=//p')"
if [[ -z "$DEPS" ]]; then
  echo "error: dpkg-shlibdeps produced no dependencies" >&2
  exit 1
fi

SIZE="$(du -sk "$STAGE" | cut -f1)"

cat > "$STAGE/DEBIAN/control" <<CTRL
Package: faceforge
Version: ${VERSION}
Section: graphics
Priority: optional
Architecture: ${ARCH}
Maintainer: vhanla <vhanla@users.noreply.github.com>
Homepage: https://github.com/vhanla/faceforge
Depends: ${DEPS}
Recommends: ffmpeg
Installed-Size: ${SIZE}
Description: Native face restoration and face swap
 FaceForge detects faces, swaps identities with inswapper_128 and can
 restore the result with GFPGAN 1.4 or Real-ESRGAN, using ONNX Runtime
 and OpenCV behind a Zig + DVUI desktop UI.
 .
 Model weights are not included (non-commercial licenses). Run
 faceforge-fetch-models to download them into a per-user data directory.
CTRL

dpkg-deb --build --root-owner-group "$STAGE" "$DIST/${PKG}.deb"
rm -rf "$STAGE"
echo "==> $DIST/${PKG}.deb"
