#!/usr/bin/env bash
# Run with sudo. Stops the service and removes everything install.sh added.
set -euo pipefail

user=${SUDO_USER:?run with sudo}
home=$(getent passwd "$user" | cut -d: -f6)
hook=/usr/local/bin/waydroid

if waydroid status 2> /dev/null | grep -q $'^Container:\tRUNNING'; then
   waydroid shell -- sh -c 'pid=$(pidof waydroid-app-displays) && kill $pid' || true
fi
if [ -e "$hook" ] && grep -q waydroid-app-displays "$hook"; then
   rm -f "$hook"
fi
rm -rf /usr/local/lib/waydroid-app-displays
rm -f /var/lib/waydroid/overlay/vendor/etc/init/waydroid-app-displays.rc
rm -rf "$home/.local/share/waydroid/data/waydroid-app-displays"
echo "uninstalled; Waydroid's own window modes apply again"
