#pragma once

#include <jni.h>
#include <android/log.h>
#include <media/NdkImageReader.h>
#include <wayland-client.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define LOG_TAG "waydroid-app-displays"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct wp_viewport;
struct wp_fractional_scale_v1;
struct xdg_surface;
struct xdg_toplevel;

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

    wl_surface *surface = nullptr;
    xdg_surface *xsurface = nullptr;
    xdg_toplevel *toplevel = nullptr;
    wp_viewport *viewport = nullptr;
    wp_fractional_scale_v1 *fractional = nullptr;

    // Surface size in logical pixels: the latest configure, and the size in use.
    int pending_w = 0, pending_h = 0;
    int width = 0, height = 0;
    bool configured = false;
    int scale120 = 0; // 0 until the compositor sends a preferred scale

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
            focusDisplay, injectMotion, injectKey;
    jintArray ids = nullptr;
    jfloatArray xy = nullptr;

    bool check(const char *what);
};

extern Java java;

// Window lookup for input handlers, which only see wl_surfaces.
Window *window_for_surface(wl_surface *surface);
Window *window_by_id(uint32_t id);

// Input (input.cpp)
void input_bind_seat(wl_registry *registry, uint32_t name, uint32_t version);
void input_set_cursor_shape_manager(struct wp_cursor_shape_manager_v1 *manager);
// Ends any key, button or touch still held on the window before it goes away.
void input_forget_window(const Window *w);
// Forgets all seat state when the Wayland connection ends.
void input_reset();
int64_t input_next_deadline();
void input_run_timers(int64_t now);
// Maps a surface position to the display's pixels, accounting for a pending resize.
void window_to_display(const Window *w, double sx, double sy, float *x, float *y);
