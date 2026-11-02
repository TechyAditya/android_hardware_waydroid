#!/system/bin/sh
# Runs whenever surfaceflinger starts.  When surfaceflinger restarts, init
# restarts the framework and the services it depends on by killing their
# process groups.  Waydroid has no process cgroups, so those kills fail and
# Android keeps running against the dead surfaceflinger.  A surfaceflinger
# newer than system_server means that happened, so ask Waydroid to restart
# the container (IHardware.reboot).

sleep 3

sf=$(pidof surfaceflinger)
ss=$(pidof system_server)
[ -n "$sf" ] && [ -n "$ss" ] || exit 0

start_time() {
   awk '{ print $22 }' "/proc/$1/stat"
}
[ "$(start_time "$sf")" -gt "$(start_time "$ss")" ] || exit 0

log -t waydroid-venus "surfaceflinger restarted, restarting Android"
service call waydroidhardware 4
