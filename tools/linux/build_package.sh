#!/bin/bash
# The Linux package, start to finish, inside the jammy root:
#
#   tools/linux/jammy_run.sh bash /mnt/c/<path to Bedrock>/tools/linux/build_package.sh [<out dir>]
#
# Configures and builds bedrock against the static OpenCV, runs the self-test
# out of the build folder, writes the third-party notices from what the
# AppDir carries, and packs Bedrock-<version>-x86_64.AppImage. The package
# itself is then tried from an empty folder: self-test and one picture.
set -e
SRC=${BEDROCK_SRC:-$(cd "$(dirname "$0")/../.." && pwd)}
BUILD=${BEDROCK_BUILD:-/work/bedrock}
OUT=${1:-$BUILD}
export QMAKE=/usr/bin/qmake6
mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DOpenCV_DIR=/opt/opencv-static/lib/cmake/opencv4 -DBEDROCK_UPDATE_URL="${BEDROCK_UPDATE_URL:-}" \
    -DBEDROCK_REFINERY_URL="${BEDROCK_REFINERY_URL:-}" > "$BUILD/configure.log" 2>&1 \
    || { tail -30 "$BUILD/configure.log"; exit 1; }
cmake --build "$BUILD" --target bedrock -j "$(nproc)" > "$BUILD/build.log" 2>&1 \
    || { grep -n "error" "$BUILD/build.log" | grep -v Werror | head -20; exit 1; }
echo "built $(ls -la "$BUILD/bedrock" | awk '{print $5}') bytes"
sha256sum "$BUILD/bedrock"
objdump -T "$BUILD/bedrock" | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
objdump -T "$BUILD/bedrock" | grep -o "GLIBCXX_[0-9.]*" | sort -uV | tail -1

cd "$BUILD"
QT_QPA_PLATFORM=offscreen ./bedrock --selftest > selftest.log 2>&1 || true
echo "build-folder self-test: PASS $(grep -c '\[PASS\]' selftest.log) FAIL $(grep -c '\[FAIL\]' selftest.log)"

# the AppDir first, the notices from it, then the AppDir again with the
# notices inside - the notices are about what is in the package
bash "$SRC/tools/make_appimage.sh" "$BUILD" "$OUT" > appimage.log 2>&1 || { tail -20 appimage.log; exit 1; }
python3 "$SRC/tools/gen_linux_notices.py" "$BUILD/AppDir" "$SRC/THIRD-PARTY-NOTICES-LINUX.txt"
bash "$SRC/tools/make_appimage.sh" "$BUILD" "$OUT" > appimage.log 2>&1 || { tail -20 appimage.log; exit 1; }
PACKAGE=$(ls -t "$OUT"/Bedrock-*-x86_64.AppImage | head -1)
ls -la "$PACKAGE"
sha256sum "$PACKAGE"

rm -rf /work/pkgtest && mkdir -p /work/pkgtest && cp "$PACKAGE" /work/pkgtest/ && cd /work/pkgtest
export APPIMAGE_EXTRACT_AND_RUN=1
QT_QPA_PLATFORM=offscreen "./$(basename "$PACKAGE")" --selftest > selftest.log 2>&1 || true
echo "package self-test: PASS $(grep -c '\[PASS\]' selftest.log) FAIL $(grep -c '\[FAIL\]' selftest.log)"
