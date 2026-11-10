#!/bin/bash
# Adds a null check to SurfaceFlinger::getPhysicalDisplayIdsLocked() for waydroid/waydroid#2464.
# It calls getDefaultDisplayDeviceLocked()->getPhysicalId() without one, and while a resize
# reconnects the display the default display is missing, so a getPhysicalDisplayIds call in that
# gap crashes SurfaceFlinger. The patch fits only the SurfaceFlinger of the
# lineage-20.0-20260927 x86_64 system image, which it reads from system.img with debugfs.
#
# usage: surfaceflinger.sh install|uninstall, as root. stack/install.sh and stack/uninstall.sh
# run it. install skips other images, and removes its own copy once the image has changed.
# uninstall removes only its own copy. Restart Waydroid afterwards: waydroid session stop.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }
images=$(sed -n 's/^images_path *= *//p' /var/lib/waydroid/waydroid.cfg 2> /dev/null || true)
image=${WAYDROID_IMAGES:-${images:-/var/lib/waydroid/images}}/system.img
installed=/var/lib/waydroid/overlay/system/bin/surfaceflinger
original_sha=23ff152783a3f026e482b453bc7cd50b8770893e31bb58a0aae96a5378e30248
patched_sha=8d344d6f02eb3fb1139eb9d87e974b562b5c28c4a9ade8675f92d44642ab13d6

sha() { sha256sum "$1" | cut -d' ' -f1; }
ours() { [[ -f $installed && $(sha "$installed") == "$patched_sha" ]]; }

case ${1:-} in
uninstall)
    if ours; then
        rm "$installed"
        echo "removed the patched system/bin/surfaceflinger"
    fi
    exit 0
    ;;
install) ;;
*)
    echo "usage: $0 install|uninstall" >&2
    exit 2
    ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
debugfs -R "dump /system/bin/surfaceflinger $tmp/surfaceflinger" "$image" > /dev/null 2>&1 || true
if [[ ! -s $tmp/surfaceflinger || $(sha "$tmp/surfaceflinger") != "$original_sha" ]]; then
    # a copy from an older image would replace the new image's SurfaceFlinger
    if ours; then
        rm "$installed"
        echo "removed the patched system/bin/surfaceflinger, which does not fit $image"
    fi
    echo "skipped the SurfaceFlinger null check: it fits only the 20260927 system image"
    exit 0
fi
if ours; then
    echo "system/bin/surfaceflinger already has the null check"
    exit 0
fi
if [[ -e $installed ]]; then
    echo "skipped the SurfaceFlinger null check: $installed exists and is not this patch" >&2
    exit 0
fi

# At 0x20c40f, `mov %r15,0x58(%rsp)` (a debug tag only) becomes `test %r15,%r15; je 0x20c45b`,
# which skips the default display's ID when there is no default display.
printf '\x4d\x85\xff\x74\x47' | dd of="$tmp/surfaceflinger" bs=1 seek=$((0x20b40f)) conv=notrunc status=none
[[ $(sha "$tmp/surfaceflinger") == "$patched_sha" ]] || { echo "the patched SurfaceFlinger does not match" >&2; exit 1; }
# a partial write would run in Android, and its hash would no longer mark it as ours
install -D -m 0755 -o root -g 2000 "$tmp/surfaceflinger" "$installed.new"
mv -f "$installed.new" "$installed"
echo "installed system/bin/surfaceflinger with the null check"
