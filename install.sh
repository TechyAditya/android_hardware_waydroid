#!/usr/bin/env bash
# Run with sudo after build.sh. Installs the service into Waydroid's data and vendor overlay,
# and the `waydroid app launch` hook into /usr/local. Starts the service if Android is running;
# a service that is already running keeps its old version until `--restart` or the next boot.
set -eEuo pipefail

here=$(cd "$(dirname "$0")" && pwd)
user=${SUDO_USER:?run with sudo}
home=$(getent passwd "$user" | cut -d: -f6)
out=$here/build/out
data=$home/.local/share/waydroid/data/waydroid-app-displays
rc=/var/lib/waydroid/overlay/vendor/etc/init/waydroid-app-displays.rc
prefix=/usr/local/lib/waydroid-app-displays
hook=/usr/local/bin/waydroid
restart=0
[ "${1:-}" = --restart ] && restart=1

for file in "$out/appdisplay.jar" "$out/libappdisplay.so"; do
   [ -f "$file" ] || { echo "missing $file; run build.sh first" >&2; exit 1; }
done
[ -d "$home/.local/share/waydroid/data" ] || { echo "Waydroid isn't initialized for $user" >&2; exit 1; }
# Only replace a hook this script installed. The first release had no marker line; its
# second line identifies it.
if [ -e "$hook" ]; then
   case "$(sed -n 2p "$hook")" in
      "# managed-by: waydroid-app-displays" | \
      "# Installed ahead of /usr/bin/waydroid: \`waydroid app launch PACKAGE\`, which Waydroid's") ;;
      *)
         echo "$hook exists and isn't ours; not replacing it" >&2
         exit 1
         ;;
   esac
fi

# Android's system user is the host user, so the files and control socket belong to them.
install -d -o "$user" -g "$user" -m 755 "$data"
for file in appdisplay.jar libappdisplay.so; do
   # Rename into place: a running service has the old library mapped.
   install -o "$user" -g "$user" -m 644 "$out/$file" "$data/.$file.new"
   mv -f "$data/.$file.new" "$data/$file"
done
install -D -m 644 "$here/android/waydroid-app-displays.rc" "$rc"
install -D -m 755 "$here/host/launch.py" "$prefix/launch.py"
install -m 755 "$here/host/waydroid" "$hook"
echo "installed; Android starts the service at boot"

if waydroid status 2> /dev/null | grep -q $'^Container:\tRUNNING'; then
   if [ -n "$(waydroid shell -- pidof waydroid-app-displays 2> /dev/null)" ]; then
      [ "$restart" = 1 ] || { echo "the running service keeps its old version until --restart"; exit 0; }
      echo "restarting the service; its windows close"
      waydroid shell -- sh -c 'kill $(pidof waydroid-app-displays)'
      sleep 1
   fi
   waydroid shell -- sh -c 'CLASSPATH=/data/waydroid-app-displays/appdisplay.jar setsid /system/bin/app_process \
      /system/bin --nice-name=waydroid-app-displays waydroid.appdisplay.Main > /dev/null 2>&1 < /dev/null &'
   echo "started the service"
fi
