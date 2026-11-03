#!/usr/bin/env bash
# Run with sudo.  Restores Waydroid's software rendering and removes the
# render server user service.  Restarts a running Waydroid container.
set -euo pipefail

user=${SUDO_USER:?run with sudo}
home=$(getent passwd "$user" | cut -d: -f6)
overlay=/var/lib/waydroid/overlay/vendor
cfg=/var/lib/waydroid/waydroid.cfg
backup=/var/lib/waydroid/waydroid-venus-properties.json

python3 - "$cfg" "$backup" <<'EOF'
import configparser, json, os, sys
cfg_path, backup_path = sys.argv[1:]
keys = ("ro.hardware.egl", "ro.hardware.vulkan", "mesa.vn.debug",
        "mesa.vtest.socket.name")
previous = {}
if os.path.exists(backup_path):
    with open(backup_path) as f:
        previous = json.load(f)
cfg = configparser.ConfigParser()
cfg.read(cfg_path)
if cfg.has_section("properties"):
    for key in keys:
        if previous.get(key) is None:
            cfg.remove_option("properties", key)
        else:
            cfg["properties"][key] = previous[key]
with open(cfg_path, "w") as f:
    cfg.write(f)
EOF
rm -f "$backup"

for lib in lib64 lib; do
   rm -f "$overlay/$lib/hw/vulkan.virtio.so" "$overlay/$lib/hw/gralloc.default.so"
done
rm -f "$overlay/bin/waydroid-venus-recover.sh" "$overlay/etc/init/waydroid-venus.rc"
waydroid upgrade --offline

systemctl --user -M "$user@" disable --now waydroid-venus.service || true
rm -f "$home/.config/systemd/user/waydroid-venus.service"
systemctl --user -M "$user@" daemon-reload
