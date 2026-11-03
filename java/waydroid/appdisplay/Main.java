package waydroid.appdisplay;

import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

public final class Main {
    static final String TAG = "waydroid-app-displays";
    static final String DIR = "/data/waydroid-app-displays";
    private static final long RECONNECT_DELAY_MS = 5000;

    public static void main(String[] args) {
        if (AndroidBridge.prop("persist.waydroid.app_displays", "true").equals("false")) {
            Log.i(TAG, "disabled by persist.waydroid.app_displays");
            return;
        }
        System.load(DIR + "/libappdisplay.so");
        if (!Native.dropToSystem()) {
            Log.e(TAG, "cannot switch to the system user");
            System.exit(1);
        }
        Looper.prepareMainLooper();

        AndroidBridge bridge = new AndroidBridge();
        String runtimeDir = AndroidBridge.prop("waydroid.xdg_runtime_dir", "/run/xdg");
        String waylandDisplay = AndroidBridge.prop("waydroid.wayland_display", "wayland-0");
        int resizeIntervalMs = Integer.parseInt(AndroidBridge.prop("persist.waydroid.resize_interval_ms", "500"));
        int density = Integer.parseInt(AndroidBridge.prop("persist.waydroid.app_display_density",
                AndroidBridge.prop("ro.sf.lcd_density", "240")));

        Thread loop = new Thread(new Runnable() {
            @Override
            public void run() {
                for (;;) {
                    int status = Native.run(bridge, runtimeDir, waylandDisplay, DIR + "/control.sock",
                            resizeIntervalMs, density);
                    Log.w(TAG, "Wayland connection ended (" + status + "); reconnecting in "
                            + RECONNECT_DELAY_MS / 1000 + " s");
                    SystemClock.sleep(RECONNECT_DELAY_MS);
                }
            }
        }, "wayland");
        loop.start();
        bridge.watchTasks();
        Looper.loop();
    }
}
