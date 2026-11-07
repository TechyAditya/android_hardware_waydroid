#!/bin/bash
# Copies the build/dist output of build.sh into Waydroid's vendor overlay, and sets the
# Waydroid settings the stack is tested with in waydroid.cfg.
# Run with: sudo stack/install.sh, then restart Waydroid (waydroid session stop).
# The first run saves the overlay files and settings it replaces; stack/uninstall.sh puts them back.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
dist=${WORK:-$HERE/build}/dist/vendor
overlay=/var/lib/waydroid/overlay/vendor
backup=/var/lib/waydroid/all-fixes-backup
cfg=/var/lib/waydroid/waydroid.cfg
[[ -d $dist ]] || { echo "Missing $dist. Run stack/build.sh first." >&2; exit 1; }

mkdir -p "$backup/files"
touch "$backup/manifest"
while IFS= read -r rel; do
    if ! grep -qxF -e "saved $rel" -e "absent $rel" "$backup/manifest"; then
        if [[ -e $overlay/$rel ]]; then
            install -D -m 0644 "$overlay/$rel" "$backup/files/$rel"
            echo "saved $rel" >> "$backup/manifest"
        else
            echo "absent $rel" >> "$backup/manifest"
        fi
    fi
    install -D -m 0644 "$dist/$rel" "$overlay/$rel"
    echo "installed vendor/$rel"
done < <(cd "$dist" && find . -type f -printf '%P\n' | sort)

"$HERE/surfaceflinger.sh" install
"$HERE/services.sh" install

changed=$(python3 "$HERE/settings.py" install "$cfg" "$backup/settings.json")
echo "$changed"
# Android reads [properties] from waydroid_base.prop, which only `waydroid upgrade` regenerates.
# It restarts a running container.
[[ $changed != "set "* ]] || waydroid upgrade --offline

echo "Restart Waydroid to load them: waydroid session stop"
