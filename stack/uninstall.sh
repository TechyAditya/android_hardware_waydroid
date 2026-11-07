#!/bin/bash
# Puts back the overlay files and waydroid.cfg settings that stack/install.sh replaced, and
# removes the ones it added.
# Run with: sudo stack/uninstall.sh, then restart Waydroid (waydroid session stop).
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/surfaceflinger.sh" uninstall
"$HERE/services.sh" uninstall
overlay=/var/lib/waydroid/overlay/vendor
backup=/var/lib/waydroid/all-fixes-backup
cfg=/var/lib/waydroid/waydroid.cfg
[[ -f $backup/manifest ]] || { echo "Nothing to uninstall: $backup/manifest not found." >&2; exit 1; }

while read -r state rel; do
    case $state in
        saved) install -D -m 0644 "$backup/files/$rel" "$overlay/$rel"; echo "restored vendor/$rel" ;;
        absent) rm -f "$overlay/$rel"; echo "removed vendor/$rel" ;;
    esac
done < "$backup/manifest"
if [[ -f $backup/settings.json ]]; then
    changed=$(python3 "$HERE/settings.py" uninstall "$cfg" "$backup/settings.json")
    echo "$changed"
    [[ $changed != "restored "* ]] || waydroid upgrade --offline
fi
rm -rf "$backup"

echo "Restart Waydroid to load them: waydroid session stop"
