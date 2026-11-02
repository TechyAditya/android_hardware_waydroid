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

if [ ! -f "$out/lib64/vulkan.virtio.so" ] || [ ! -f "$out/lib/gralloc.default.so" ] ||
   [ ! -x "$prefix/bin/virgl_test_server" ]; then
   echo "run build.sh first" >&2
   exit 1
fi

unit_dir=$home/.config/systemd/user
runuser -u "$user" -- mkdir -p "$unit_dir"
sed "s|@PREFIX@|$prefix|g" "$here/waydroid-venus.service" |
   runuser -u "$user" -- tee "$unit_dir/waydroid-venus.service" > /dev/null
systemctl --user -M "$user@" daemon-reload
systemctl --user -M "$user@" enable waydroid-venus.service
systemctl --user -M "$user@" restart waydroid-venus.service

for lib in lib64 lib; do
   for module in vulkan.virtio.so gralloc.default.so; do
      install -D -m 0644 "$out/$lib/$module" "/var/lib/waydroid/overlay/vendor/$lib/hw/$module"
   done
done
install -D -m 0755 "$here/android/waydroid-venus-recover.sh" \
   /var/lib/waydroid/overlay/vendor/bin/waydroid-venus-recover.sh
install -D -m 0644 "$here/android/waydroid-venus.rc" \
   /var/lib/waydroid/overlay/vendor/etc/init/waydroid-venus.rc

python3 - /var/lib/waydroid/waydroid.cfg <<'EOF'
import configparser, sys
cfg = configparser.ConfigParser()
cfg.read(sys.argv[1])
if not cfg.has_section("properties"):
    cfg.add_section("properties")
cfg["properties"].update({
    "ro.hardware.egl": "angle",
    "ro.hardware.vulkan": "virtio",
    "mesa.vn.debug": "vtest",
    "mesa.vtest.socket.name": "/data/venus/vtest.sock",
})
with open(sys.argv[1], "w") as f:
    cfg.write(f)
EOF

# regenerates waydroid_base.prop and restarts a running container
waydroid upgrade --offline
