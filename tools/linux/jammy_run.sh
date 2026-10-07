#!/bin/bash
# Runs a command inside the jammy build root (see jammy_setup.sh), with the
# Windows drive and the tools folder mounted where the scripts expect them:
#   /mnt/c       the same as outside (the sources somewhere under it)
#   /opt/tools   ~/bedrock-tools (linuxdeploy and its Qt plugin)
#   /work        the build folders, kept between runs
ROOT=${JAMMY_ROOT:-/srv/jammy}
TOOLS=${BEDROCK_TOOLS:-$HOME/bedrock-tools}
mount_once() {
    mountpoint -q "$ROOT$2" 2>/dev/null || mount $1 "$ROOT$2"
}
mount_once "-t proc proc" /proc
mount_once "-t sysfs sys" /sys
mount_once "--bind /dev" /dev
mount_once "-t devpts devpts" /dev/pts
mount_once "--bind /mnt/c" /mnt/c
mkdir -p "$TOOLS" "$ROOT/opt/tools"
mount_once "--bind $TOOLS" /opt/tools
cp /etc/resolv.conf "$ROOT/etc/resolv.conf" 2>/dev/null
exec chroot "$ROOT" /usr/bin/env -i HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/opt/tools TERM=xterm LANG=C.UTF-8 "$@"
