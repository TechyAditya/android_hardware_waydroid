package waydroid.appdisplay;

final class Native {
    static final int MSG_TASKS_CHANGED = 2;

    static native boolean dropToSystem();

    /** Runs the Wayland event loop on the calling thread until the compositor goes away. */
    static native int run(AndroidBridge bridge, String runtimeDir, String waylandDisplay,
            String controlSocket, int resizeIntervalMs, int density);

    /** Wakes the event loop from any thread. */
    static native void post(int what, int arg);

    private Native() {}
}
