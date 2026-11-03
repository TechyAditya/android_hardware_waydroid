package waydroid.appdisplay;

import android.app.ActivityOptions;
import android.app.TaskStackListener;
import android.content.AttributionSource;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;

import java.io.BufferedReader;
import java.io.FileReader;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;

/** Android-side operations the native event loop calls into. All methods run on the loop thread. */
final class AndroidBridge {
    private static final String TAG = Main.TAG;

    // DisplayManager.VIRTUAL_DISPLAY_FLAG_*; the last four are hidden.
    private static final int DISPLAY_FLAGS = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC
            | DisplayManager.VIRTUAL_DISPLAY_FLAG_SECURE
            | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
            | (1 << 6)   // SUPPORTS_TOUCH
            | (1 << 8)   // DESTROY_CONTENT_ON_REMOVAL
            | (1 << 10)  // TRUSTED
            | (1 << 12); // ALWAYS_UNLOCKED
    private static final int WINDOWING_MODE_FULLSCREEN = 1;
    private static final int INJECT_ASYNC = 0;

    private final DisplayManager displayManager;
    private final Object taskManager;
    private final Object packageManager;
    private final Object windowManager;
    private final InputManager inputManager;
    private final Method injectInputEvent;
    private final Method setDisplayId;
    private final Method setActionButton;
    private final int[] keycodes = new int[1024];

    private final MotionEvent.PointerProperties[] pointerProps = new MotionEvent.PointerProperties[10];
    private final MotionEvent.PointerCoords[] pointerCoords = new MotionEvent.PointerCoords[10];

    AndroidBridge() {
        try {
            displayManager = DisplayManager.class.getDeclaredConstructor(Context.class)
                    .newInstance(new SystemContext());
            taskManager = service("activity_task", "android.app.IActivityTaskManager");
            packageManager = service("package", "android.content.pm.IPackageManager");
            windowManager = service("window", "android.view.IWindowManager");
            inputManager = (InputManager) InputManager.class.getMethod("getInstance").invoke(null);
            injectInputEvent = InputManager.class.getMethod("injectInputEvent", InputEvent.class, int.class);
            setDisplayId = InputEvent.class.getMethod("setDisplayId", int.class);
            setActionButton = MotionEvent.class.getMethod("setActionButton", int.class);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("framework API missing", e);
        }
        for (int i = 0; i < pointerProps.length; i++) {
            pointerProps[i] = new MotionEvent.PointerProperties();
            pointerCoords[i] = new MotionEvent.PointerCoords();
        }
        loadKeyLayout("/system/usr/keylayout/Generic.kl");
    }

