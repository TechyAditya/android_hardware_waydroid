#pragma once

#include <jni.h>
#include <android/log.h>
#include <media/NdkImageReader.h>
#include <wayland-client.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define LOG_TAG "waydroid-app-displays"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct wp_viewport;
struct wp_viewporter;
struct wp_fractional_scale_v1;
struct xdg_surface;
struct xdg_toplevel;
struct zxdg_toplevel_decoration_v1;

int64_t now_ms();

enum : uint32_t {
    MSG_FRAME = 1,
    MSG_TASKS_CHANGED = 2,
};

struct Msg {
    uint32_t what;
    uint32_t window;
    uint32_t gen;
    uint32_t pad;
};

struct Window;
struct Reader;
struct Decor;

// One gralloc buffer of a reader, wrapped as a wl_buffer the first time it is presented.
struct BufEntry {
    Reader *reader = nullptr;
    wl_buffer *wl = nullptr;
    AImage *held = nullptr; // the image whose buffer the compositor may still read
};

// The image queue a display renders into. A resize replaces it; the old one lives on until
// the compositor releases its last buffer.
struct Reader {
    Window *window = nullptr;
    uint32_t gen = 0;
    AImageReader *reader = nullptr;
    int width = 0, height = 0;
    bool retired = false;
    std::map<uint64_t, std::unique_ptr<BufEntry>> bufs;
};

struct Window {
    uint32_t id = 0;
    std::string pkg;
    std::string title;
    // A hidden window has no Wayland objects: its display is only seen through captures and adb.
    bool hidden = false;
    // Set at launch: the display keeps this size and density whatever the window's size and scale.
    int fixed_w = 0, fixed_h = 0, fixed_dpi = 0;
    int hint_w = 0, hint_h = 0; // the fixed window size asked of the compositor, in logical pixels

    wl_surface *surface = nullptr;
    xdg_surface *xsurface = nullptr;
    xdg_toplevel *toplevel = nullptr;
    zxdg_toplevel_decoration_v1 *decoration = nullptr;
    wp_viewport *viewport = nullptr;
    wp_fractional_scale_v1 *fractional = nullptr;
    Decor *decor = nullptr; // the window's own title bar, where the compositor draws none

    // Size of the display's surface in logical pixels: the latest configure, and the size in use.
    // A title bar of the window's own is outside it.
    int pending_w = 0, pending_h = 0;
    int width = 0, height = 0;
    bool configured = false;
    int scale120 = 0; // 0 until the compositor sends a preferred scale
    std::set<wl_output *> outputs; // the monitors the window is on

    jobject display = nullptr; // VirtualDisplay global ref
    int display_id = -1;
    int display_w = 0, display_h = 0, dpi = 0;

    std::vector<std::unique_ptr<Reader>> readers; // back() is current
    uint32_t next_gen = 1;
    BufEntry *attached = nullptr;
    bool has_frame = false;

    AImage *pending_image = nullptr; // acquired, waiting for its fence
    int pending_fence = -1;
    uint32_t pending_gen = 0;
    bool frame_waiting = false; // a frame is queued but every image slot is in use

    bool resize_pending = false;
    int64_t resize_due = 0;
    int64_t last_resize = 0;

    bool start_pending = false; // configured, waiting for the window's own scale
    int64_t start_due = 0;

    bool launched = false;
    bool had_tasks = false;
    int64_t launched_at = 0;
    bool closing = false;
};

struct Java {
    JNIEnv *env = nullptr;
    jobject bridge = nullptr;
    jmethodID createDisplay, displayId, resizeDisplay, releaseDisplay, launch, taskCount, removeTasks,
            focusDisplay, injectMotion, injectKey, setRefreshRate, screenshot;
    jintArray ids = nullptr;
    jfloatArray xy = nullptr;

    bool check(const char *what);
};

extern Java java;

// Window lookup for input handlers, which only see wl_surfaces.
Window *window_for_surface(wl_surface *surface);
Window *window_by_id(uint32_t id);
// Wakes the event loop; safe from any thread.
void post(const Msg &msg);

// Input (input.cpp)
void input_bind_seat(wl_registry *registry, uint32_t name, uint32_t version);
void input_set_cursor_shape_manager(struct wp_cursor_shape_manager_v1 *manager);
// Ends any key, button or touch still held on the window before it goes away.
void input_forget_window(const Window *w);
// Forgets all seat state when the Wayland connection ends.
void input_reset();
int64_t input_next_deadline();
void input_run_timers(int64_t now);
wl_seat *input_seat();
// Sets the cursor shape, with the serial of the pointer's latest enter.
void input_set_cursor_shape(uint32_t serial, uint32_t shape);
// Presses and releases Back on the window's display.
void input_send_back(const Window *w);

// Decorations (decor.cpp): a title bar with Back and window controls, and resize edges, which
// the window draws itself where the compositor draws none.
void decor_init(wl_compositor *compositor, wl_subcompositor *subcompositor, wl_shm *shm, wp_viewporter *viewporter);
// `dark` picks the colours; `layout` is GNOME's button-layout, such as "icon:minimize,maximize,close".
void decor_set_appearance(bool dark, const std::string &layout);
// Before the window's first commit. Without client_side, nothing shows until
// decor_set_client_side() turns it on.
void decor_create(Window *w, bool client_side);
void decor_set_client_side(Window *w, bool on);
// From each xdg_toplevel configure, before the matching xdg_surface configure.
void decor_states(Window *w, wl_array *states);
// The title bar's height in logical pixels, or 0 if it isn't shown.
int decor_title_height(const Window *w);
// Places and draws the decorations for the window's current size; the window's next commit
// shows them.
void decor_update(Window *w, int scale120);
void decor_destroy(Window *w);
// The window whose decoration this surface is, or nullptr.
Window *decor_window_for_surface(wl_surface *surface);
// Pointer and touch input on decoration surfaces. The enter and down calls return false for
// surfaces that aren't decorations; decor_touch_up returns false for touches it didn't take.
bool decor_pointer_enter(wl_surface *surface, uint32_t serial, double x, double y);
void decor_pointer_motion(double x, double y);
void decor_pointer_leave();
void decor_pointer_button(uint32_t serial, uint32_t button, bool pressed);
bool decor_touch_down(wl_surface *surface, uint32_t serial, int32_t id, double x, double y);
bool decor_touch_up(int32_t id);
// Maps a surface position to the display's pixels, accounting for a pending resize.
void window_to_display(const Window *w, double sx, double sy, float *x, float *y);
