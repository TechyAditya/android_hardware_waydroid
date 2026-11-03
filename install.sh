#!/usr/bin/env bash
# Run with sudo after build.sh.  Installs the Venus driver into Waydroid,
# starts the render server as a user service and restarts a running
# Waydroid container to apply the new properties.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
user=${SUDO_USER:?run with sudo}
home=$(getent passwd "$user" | cut -d: -f6)
prefix=${PREFIX:-$home/.local/lib/waydroid-venus}
out=$here/build/out
overlay=/var/lib/waydroid/overlay/vendor
cfg=/var/lib/waydroid/waydroid.cfg
# property values from before the first install, for uninstall.sh
backup=/var/lib/waydroid/waydroid-venus-properties.json

missing=0
for file in "$out"/{lib64,lib}/{vulkan.virtio.so,gralloc.default.so} \
   "$prefix/bin/virgl_test_server" "$prefix/libexec/virgl_render_server" \
   "$here/android/waydroid-venus-recover.sh" "$here/android/waydroid-venus.rc"; do
   if [ ! -f "$file" ]; then
      echo "missing $file" >&2
      missing=1
   fi
done
if [ "$missing" = 1 ]; then
   echo "run build.sh first" >&2
   exit 1
fi
if [ ! -f "$cfg" ]; then
   echo "$cfg not found; initialize Waydroid first" >&2
   exit 1
fi

for lib in lib64 lib; do
   for module in vulkan.virtio.so gralloc.default.so; do
      install -D -m 0644 "$out/$lib/$module" "$overlay/$lib/hw/$module"
   done
done
install -D -m 0755 "$here/android/waydroid-venus-recover.sh" \
   "$overlay/bin/waydroid-venus-recover.sh"
install -D -m 0644 "$here/android/waydroid-venus.rc" \
   "$overlay/etc/init/waydroid-venus.rc"

python3 - "$cfg" "$backup" <<'EOF'
import configparser, json, os, sys
cfg_path, backup_path = sys.argv[1:]
props = {
    "ro.hardware.egl": "angle",
    "ro.hardware.vulkan": "virtio",
    "mesa.vn.debug": "vtest",
    "mesa.vtest.socket.name": "/data/venus/vtest.sock",
}
cfg = configparser.ConfigParser()
cfg.read(cfg_path)
if not cfg.has_section("properties"):
    cfg.add_section("properties")
if not os.path.exists(backup_path):
    # a value equal to ours comes from an install without a backup
    previous = {}
    for key, value in props.items():
        current = cfg["properties"].get(key)
        previous[key] = None if current == value else current
    with open(backup_path, "w") as f:
        json.dump(previous, f)
cfg["properties"].update(props)
with open(cfg_path, "w") as f:
    cfg.write(f)
EOF

unit_dir=$home/.config/systemd/user
runuser -u "$user" -- mkdir -p "$unit_dir"
sed "s|@PREFIX@|$prefix|g" "$here/waydroid-venus.service" |
   runuser -u "$user" -- tee "$unit_dir/waydroid-venus.service" > /dev/null
systemctl --user -M "$user@" daemon-reload
systemctl --user -M "$user@" enable waydroid-venus.service
systemctl --user -M "$user@" restart waydroid-venus.service

# regenerates waydroid_base.prop and restarts a running container
waydroid upgrade --offline
