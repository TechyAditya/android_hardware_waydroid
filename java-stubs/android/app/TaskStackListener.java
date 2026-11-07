package android.app;

/** Compile-time stub for the hidden framework class; the device provides the real one. */
public abstract class TaskStackListener {
    public TaskStackListener() {}

    public void onTaskStackChanged() throws android.os.RemoteException {}

    public void onTaskMovedToBack(ActivityManager.RunningTaskInfo ti) throws android.os.RemoteException {}
}
