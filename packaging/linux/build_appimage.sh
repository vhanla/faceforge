#!/usr/bin/env bash
# Build a portable AppImage:
#   dist/faceforge-<version>-x86_64.AppImage
#
# linuxdeploy copies the linked OpenCV/FFmpeg/GStreamer libraries into the
# AppDir and appimagetool packs it. The custom AppRun keeps the working
# directory in a per-user data folder so the default "models" path is
# writable. Video decoding still needs the host's GStreamer plugins.
#
#   packaging/linux/build_appimage.sh <version>
set -euo pipefail

VERSION="${1:?usage: build_appimage.sh <version>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST="${DIST:-$ROOT/dist}"
TOOLS="${TOOLS:-$ROOT/.cache/appimage-tools}"
ARCH="x86_64"

if [[ ! -x "$ROOT/build-native/faceforge-gui" ]]; then
  echo "error: build-native/faceforge-gui is missing; build the GUI first." >&2
  exit 1
fi

mkdir -p "$TOOLS" "$DIST"

fetch_tool() {
  local name="$1" url="$2" out="$TOOLS/$name"
  if [[ ! -x "$out" ]]; then
    echo "==> fetching $name"
    curl -L --fail --retry 3 -o "$out" "$url"
    chmod +x "$out"
  fi
}
fetch_tool linuxdeploy \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
fetch_tool appimagetool \
  "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"

# AppImages need FUSE to self-mount; extract and run instead so CI works in
# containers without it.
export APPIMAGE_EXTRACT_AND_RUN=1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
APPDIR="$WORK/AppDir"
mkdir -p "$APPDIR/usr/bin" \
         "$APPDIR/usr/lib" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"

install -m 0755 "$ROOT/build-native/faceforge-gui" "$APPDIR/usr/bin/faceforge-gui"
install -m 0644 "$ROOT/build/libfaceforge.so" "$APPDIR/usr/lib/libfaceforge.so"
install -m 0644 "$ROOT"/third_party/onnxruntime/lib/libonnxruntime.so* "$APPDIR/usr/lib/"
install -m 0644 "$ROOT/packaging/icons/faceforge.png" \
  "$APPDIR/usr/share/icons/hicolor/256x256/apps/faceforge.png"
# The AppImage ships its own launch wrapper, so the desktop entry points at
# the real binary rather than the .deb launcher.
sed 's/^Exec=.*/Exec=faceforge-gui/' \
  "$ROOT/packaging/linux/faceforge.desktop" \
  > "$APPDIR/usr/share/applications/faceforge.desktop"

export LD_LIBRARY_PATH="$APPDIR/usr/lib:$ROOT/third_party/onnxruntime/lib:${LD_LIBRARY_PATH:-}"

"$TOOLS/linuxdeploy" \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/faceforge-gui" \
  --desktop-file "$APPDIR/usr/share/applications/faceforge.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/faceforge.png"

# linuxdeploy generates a plain AppRun; replace it so the app always starts in
# a per-user, writable data directory.
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/sh
HERE="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
export LD_LIBRARY_PATH="$HERE/usr/lib:${LD_LIBRARY_PATH:-}"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/faceforge"
mkdir -p "$DATA" 2>/dev/null || true
cd "$DATA" 2>/dev/null || cd "$HOME"
exec "$HERE/usr/bin/faceforge-gui" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

ARCH="$ARCH" "$TOOLS/appimagetool" "$APPDIR" "$DIST/faceforge-${VERSION}-${ARCH}.AppImage"
echo "==> $DIST/faceforge-${VERSION}-${ARCH}.AppImage"
