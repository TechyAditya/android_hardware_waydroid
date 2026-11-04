#!/usr/bin/env bash
# Run with sudo. Stops the service and removes everything install.sh added.
set -euo pipefail

user=${SUDO_USER:?run with sudo}
home=$(getent passwd "$user" | cut -d: -f6)
hook=/usr/local/bin/waydroid

if waydroid status 2> /dev/null | grep -q $'^Container:\tRUNNING'; then
   waydroid shell -- sh -c 'pid=$(pidof waydroid-app-displays) && kill $pid' || true
   # The service pinned Android's refresh rate to its windows' monitors
   waydroid shell -- sh -c 'settings delete system min_refresh_rate; settings delete system peak_refresh_rate' \
      > /dev/null 2>&1 || true
fi
for file in "$hook" /usr/local/bin/waydroid-app-displays; do
   [ -e "$file" ] || continue
   case "$(sed -n 2p "$file")" in
      "# managed-by: waydroid-app-displays" | \
      "# Installed ahead of /usr/bin/waydroid: \`waydroid app launch PACKAGE\`, which Waydroid's")
         rm -f "$file" ;;
   esac
done
rm -rf /usr/local/lib/waydroid-app-displays
rm -f /var/lib/waydroid/overlay/vendor/etc/init/waydroid-app-displays.rc
rm -rf "$home/.local/share/waydroid/data/waydroid-app-displays"
echo "uninstalled; Waydroid's own window modes apply again"
