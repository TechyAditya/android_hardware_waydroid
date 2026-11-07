#!/bin/bash
# Lets Back leave an app's first screen on displays other than Android's main one.
# Waydroid's framework patch 0032 ignores Back on a task's root activity unless the full UI is
# active, so in waydroid-app-displays windows Back did nothing on an app's first screen. This adds a
# display check to ActivityClientController.onBackPressed() in services.jar: the block stays on
# display 0, and other displays get Android's own handling (finish the activity, or move a launcher
# activity's task to the back, which closes its window). The patch fits only the services.jar of
# the lineage-20.0-20260927 x86_64 system image, which it reads from system.img with debugfs.
#
# usage: services.sh install|uninstall, as root. stack/install.sh and stack/uninstall.sh run it.
# install skips other images, and removes its own copy once the image has changed. uninstall
# removes only its own copy. Restart Waydroid afterwards: waydroid session stop.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=${WORK:-$HERE/build}/tools
images=$(sed -n 's/^images_path *= *//p' /var/lib/waydroid/waydroid.cfg 2> /dev/null || true)
image=${WAYDROID_IMAGES:-${images:-/var/lib/waydroid/images}}/system.img
installed=/var/lib/waydroid/overlay/system/framework/services.jar
original_sha=2634ae4c9f2d8fcb9e3cf269606bd8f0e99ae8b53dd04beaa409dde7842a72cb
patched_sha=c35e2c60a4445d64afac4685f368354af9c0513bb2ba5bafd28f55707c90c8a4

sha() { sha256sum "$1" | cut -d' ' -f1; }
ours() { [[ -f $installed && $(sha "$installed") == "$patched_sha" ]]; }

case ${1:-} in
uninstall)
    if ours; then
        rm "$installed"
        echo "removed the patched system/framework/services.jar"
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
debugfs -R "dump /system/framework/services.jar $tmp/services.jar" "$image" > /dev/null 2>&1 || true
if [[ ! -s $tmp/services.jar || $(sha "$tmp/services.jar") != "$original_sha" ]]; then
    # a copy from an older image would replace the new image's framework
    if ours; then
        rm "$installed"
        echo "removed the patched system/framework/services.jar, which does not fit $image"
    fi
    echo "skipped the Back patch for app windows: it fits only the 20260927 system image"
    exit 0
fi
if ours; then
    echo "system/framework/services.jar already has the Back patch"
    exit 0
fi
if [[ -e $installed ]]; then
    echo "skipped the Back patch for app windows: $installed exists and is not this patch" >&2
    exit 0
fi
command -v java > /dev/null || { echo "skipped the Back patch for app windows: it needs java" >&2; exit 0; }

fetch() { # file sha256
    local file=$TOOLS/$1
    if [[ ! -f $file ]]; then
        mkdir -p "$TOOLS"
        curl -fsSL --retry 5 -o "$file.part" "https://bitbucket.org/JesusFreke/smali/downloads/$1"
        mv "$file.part" "$file"
    fi
    echo "$2  $file" | sha256sum -c --quiet
    chown -R --reference="$(dirname "$TOOLS")" "$TOOLS"
}
fetch baksmali-2.5.2.jar d3116248cce4f82ec5a31eb7f95ee75daff42ddf6eed0ab573973dc53fbad2e5
fetch smali-2.5.2.jar 9544299578b16f771d8aa8eaefe0d3718ca03478c16f3c356f2fcf1366bfb116

(cd "$tmp" && unzip -q services.jar classes2.dex)
java -jar "$TOOLS/baksmali-2.5.2.jar" d --api 33 -o "$tmp/smali" "$tmp/classes2.dex"
python3 - "$tmp/smali/com/android/server/wm/ActivityClientController.smali" << 'EOF'
import sys
path = sys.argv[1]
text = open(path).read()
start = text.index(".method public onBackPressed(Landroid/os/IBinder;Landroid/app/IRequestFinishCallback;)V")
end = text.index(".end method", start)
# v5 holds isTaskRoot and v3 the activity. After this point v5 only gates Waydroid's early return,
# so clearing it off display 0 leaves the rest of the method as in AOSP.
old = """    :cond_53
    :try_start_53
    invoke-direct {p0, v3}, Lcom/android/server/wm/ActivityClientController;->isTopActivityInTaskFragment(Lcom/android/server/wm/ActivityRecord;)Z
"""
new = """    :cond_53
    :try_start_53
    if-eqz v5, :waydroid_main_display_only

    invoke-virtual {v3}, Lcom/android/server/wm/ActivityRecord;->getDisplayId()I

    move-result v8

    if-eqz v8, :waydroid_main_display_only

    const/4 v5, 0x0

    :waydroid_main_display_only
    invoke-direct {p0, v3}, Lcom/android/server/wm/ActivityClientController;->isTopActivityInTaskFragment(Lcom/android/server/wm/ActivityRecord;)Z
"""
body = text[start:end]
assert body.count(old) == 1, "onBackPressed does not look as expected"
open(path, "w").write(text[:start] + body.replace(old, new) + text[end:])
EOF
# One thread, so the output is the same on every machine and matches patched_sha.
java -jar "$TOOLS/smali-2.5.2.jar" a -j 1 --api 33 -o "$tmp/classes2.patched.dex" "$tmp/smali"

# The dex files stay stored and 4-byte aligned, as in the image, so ART can map them.
python3 - "$tmp/services.jar" "$tmp/classes2.patched.dex" "$tmp/services.patched.jar" << 'EOF'
import sys, zipfile
src, dex, dst = sys.argv[1:]
with zipfile.ZipFile(src) as zin, open(dst, "wb") as out, zipfile.ZipFile(out, "w") as zout:
    for info in zin.infolist():
        data = open(dex, "rb").read() if info.filename == "classes2.dex" else zin.read(info.filename)
        entry = zipfile.ZipInfo(info.filename, date_time=info.date_time)
        entry.compress_type = info.compress_type
        entry.external_attr = info.external_attr
        if entry.compress_type == zipfile.ZIP_STORED:
            pad = -(out.tell() + 30 + len(entry.filename.encode())) % 4
            if pad:
                pad += 4  # room for the extra field's own header
                entry.extra = b"\x35\xd9" + (pad - 4).to_bytes(2, "little") + b"\x00" * (pad - 4)
        zout.writestr(entry, data)
EOF
built=$(sha "$tmp/services.patched.jar")
[[ $built == "$patched_sha" ]] || { echo "the patched services.jar does not match: $built" >&2; exit 1; }
install -D -m 0644 -o root -g root "$tmp/services.patched.jar" "$installed"
echo "installed system/framework/services.jar with the Back patch for app windows"
