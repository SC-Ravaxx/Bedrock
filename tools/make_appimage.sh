#!/bin/bash
# Packs the Linux build into one AppImage: Bedrock-<version>-x86_64.AppImage.
#
#   tools/make_appimage.sh <build dir> [<out dir>]
#
# Needs linuxdeploy and its Qt plugin (github.com/linuxdeploy) on the PATH
# or in ~/bedrock-tools, and a build dir with a `bedrock` binary in it. The
# AppImage carries Qt, OpenCV, Tesseract and the rest of what ldd finds, the
# fonts and the models are inside the binary already, and the player's things
# go to $XDG_DATA_HOME/bedrock - nothing beside the file is needed or written.
#
# No FUSE needed to build: the two tools are run unpacked
# (APPIMAGE_EXTRACT_AND_RUN). The result needs FUSE to run, as every AppImage
# does, or --appimage-extract-and-run.
set -e
BUILD=${1:?build dir}
OUT=${2:-$BUILD}
SRC=$(cd "$(dirname "$0")/.." && pwd)
TOOLS=${BEDROCK_TOOLS:-$HOME/bedrock-tools}
export PATH="$TOOLS:$PATH"
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
# the same three numbers resource.h carries, with the fourth when it is not 0
VERSION=$(python3 - "$SRC/include/resource.h" <<'EOF'
import re, sys
h = open(sys.argv[1]).read()
n = [int(re.search(r'#define BEDROCK_VERSION_%s (\d+)' % k, h).group(1)) for k in ('MAJOR', 'MINOR', 'PATCH', 'BUILD')]
print('.'.join(map(str, n[:3])) + ('.%d' % n[3] if n[3] else ''))
EOF
)
APPDIR=$BUILD/AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$BUILD/bedrock" "$APPDIR/usr/bin/bedrock"
cp "$SRC/resources/linux/bedrock.desktop" "$APPDIR/usr/share/applications/"
cp "$SRC/resources/icon_256.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/bedrock.png"
# the licences travel with the binary, as they do beside Bedrock.exe
mkdir -p "$APPDIR/usr/share/doc/bedrock"
cp "$SRC/LICENSE" "$APPDIR/usr/share/doc/bedrock/"
[ -f "$SRC/THIRD-PARTY-NOTICES-LINUX.txt" ] && cp "$SRC/THIRD-PARTY-NOTICES-LINUX.txt" "$APPDIR/usr/share/doc/bedrock/"
cp "$SRC"/resources/fonts/LICENSE-*.txt "$APPDIR/usr/share/doc/bedrock/"
mkdir -p "$OUT"
export OUTPUT="$OUT/Bedrock-$VERSION-x86_64.AppImage"
# The platform plugins the window may need on a stranger's desktop: xcb and
# wayland both; linuxdeploy-plugin-qt takes xcb alone unless told
# (Qt 6.2 calls them libqwayland-generic.so and libqwayland-egl.so, 6.10
# libqwayland.so - whichever this Qt has is taken)
PLUGINS=$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms
EXTRA=""
for p in libqwayland.so libqwayland-generic.so libqwayland-egl.so libqoffscreen.so; do
    [ -f "$PLUGINS/$p" ] && EXTRA="$EXTRA;$p"
done
export EXTRA_PLATFORM_PLUGINS="${EXTRA#;}"
echo "platform plugins besides xcb: $EXTRA_PLATFORM_PLUGINS"
# The runtime at the front of the file: the static one from
# github.com/AppImage/type2-runtime, which mounts with fuse3 or fuse2 and
# unpacks itself where there is neither - Arch and SteamOS ship without
# libfuse2, and the old runtime linuxdeploy would use only speaks that
if [ -f "$TOOLS/runtime-x86_64" ]; then
    export LDAI_RUNTIME_FILE="$TOOLS/runtime-x86_64"
fi
linuxdeploy-x86_64.AppImage --appdir "$APPDIR" --plugin qt --output appimage
ls -la "$OUTPUT"
sha256sum "$OUTPUT"
