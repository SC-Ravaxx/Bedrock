#!/bin/bash
# A build root for the Linux package: Ubuntu 22.04 (jammy) in a chroot.
#
# The AppImage carries its own Qt, OpenCV and Tesseract, but it cannot carry
# glibc, and it should not carry libstdc++: both come from the machine it
# runs on, and a binary built against newer ones than that machine has does
# not start ("GLIBC_2.38 not found"). Built on WSL's Ubuntu 26.04 the package
# wanted glibc 2.38, which is Ubuntu 24.04 and newer, nothing older. The
# usual answer is to build on the oldest base one is willing to support:
# jammy's glibc 2.35 and GCC 11 cover every distribution of 2022 and later.
#
#   sudo tools/linux/jammy_setup.sh            makes /srv/jammy, once
#   tools/linux/jammy_run.sh <command...>      runs a command inside it
#
# Everything inside is Ubuntu's own packages except OpenCV, which is built
# from source as static libraries with only what Bedrock uses (see
# opencv_static.sh) - Ubuntu's OpenCV drags GDAL, HDF5 and half a desktop
# into the package, and a version too old to run the second reader's model.
set -e
ROOT=${JAMMY_ROOT:-/srv/jammy}
MIRROR=${JAMMY_MIRROR:-http://archive.ubuntu.com/ubuntu}

if [ ! -x /usr/sbin/debootstrap ]; then
    apt-get update -q && apt-get install -y -q debootstrap
fi
if [ ! -f "$ROOT/etc/os-release" ]; then
    debootstrap --variant=minbase jammy "$ROOT" "$MIRROR"
fi
cat > "$ROOT/etc/apt/sources.list" <<EOF
deb $MIRROR jammy main universe
deb $MIRROR jammy-updates main universe
deb http://security.ubuntu.com/ubuntu jammy-security main universe
EOF
cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
mkdir -p "$ROOT/opt/tools" "$ROOT/mnt/c" "$ROOT/work"

HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/jammy_run.sh" bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends \
    build-essential cmake ninja-build pkg-config git python3 file curl ca-certificates \
    libx11-dev libxcomposite-dev libxext-dev libxfixes-dev libxcb1-dev \
    qt6-base-dev qt6-base-dev-tools qt6-wayland libqt6svg6 \
    libgl-dev libglx-dev libegl-dev libxkbcommon-dev libfontconfig1-dev \
    libleptonica-dev libtesseract-dev libzstd-dev \
    zlib1g-dev libpng-dev libjpeg-turbo8-dev \
    patchelf desktop-file-utils
gcc --version | head -1
qmake6 -query QT_VERSION
'
echo "jammy is ready at $ROOT"
