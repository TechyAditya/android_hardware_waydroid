#!/bin/bash
# Copies the build/dist output of build.sh into Waydroid's vendor overlay, and sets the
# Waydroid settings the stack is tested with in waydroid.cfg.
# Run with: sudo stack/install.sh, then restart Waydroid (waydroid session stop).
# The first run saves the overlay files and settings it replaces; stack/uninstall.sh puts them back.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
dist=${WORK:-$HERE/build}/dist
overlay=/var/lib/waydroid/overlay
backup=/var/lib/waydroid/all-fixes-backup
cfg=/var/lib/waydroid/waydroid.cfg
[[ -d $dist/vendor ]] || { echo "Missing $dist/vendor. Run stack/build.sh first." >&2; exit 1; }

mkdir -p "$backup/files" "$backup/system-files"
touch "$backup/manifest"
# Vendor files are recorded as `saved`/`absent`, as before system files were installed, and
# system files as `saved-system`/`absent-system`
for part in vendor system; do
    [[ -d $dist/$part ]] || continue
    suffix=; files=$backup/files
    [[ $part == system ]] && { suffix=-system; files=$backup/system-files; }
    while IFS= read -r rel; do
        if ! grep -qxF -e "saved$suffix $rel" -e "absent$suffix $rel" "$backup/manifest"; then
            if [[ -e $overlay/$part/$rel ]]; then
                mkdir -p "$(dirname "$files/$rel")"
                cp -a "$overlay/$part/$rel" "$files/$rel"
                echo "saved$suffix $rel" >> "$backup/manifest"
            else
                echo "absent$suffix $rel" >> "$backup/manifest"
            fi
        fi
        mode=0644
        [[ -x $dist/$part/$rel ]] && mode=0755
        install -D -m $mode "$dist/$part/$rel" "$overlay/$part/$rel"
        echo "installed $part/$rel"
    done < <(cd "$dist/$part" && find . -type f -printf '%P\n' | sort)
done

"$HERE/surfaceflinger.sh" install
"$HERE/stagefright.sh" install
"$HERE/services.sh" install

changed=$(python3 "$HERE/settings.py" install "$cfg" "$backup/settings.json")
echo "$changed"
# Android reads [properties] from waydroid_base.prop, which only `waydroid upgrade` regenerates.
# It restarts a running container.
[[ $changed != "set "* ]] || waydroid upgrade --offline

echo "Restart Waydroid to load them: waydroid session stop"
