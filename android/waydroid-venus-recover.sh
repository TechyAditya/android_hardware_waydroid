#!/system/bin/sh
# Runs whenever surfaceflinger starts.  When surfaceflinger restarts, init
# restarts the framework and the services it depends on by killing their
# process groups.  Waydroid has no process cgroups, so some of those kills
# fail, leaving Android without a display or without networking.  A
# surfaceflinger starting after boot completed is such a restart, so ask
# Waydroid to restart the container (IHardware.reboot).  Properties reset
# with the container, so a cold boot does not trigger this.

[ "$(getprop sys.boot_completed)" = 1 ] || exit 0

# one restart request per boot, however often surfaceflinger crashes
[ "$(getprop vendor.waydroid_venus.recovering)" = 1 ] && exit 0
setprop vendor.waydroid_venus.recovering 1

log -t waydroid-venus "surfaceflinger restarted, restarting Android"
# IHardware.TRANSACTION_reboot in Waydroid's tools/interfaces/IHardware.py
if ! service call waydroidhardware 4 > /dev/null; then
   log -t waydroid-venus "waydroidhardware reboot failed"
   setprop vendor.waydroid_venus.recovering 0
fi
