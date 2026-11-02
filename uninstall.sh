#!/usr/bin/env bash
# Run with sudo.  Restores Waydroid's software rendering and removes the
# render server user service.  Restarts a running Waydroid container.
set -euo pipefail

user=${SUDO_USER:?run with sudo}
home=$(getent passwd "$user" | cut -d: -f6)

python3 - /var/lib/waydroid/waydroid.cfg <<'EOF'
import configparser, sys
cfg = configparser.ConfigParser()
cfg.read(sys.argv[1])
if cfg.has_section("properties"):
    for key in ("ro.hardware.egl", "ro.hardware.vulkan", "mesa.vn.debug",
                "mesa.vtest.socket.name"):
        cfg.remove_option("properties", key)
with open(sys.argv[1], "w") as f:
    cfg.write(f)
EOF

rm -f /var/lib/waydroid/overlay/vendor/lib64/hw/vulkan.virtio.so
waydroid upgrade --offline

systemctl --user -M "$user@" disable --now waydroid-venus.service || true
rm -f "$home/.config/systemd/user/waydroid-venus.service"
systemctl --user -M "$user@" daemon-reload
