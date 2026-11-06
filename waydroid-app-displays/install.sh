#!/usr/bin/env bash
# Run with sudo after build.sh. Installs the service into Waydroid's data and vendor overlay,
# and the waydroid-app-displays command and `waydroid app launch` hook into /usr/local. Starts
# the service if Android is running; a service that is already running keeps its old version
# until `--restart` or the next boot.
set -eEuo pipefail

here=$(cd "$(dirname "$0")" && pwd)
user=${SUDO_USER:?run with sudo}
home=$(getent passwd "$user" | cut -d: -f6)
out=$here/build/out
data=$home/.local/share/waydroid/data/waydroid-app-displays
rc=/var/lib/waydroid/overlay/vendor/etc/init/waydroid-app-displays.rc
hook=/usr/local/bin/waydroid
cli=/usr/local/bin/waydroid-app-displays
units=/usr/local/lib/systemd/user
unit=waydroid-app-displays-mpris.service
restart=0
[ "${1:-}" = --restart ] && restart=1

for file in "$out/appdisplay.jar" "$out/libappdisplay.so"; do
   [ -f "$file" ] || { echo "missing $file; run build.sh first" >&2; exit 1; }
done
[ -d "$home/.local/share/waydroid/data" ] || { echo "Waydroid isn't initialized for $user" >&2; exit 1; }
# Only replace files this script installed. The first hook had no marker line; its second
# line identifies it.
for file in "$hook" "$cli"; do
   [ -e "$file" ] || continue
   case "$(sed -n 2p "$file")" in
      "# managed-by: waydroid-app-displays" | \
      "# Installed ahead of /usr/bin/waydroid: \`waydroid app launch PACKAGE\`, which Waydroid's") ;;
      *)
         echo "$file exists and isn't ours; not replacing it" >&2
         exit 1
         ;;
   esac
done

# Android's system user is the host user, so the files and control socket belong to them.
install -d -o "$user" -g "$user" -m 755 "$data"
for file in appdisplay.jar libappdisplay.so; do
   # Rename into place: a running service has the old library mapped.
   install -o "$user" -g "$user" -m 644 "$out/$file" "$data/.$file.new"
   mv -f "$data/.$file.new" "$data/$file"
done
install -D -m 644 "$here/android/waydroid-app-displays.rc" "$rc"
install -m 755 "$here/host/waydroid-app-displays" "$cli"
install -m 755 "$here/host/waydroid" "$hook"
install -D -m 644 "$here/host/$unit" "$units/$unit"
# Earlier versions kept the launcher here.
rm -rf /usr/local/lib/waydroid-app-displays
echo "installed; Android starts the services at boot"

# The MPRIS bridge runs in the user's systemd instance, which needs their runtime directory.
uid=$(id -u "$user")
user_systemctl() {
   runuser -u "$user" -- env XDG_RUNTIME_DIR="/run/user/$uid" \
      DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" systemctl --user "$@"
}
if [ -d "/run/user/$uid" ] && user_systemctl daemon-reload; then
   user_systemctl enable "$unit" > /dev/null 2>&1
   user_systemctl restart "$unit"
   echo "started $unit"
else
   echo "$user has no systemd user instance running; $unit starts at their next login"
   runuser -u "$user" -- mkdir -p "$home/.config/systemd/user/default.target.wants"
   runuser -u "$user" -- ln -sf "$units/$unit" "$home/.config/systemd/user/default.target.wants/$unit"
fi

if waydroid status 2> /dev/null | grep -q $'^Container:\tRUNNING' &&
   [ "$(waydroid shell -- getprop sys.boot_completed 2> /dev/null | tr -d '\r')" != 1 ]; then
   echo "Android is still booting; it starts the services when it's done"
elif waydroid status 2> /dev/null | grep -q $'^Container:\tRUNNING'; then
   # The media bridge has no windows, so it always restarts.
   waydroid shell -- sh -c 'pid=$(pidof waydroid-app-media) && kill $pid' > /dev/null 2>&1 || true
   waydroid shell -- sh -c 'CLASSPATH=/data/waydroid-app-displays/appdisplay.jar setsid /system/bin/app_process \
      /system/bin --nice-name=waydroid-app-media waydroid.appdisplay.Media > /dev/null 2>&1 < /dev/null &'
   echo "started the media bridge"
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
