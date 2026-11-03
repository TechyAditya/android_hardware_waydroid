#!/bin/bash
# Copies the build/dist output of build.sh into Waydroid's vendor overlay.
# Run with: sudo stack/install.sh, then restart Waydroid (waydroid session stop).
# The first run saves the overlay files it replaces; stack/uninstall.sh puts them back.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
dist=${WORK:-$HERE/build}/dist/vendor
overlay=/var/lib/waydroid/overlay/vendor
backup=/var/lib/waydroid/all-fixes-backup
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

echo "Restart Waydroid to load them: waydroid session stop"