    static String prop(String name, String def) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            return (String) sp.getMethod("get", String.class, String.class).invoke(null, name, def);
        } catch (ReflectiveOperationException e) {
            return def;
        }
    }

    // ---- Displays ----

    VirtualDisplay createDisplay(String name, int width, int height, int dpi, Surface surface) {
        VirtualDisplay display;
        try {
            display = displayManager.createVirtualDisplay(name, width, height, dpi, surface, DISPLAY_FLAGS);
        } catch (Throwable t) {
            Log.e(TAG, "createVirtualDisplay failed", t);
            return null;
        }
        // Apps that lock their orientation would otherwise be letterboxed whenever the window's
        // shape doesn't match, as on a phone held the other way.
        try {
            method(windowManager, "setIgnoreOrientationRequest", 2)
                    .invoke(windowManager, displayId(display), true);
        } catch (Throwable t) {
            Log.w(TAG, "can't make display " + displayId(display) + " ignore orientation requests", t);
        }
        return display;
    }

    int displayId(VirtualDisplay display) {
        return display.getDisplay().getDisplayId();
    }

    void resizeDisplay(VirtualDisplay display, int width, int height, int dpi, Surface surface) {
        try {
            display.resize(width, height, dpi);
            display.setSurface(surface);
        } catch (Throwable t) {
            Log.e(TAG, "resizing display " + displayId(display) + " failed", t);
        }
    }

    void releaseDisplay(VirtualDisplay display) {
        try {
            display.release();
        } catch (Throwable t) {
            Log.e(TAG, "releasing a display failed", t);
        }
    }

    // ---- Tasks ----

    /** Starts the package's launcher activity on the display; returns false if it has none. */
    boolean launch(String pkg, int displayId) {
        try {
            ComponentName component = launcherActivity(pkg);
            if (component == null) {
                Log.w(TAG, pkg + " has no launcher activity");
                return false;
            }
            Intent intent = new Intent(Intent.ACTION_MAIN)
                    .addCategory(Intent.CATEGORY_LAUNCHER)
                    .setComponent(component)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
            ActivityOptions options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId);
            ActivityOptions.class.getMethod("setLaunchWindowingMode", int.class)
                    .invoke(options, WINDOWING_MODE_FULLSCREEN);
            Method start = method(taskManager, "startActivityAsUser", 12);
            int result = (Integer) start.invoke(taskManager, null, "android", null, intent, null, null, null,
                    0, 0, null, options.toBundle(), 0);
            if (result < 0) {
                Log.e(TAG, "starting " + component + " on display " + displayId + " returned " + result);
                return false;
            }
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "launching " + pkg + " failed", t);
            return false;
        }
    }

    private ComponentName launcherActivity(String pkg) throws ReflectiveOperationException {
        Intent query = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setPackage(pkg);
        Method m = method(packageManager, "queryIntentActivities", 4);
        Object flags = m.getParameterTypes()[2] == long.class ? (Object) 0L : (Object) 0;
        Object slice = m.invoke(packageManager, query, null, flags, 0);
        @SuppressWarnings("unchecked")
        List<ResolveInfo> list = (List<ResolveInfo>) slice.getClass().getMethod("getList").invoke(slice);
        if (list == null || list.isEmpty())
            return null;
        return new ComponentName(pkg, list.get(0).activityInfo.name);
    }

    private List<?> rootTasks(int displayId) throws ReflectiveOperationException {
        return (List<?>) method(taskManager, "getAllRootTaskInfosOnDisplay", 1).invoke(taskManager, displayId);
    }

    private static int[] childTaskIds(Object rootTask) throws ReflectiveOperationException {
        return (int[]) field(rootTask, "childTaskIds").get(rootTask);
    }

    /** Number of tasks on the display, or -1 if Android can't say. */
    int taskCount(int displayId) {
        try {
            int count = 0;
            for (Object root : rootTasks(displayId))
                count += childTaskIds(root).length;
            return count;
        } catch (Throwable t) {
            Log.e(TAG, "listing tasks on display " + displayId + " failed", t);
            return -1;
        }
    }

    /** Finishes every task on the display, so releasing it leaves nothing behind. */
    void removeTasks(int displayId) {
        try {
            Method remove = method(taskManager, "removeTask", 1);
            for (Object root : rootTasks(displayId))
                for (int id : childTaskIds(root))
                    remove.invoke(taskManager, id);
        } catch (Throwable t) {
            Log.e(TAG, "removing tasks on display " + displayId + " failed", t);
        }
    }

    /** Makes the display's top task Android's focused one, so key events go to it. */
    void focusDisplay(int displayId) {
        try {
            List<?> roots = rootTasks(displayId);
            if (roots.isEmpty())
                return;
            int taskId = field(roots.get(0), "taskId").getInt(roots.get(0));
            method(taskManager, "setFocusedTask", 1).invoke(taskManager, taskId);
        } catch (Throwable t) {
            Log.e(TAG, "focusing display " + displayId + " failed", t);
        }
    }

    void watchTasks() {
        TaskStackListener listener = new TaskStackListener() {
            @Override
            public void onTaskStackChanged() {
                Native.post(Native.MSG_TASKS_CHANGED, 0);
            }
        };
        try {
            method(taskManager, "registerTaskStackListener", 1).invoke(taskManager, listener);
        } catch (Throwable t) {
            Log.e(TAG, "registering the task listener failed; windows won't close when apps exit", t);
        }
    }

    // ---- Input ----

    void injectMotion(int displayId, int action, int actionButton, int buttonState, int source, int count,
            int[] ids, float[] xy, float vscroll, float hscroll, long downTime) {
        boolean mouse = source == InputDevice.SOURCE_MOUSE;
        for (int i = 0; i < count; i++) {
            MotionEvent.PointerProperties p = pointerProps[i];
            p.id = ids[i];
            p.toolType = mouse ? MotionEvent.TOOL_TYPE_MOUSE : MotionEvent.TOOL_TYPE_FINGER;
            MotionEvent.PointerCoords c = pointerCoords[i];
            c.clear();
            c.x = xy[2 * i];
            c.y = xy[2 * i + 1];
            c.pressure = mouse && buttonState == 0 ? 0f : 1f;
            c.size = 1f;
            if (vscroll != 0f)
                c.setAxisValue(MotionEvent.AXIS_VSCROLL, vscroll);
            if (hscroll != 0f)
                c.setAxisValue(MotionEvent.AXIS_HSCROLL, hscroll);
        }
        MotionEvent event = MotionEvent.obtain(downTime, SystemClock.uptimeMillis(), action, count,
                pointerProps, pointerCoords, 0, buttonState, 1f, 1f, 0, 0, source, 0);
        try {
            if (actionButton != 0)
                setActionButton.invoke(event, actionButton);
            inject(event, displayId);
        } catch (Throwable t) {
            Log.e(TAG, "injecting a motion event failed", t);
        } finally {
            event.recycle();
        }
    }

    void injectKey(int displayId, int action, int scancode, int metaState, int repeat, long downTime) {
        int keycode = scancode >= 0 && scancode < keycodes.length ? keycodes[scancode] : 0;
        if (keycode == 0)
            return;
        KeyEvent event = new KeyEvent(downTime, SystemClock.uptimeMillis(), action, keycode, repeat, metaState,
                KeyCharacterMap.VIRTUAL_KEYBOARD, scancode, KeyEvent.FLAG_FROM_SYSTEM, InputDevice.SOURCE_KEYBOARD);
        try {
            inject(event, displayId);
        } catch (Throwable t) {
            Log.e(TAG, "injecting a key event failed", t);
        }
    }

    private void inject(InputEvent event, int displayId) throws ReflectiveOperationException {
        setDisplayId.invoke(event, displayId);
        injectInputEvent.invoke(inputManager, event, INJECT_ASYNC);
    }

    /** Maps Linux key codes, which Wayland sends, to Android key codes the way Android's generic layout does. */
    private void loadKeyLayout(String path) {
        try (BufferedReader in = new BufferedReader(new FileReader(path))) {
            String line;
            while ((line = in.readLine()) != null) {
                String[] f = line.trim().split("\\s+");
                if (f.length < 3 || !f[0].equals("key") || f[1].equals("usage"))
                    continue;
                int scancode = Integer.parseInt(f[1]);
                int keycode = KeyEvent.keyCodeFromString(f[2]);
                if (scancode >= 0 && scancode < keycodes.length && keycode > 0)
                    keycodes[scancode] = keycode;
            }
        } catch (Exception e) {
            Log.e(TAG, "reading " + path + " failed; keyboard input won't work", e);
        }
    }

    // ---- Reflection helpers ----

    private static Object service(String name, String iface) throws ReflectiveOperationException {
        IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("getService", String.class).invoke(null, name);
        return Class.forName(iface + "$Stub").getMethod("asInterface", IBinder.class).invoke(null, binder);
    }

    private static Method method(Object target, String name, int params) throws NoSuchMethodException {
        for (Method m : target.getClass().getMethods())
            if (m.getName().equals(name) && m.getParameterTypes().length == params)
                return m;
        throw new NoSuchMethodException(name + "/" + params);
    }

    private static Field field(Object target, String name) throws NoSuchFieldException {
        for (Class<?> c = target.getClass(); c != null; c = c.getSuperclass()) {
            try {
                return c.getDeclaredField(name);
            } catch (NoSuchFieldException ignored) {
            }
        }
        throw new NoSuchFieldException(name);
    }

    /** The minimum a framework manager needs to act as the system package. */
    private static final class SystemContext extends ContextWrapper {
        SystemContext() {
            super(null);
        }

        @Override
        public String getPackageName() {
            return "android";
        }

        @Override
        public String getOpPackageName() {
            return "android";
        }

        @Override
        public AttributionSource getAttributionSource() {
            return new AttributionSource.Builder(Process.SYSTEM_UID).setPackageName("android").build();
        }

        @Override
        public Context getApplicationContext() {
            return this;
        }
    }
}
