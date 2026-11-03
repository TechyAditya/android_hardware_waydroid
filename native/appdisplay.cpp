// Shows each Android app in its own Wayland window, backed by its own virtual display.

#include "appdisplay.h"

#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

Java java;

namespace {
// Set when a wake-up didn't fit in the pipe; the loop then rechecks every window.
std::atomic<bool> g_lost_wakeup{false};
int g_msg_w = -1;
}

void post(const Msg &msg) {
    if (g_msg_w < 0 || write(g_msg_w, &msg, sizeof(msg)) != ssize_t(sizeof(msg)))
        g_lost_wakeup.store(true);
}

namespace {

constexpr int kImages = 4;
constexpr int kDefaultWidth = 1280, kDefaultHeight = 800;
constexpr int64_t kTaskCheckDelayMs = 250;
constexpr int64_t kTaskPollMs = 3000;
constexpr int64_t kLaunchTimeoutMs = 20000;
constexpr int64_t kScaleWaitMs = 250;
// How long the monitors a window is on must hold still before Android's refresh rate follows;
// a window dragged across monitors enters and leaves several.
constexpr int64_t kRefreshSettleMs = 300;
// AndroidBridge.resizeDisplay() results
enum : int { kResizeDone, kResizeRestored, kResizeBroken };
constexpr int kGrallocMagic = 0x3141592;
// gralloc.default's private_handle_t::PRIV_FLAGS_GPU_MEMORY
constexpr int kGrallocGpuMemory = 0x2;
constexpr uint32_t kDrmFormatXbgr8888 = 0x34324258; // 'XB24'
constexpr uint32_t kDrmFormatAbgr8888 = 0x34324241; // 'AB24'
// AHARDWAREBUFFER_USAGE_VENDOR_0: gralloc allocates the buffer in GPU memory
constexpr uint64_t kUsageGpuMemory = 1ull << 28;

struct native_handle_layout {
    int version, numFds, numInts;
    int data[0];
};
using GetNativeHandle = const native_handle_layout *(*)(const AHardwareBuffer *);

struct Output {
    int32_t refresh = 0, pending = 0;
};

struct {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *wm_base = nullptr;
    wp_viewporter *viewporter = nullptr;
    wp_fractional_scale_manager_v1 *fractional = nullptr;
    zwp_linux_dmabuf_v1 *dmabuf = nullptr;
    bool shm_xbgr = false;
    wl_buffer *placeholder = nullptr;

    std::map<uint32_t, std::unique_ptr<Window>> windows;
    uint32_t next_window = 1;

    int msg_r = -1, msg_w = -1;
    int control = -1;
    int resize_interval_ms = 0;
    int density = 240;
    int base_scale120 = 0; // the first preferred scale seen; `density` applies at this scale
    int last_scale120 = 120;

    bool tasks_dirty = false;
    int64_t tasks_due = 0;
    int64_t next_task_poll = 0;

    // Current refresh rate of each monitor in mHz, by output and by registry name
    std::map<wl_output *, Output> outputs;
    std::map<uint32_t, wl_output *> output_names;
    bool refresh_dirty = false;
    int64_t refresh_due = 0;
    int published_refresh = -1;

    GetNativeHandle get_native_handle = nullptr;
} g;

void destroy_window(Window *w);
void collect_readers(Window *w);
void present(Window *w, Reader *r, AImage *image);
void try_acquire(Window *w);
void schedule_resize(Window *w, bool now);

// ---- Java calls ----

jobject java_create_display(const std::string &name, int width, int height, int dpi, jobject surface) {
    JNIEnv *env = java.env;
    jstring jname = env->NewStringUTF(name.c_str());
    jobject vd = env->CallObjectMethod(java.bridge, java.createDisplay, jname, width, height, dpi, surface);
    env->DeleteLocalRef(jname);
    if (!java.check("createDisplay") || !vd)
        return nullptr;
    jobject global = env->NewGlobalRef(vd);
    env->DeleteLocalRef(vd);
    return global;
}

int java_task_count(int display_id) {
    int n = java.env->CallIntMethod(java.bridge, java.taskCount, display_id);
    return java.check("taskCount") ? n : -1;
}

// ---- Buffers ----

void buffer_release(void *data, wl_buffer *) {
    auto *e = static_cast<BufEntry *>(data);
    if (e->held) {
        AImage_delete(e->held);
        e->held = nullptr;
    }
    Window *w = e->reader->window;
    if (e->reader->retired)
        collect_readers(w); // may free e
    else if (w->frame_waiting)
        try_acquire(w);
}
const wl_buffer_listener buffer_listener = {buffer_release};

wl_buffer *wrap_buffer(AHardwareBuffer *hb, BufEntry *entry) {
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(hb, &desc);
    const native_handle_layout *h = g.get_native_handle ? g.get_native_handle(hb) : nullptr;
    // gralloc.default's private_handle_t: one fd, then magic, flags, size, offset, ...,
    // and for a buffer in GPU memory its DRM format modifier at 11 (low) and 12 (high).
    if (!h || h->numFds < 1 || h->numInts < 4 || h->data[h->numFds] != kGrallocMagic) {
        ALOGE("buffer isn't from gralloc.default; can't share it with the compositor");
        return nullptr;
    }
    const int *ints = h->data + h->numFds;
    int fd = h->data[0];
    int offset = ints[3];
    if ((ints[1] & kGrallocGpuMemory) && h->numInts >= 13) {
        if (!g.dmabuf) {
            ALOGE("the compositor lacks zwp_linux_dmabuf_v1 for buffers in GPU memory");
            return nullptr;
        }
        bool opaque = desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM || g.shm_xbgr;
        zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(g.dmabuf);
        zwp_linux_buffer_params_v1_add(params, fd, 0, uint32_t(offset), desc.stride * 4, uint32_t(ints[12]),
                uint32_t(ints[11]));
        wl_buffer *b = zwp_linux_buffer_params_v1_create_immed(params, int32_t(desc.width), int32_t(desc.height),
                opaque ? kDrmFormatXbgr8888 : kDrmFormatAbgr8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        wl_buffer_add_listener(b, &buffer_listener, entry);
        return b;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0)
        return nullptr;

    uint32_t format;
    switch (desc.format) {
    case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
        format = g.shm_xbgr ? WL_SHM_FORMAT_XBGR8888 : WL_SHM_FORMAT_ABGR8888;
        break;
    default:
        ALOGE("unsupported buffer format %u", desc.format);
        return nullptr;
    }
    wl_shm_pool *pool = wl_shm_create_pool(g.shm, fd, int32_t(st.st_size));
    wl_buffer *b = wl_shm_pool_create_buffer(pool, offset, desc.width, desc.height, desc.stride * 4, format);
    wl_shm_pool_destroy(pool);
    wl_buffer_add_listener(b, &buffer_listener, entry);
    return b;
}

// Runs on the reader's own thread. The context is the window id, not a pointer, because the
// callback can race with the reader being deleted.
void on_image(void *context, AImageReader *) {
    Msg msg{MSG_FRAME, uint32_t(uintptr_t(context)), 0, 0};
    post(msg);
}

Reader *new_reader(Window *w, int width, int height) {
    auto r = std::make_unique<Reader>();
    r->window = w;
    r->gen = w->next_gen++;
    r->width = width;
    r->height = height;
    // GPU memory only reaches the compositor as dma-bufs; without them, ask for buffers it can
    // map through wl_shm.
    const uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            (g.dmabuf ? kUsageGpuMemory : AHARDWAREBUFFER_USAGE_CPU_READ_RARELY);
    media_status_t s = AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_RGBA_8888, usage, kImages, &r->reader);
    if (s != AMEDIA_OK) {
        ALOGE("AImageReader_newWithUsage(%dx%d) failed: %d", width, height, s);
        return nullptr;
    }
    AImageReader_ImageListener listener{reinterpret_cast<void *>(uintptr_t(w->id)), on_image};
    AImageReader_setImageListener(r->reader, &listener);
    w->readers.push_back(std::move(r));
    return w->readers.back().get();
}

jobject reader_surface(Reader *r) {
    ANativeWindow *nw = nullptr;
    if (AImageReader_getWindow(r->reader, &nw) != AMEDIA_OK)
        return nullptr;
    return ANativeWindow_toSurface(java.env, nw);
}

void free_reader(Window *w, Reader *r) {
    for (auto &[id, e] : r->bufs) {
        if (w->attached == e.get())
            w->attached = nullptr;
        if (e->wl)
            wl_buffer_destroy(e->wl);
        if (e->held)
            AImage_delete(e->held);
    }
    r->bufs.clear();
    if (w->pending_image && w->pending_gen == r->gen) {
        AImage_delete(w->pending_image);
        close(w->pending_fence);
        w->pending_image = nullptr;
        w->pending_fence = -1;
    }
    AImageReader_setImageListener(r->reader, nullptr);
    AImageReader_delete(r->reader);
}

// Frees retired readers once the compositor holds none of their buffers.
void collect_readers(Window *w) {
    auto &rs = w->readers;
    for (auto it = rs.begin(); it != rs.end();) {
        Reader *r = it->get();
        bool busy = !r->retired;
        for (auto &[id, e] : r->bufs)
            busy |= e->held != nullptr || w->attached == e.get();
        if (busy) {
            ++it;
        } else {
            free_reader(w, r);
            it = rs.erase(it);
        }
    }
}

Reader *current_reader(Window *w) {
    for (auto it = w->readers.rbegin(); it != w->readers.rend(); ++it)
        if (!(*it)->retired)
            return it->get();
    return nullptr;
}

Reader *reader_by_gen(Window *w, uint32_t gen) {
    for (auto &r : w->readers)
        if (r->gen == gen)
            return r.get();
    return nullptr;
}

void try_acquire(Window *w) {
    if (w->closing)
        return;
    if (w->pending_image) {
        w->frame_waiting = true;
        return;
    }
    Reader *r = current_reader(w);
    if (!r)
        return;
    AImage *image = nullptr;
    int fence = -1;
    media_status_t s = AImageReader_acquireLatestImageAsync(r->reader, &image, &fence);
    if (s == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED) {
        w->frame_waiting = true;
        return;
    }
    w->frame_waiting = false;
    if (s != AMEDIA_OK || !image)
        return;
    if (fence >= 0) {
        w->pending_image = image;
        w->pending_fence = fence;
        w->pending_gen = r->gen;
        return;
    }
    present(w, r, image);
}

void set_destination(Window *w) {
    wp_viewport_set_destination(w->viewport, w->width, w->height);
}

void present(Window *w, Reader *r, AImage *image) {
    AHardwareBuffer *hb = nullptr;
    uint64_t id = 0;
    if (AImage_getHardwareBuffer(image, &hb) != AMEDIA_OK || !hb || AHardwareBuffer_getId(hb, &id) != 0) {
        AImage_delete(image);
        return;
    }
    auto &slot = r->bufs[id];
    if (!slot) {
        slot = std::make_unique<BufEntry>();
        slot->reader = r;
        slot->wl = wrap_buffer(hb, slot.get());
    }
    BufEntry *e = slot.get();
    if (!e->wl) {
        AImage_delete(image);
        return;
    }
    if (e->held)
        AImage_delete(e->held);
    e->held = image;

    wl_surface_attach(w->surface, e->wl, 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, INT32_MAX, INT32_MAX);
    set_destination(w);
    wl_surface_commit(w->surface);
    w->attached = e;
    w->has_frame = true;
    collect_readers(w);
}

// ---- Displays ----

int scale_of(const Window *w) {
    return w->scale120 ? w->scale120 : g.last_scale120;
}

void target_size(const Window *w, int *pw, int *ph, int *dpi) {
    int s = scale_of(w);
    *pw = std::max(1, int(std::lround(w->width * s / 120.0)));
    *ph = std::max(1, int(std::lround(w->height * s / 120.0)));
    *dpi = g.base_scale120 ? int(std::lround(double(g.density) * s / g.base_scale120)) : g.density;
}

void start_display(Window *w) {
    if (w->closing)
        return;
    int pw, ph, dpi;
    target_size(w, &pw, &ph, &dpi);
    Reader *r = new_reader(w, pw, ph);
    jobject surface = r ? reader_surface(r) : nullptr;
    if (!surface) {
        w->closing = true;
        return;
    }
    w->display = java_create_display("waydroid:" + w->pkg, pw, ph, dpi, surface);
    java.env->DeleteLocalRef(surface);
    if (!w->display) {
        w->closing = true;
        return;
    }
    w->display_id = java.env->CallIntMethod(java.bridge, java.displayId, w->display);
    java.check("displayId");
    w->display_w = pw;
    w->display_h = ph;
    w->dpi = dpi;
    w->last_resize = now_ms();

    jstring jpkg = java.env->NewStringUTF(w->pkg.c_str());
    bool ok = java.env->CallBooleanMethod(java.bridge, java.launch, jpkg, w->display_id);
    java.env->DeleteLocalRef(jpkg);
    if (!java.check("launch") || !ok) {
        w->closing = true;
        return;
    }
    w->launched = true;
    w->launched_at = now_ms();
    ALOGI("%s: display %d, %dx%d at %d dpi", w->pkg.c_str(), w->display_id, pw, ph, dpi);
}

void do_resize(Window *w) {
    w->resize_pending = false;
    if (w->closing)
        return;
    int pw, ph, dpi;
    target_size(w, &pw, &ph, &dpi);
    if (!w->display || (pw == w->display_w && ph == w->display_h && dpi == w->dpi))
        return;
    Reader *old = current_reader(w);
    Reader *r = new_reader(w, pw, ph);
    jobject surface = r ? reader_surface(r) : nullptr;
    if (!surface) {
        if (r) {
            r->retired = true;
            collect_readers(w);
        }
        return;
    }
    jobject old_surface = old ? reader_surface(old) : nullptr;
    int result = java.env->CallIntMethod(java.bridge, java.resizeDisplay, w->display, pw, ph, dpi, surface,
            old_surface, w->display_w, w->display_h, w->dpi);
    if (!java.check("resizeDisplay"))
        result = kResizeBroken;
    java.env->DeleteLocalRef(surface);
    if (old_surface)
        java.env->DeleteLocalRef(old_surface);
    if (result == kResizeBroken) {
        // Either reader may still be the display's surface; release the display before either goes.
        ALOGE("%s: display %d is in an unknown state after a failed resize", w->pkg.c_str(), w->display_id);
        w->closing = true;
        return;
    }
    if (result == kResizeRestored) {
        // The display still renders into the old reader at the old size; the next configure retries.
        r->retired = true;
        collect_readers(w);
        return;
    }
    if (old) {
        old->retired = true;
        if (w->pending_image && w->pending_gen == old->gen) {
            AImage_delete(w->pending_image);
            close(w->pending_fence);
            w->pending_image = nullptr;
            w->pending_fence = -1;
        }
    }
    w->frame_waiting = false;
    collect_readers(w);
    w->display_w = pw;
    w->display_h = ph;
    w->dpi = dpi;
    w->last_resize = now_ms();
}

// Resizes right away or, within resize_interval_ms of the last resize, once the interval is up.
void schedule_resize(Window *w, bool immediate) {
    if (!w->display || w->closing)
        return;
    int pw, ph, dpi;
    target_size(w, &pw, &ph, &dpi);
    if (pw == w->display_w && ph == w->display_h && dpi == w->dpi) {
        w->resize_pending = false;
        return;
    }
    int64_t now = now_ms();
    if (immediate || now - w->last_resize >= g.resize_interval_ms) {
        do_resize(w);
    } else if (!w->resize_pending) {
        w->resize_pending = true;
        w->resize_due = w->last_resize + g.resize_interval_ms;
    }
}

// ---- Windows ----

void fractional_preferred_scale(void *data, wp_fractional_scale_v1 *, uint32_t scale) {
    Window *w = window_by_id(uint32_t(uintptr_t(data)));
    if (!w || int(scale) == w->scale120)
        return;
    if (!g.base_scale120)
        g.base_scale120 = int(scale);
    g.last_scale120 = int(scale);
    w->scale120 = int(scale);
    if (w->start_pending) {
        w->start_pending = false;
        start_display(w);
        return;
    }
    schedule_resize(w, true);
}
const wp_fractional_scale_v1_listener fractional_listener = {fractional_preferred_scale};

void toplevel_configure(void *data, xdg_toplevel *, int32_t width, int32_t height, wl_array *) {
    Window *w = window_by_id(uint32_t(uintptr_t(data)));
    if (!w)
        return;
    w->pending_w = width;
    w->pending_h = height;
}

void toplevel_close(void *data, xdg_toplevel *) {
    if (Window *w = window_by_id(uint32_t(uintptr_t(data))))
        w->closing = true;
}

void toplevel_configure_bounds(void *, xdg_toplevel *, int32_t, int32_t) {}
void toplevel_wm_capabilities(void *, xdg_toplevel *, wl_array *) {}
const xdg_toplevel_listener toplevel_listener = {
        toplevel_configure, toplevel_close, toplevel_configure_bounds, toplevel_wm_capabilities};

void xsurface_configure(void *data, xdg_surface *xs, uint32_t serial) {
    xdg_surface_ack_configure(xs, serial);
    Window *w = window_by_id(uint32_t(uintptr_t(data)));
    if (!w)
        return;
    int width = w->pending_w > 0 ? w->pending_w : (w->width > 0 ? w->width : kDefaultWidth);
    int height = w->pending_h > 0 ? w->pending_h : (w->height > 0 ? w->height : kDefaultHeight);
    bool first = !w->configured;
    w->configured = true;
    w->width = width;
    w->height = height;

    if (!w->has_frame)
        wl_surface_attach(w->surface, g.placeholder, 0, 0);
    set_destination(w);
    wl_surface_commit(w->surface);

    if (first && g.fractional && !w->scale120) {
        // The compositor sends the window's own scale once it is mapped; sizing the display
        // before that would use another window's.
        w->start_pending = true;
        w->start_due = now_ms() + kScaleWaitMs;
    } else if (first) {
        start_display(w);
    } else {
        schedule_resize(w, false);
    }
}
const xdg_surface_listener xsurface_listener = {xsurface_configure};

// ---- Refresh rate ----

void refresh_changed() {
    if (!g.refresh_dirty) {
        g.refresh_dirty = true;
        g.refresh_due = now_ms() + kRefreshSettleMs;
    }
}

// Android runs at the fastest monitor showing one of the windows, or with no window at the
// fastest monitor.
void publish_refresh() {
    g.refresh_dirty = false;
    int refresh = 0;
    for (auto &[id, w] : g.windows) {
        if (w->closing)
            continue;
        for (wl_output *o : w->outputs)
            if (auto it = g.outputs.find(o); it != g.outputs.end())
                refresh = std::max(refresh, int(it->second.refresh));
    }
    if (!refresh)
        for (auto &[o, out] : g.outputs)
            refresh = std::max(refresh, int(out.refresh));
    if (refresh <= 0 || refresh == g.published_refresh)
        return;
    ALOGI("refresh rate for the windows: %d.%03d Hz", refresh / 1000, refresh % 1000);
    java.env->CallVoidMethod(java.bridge, java.setRefreshRate, refresh);
    if (java.check("setRefreshRate"))
        g.published_refresh = refresh;
}

void surface_enter(void *data, wl_surface *, wl_output *output) {
    if (Window *w = window_by_id(uint32_t(uintptr_t(data))); w && w->outputs.insert(output).second)
        refresh_changed();
}
void surface_leave(void *data, wl_surface *, wl_output *output) {
    if (Window *w = window_by_id(uint32_t(uintptr_t(data))); w && w->outputs.erase(output))
        refresh_changed();
}
const wl_surface_listener surface_listener = {surface_enter, surface_leave};

void output_geometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *,
        int32_t) {}
void output_mode(void *, wl_output *output, uint32_t flags, int32_t, int32_t, int32_t refresh) {
    if (flags & WL_OUTPUT_MODE_CURRENT)
        g.outputs[output].pending = refresh;
}
void output_done(void *, wl_output *output) {
    Output &o = g.outputs[output];
    if (o.refresh != o.pending) {
        o.refresh = o.pending;
        refresh_changed();
    }
}
void output_scale(void *, wl_output *, int32_t) {}
const wl_output_listener output_listener = {output_geometry, output_mode, output_done, output_scale};

void bind_output(wl_registry *registry, uint32_t name, uint32_t version) {
    auto *o = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 3u)));
    wl_output_add_listener(o, &output_listener, nullptr);
    g.outputs[o];
    g.output_names[name] = o;
}

void remove_output(uint32_t name) {
    auto it = g.output_names.find(name);
    if (it == g.output_names.end())
        return;
    wl_output *o = it->second;
    g.output_names.erase(it);
    g.outputs.erase(o);
    for (auto &[id, w] : g.windows)
        w->outputs.erase(o);
    if (wl_output_get_version(o) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
        wl_output_release(o);
    else
        wl_output_destroy(o);
    refresh_changed();
}

Window *create_window(const std::string &pkg, const std::string &title) {
    auto w = std::make_unique<Window>();
    w->id = g.next_window++;
    w->pkg = pkg;
    w->title = title.empty() ? pkg : title;
    void *tag = reinterpret_cast<void *>(uintptr_t(w->id));

    w->surface = wl_compositor_create_surface(g.compositor);
    wl_surface_add_listener(w->surface, &surface_listener, tag);
    w->viewport = wp_viewporter_get_viewport(g.viewporter, w->surface);
    if (g.fractional) {
        w->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(g.fractional, w->surface);
        wp_fractional_scale_v1_add_listener(w->fractional, &fractional_listener, tag);
    }
    w->xsurface = xdg_wm_base_get_xdg_surface(g.wm_base, w->surface);
    xdg_surface_add_listener(w->xsurface, &xsurface_listener, tag);
    w->toplevel = xdg_surface_get_toplevel(w->xsurface);
    xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, tag);
    xdg_toplevel_set_app_id(w->toplevel, ("waydroid." + pkg).c_str());
    xdg_toplevel_set_title(w->toplevel, w->title.c_str());
    wl_surface_commit(w->surface);

    Window *raw = w.get();
    g.windows[raw->id] = std::move(w);
    return raw;
}

void destroy_window(Window *w) {
    ALOGI("%s: closing display %d", w->pkg.c_str(), w->display_id);
    input_forget_window(w);
    if (w->display_id >= 0) {
        java.env->CallVoidMethod(java.bridge, java.removeTasks, w->display_id);
        java.check("removeTasks");
    }
    if (w->display) {
        java.env->CallVoidMethod(java.bridge, java.releaseDisplay, w->display);
        java.check("releaseDisplay");
        java.env->DeleteGlobalRef(w->display);
        w->display = nullptr;
    }
    // Detach first, so no buffer is destroyed while it is still the surface's content.
    wl_surface_attach(w->surface, nullptr, 0, 0);
    wl_surface_commit(w->surface);
    for (auto &r : w->readers)
        free_reader(w, r.get());
    w->readers.clear();
    if (w->fractional)
        wp_fractional_scale_v1_destroy(w->fractional);
    wp_viewport_destroy(w->viewport);
    xdg_toplevel_destroy(w->toplevel);
    xdg_surface_destroy(w->xsurface);
    wl_surface_destroy(w->surface);
    if (!w->outputs.empty())
        refresh_changed();
    g.windows.erase(w->id);
}

Window *window_for_pkg(const std::string &pkg) {
    for (auto &[id, w] : g.windows)
        if (w->pkg == pkg && !w->closing)
            return w.get();
    return nullptr;
}

void check_tasks() {
    int64_t now = now_ms();
    for (auto &[id, w] : g.windows) {
        if (!w->launched || w->closing)
            continue;
        int n = java_task_count(w->display_id);
        if (n > 0)
            w->had_tasks = true;
        else if (n == 0 && (w->had_tasks || now - w->launched_at > kLaunchTimeoutMs))
            w->closing = true;
    }
}

// ---- Control socket ----

bool valid_pkg(const std::string &s) {
    if (s.empty() || s.size() > 255)
        return false;
    for (char c : s)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_')
            return false;
    return true;
}

std::string handle_command(const std::string &line) {
    std::string cmd = line.substr(0, line.find(' '));
    std::string rest = line.size() > cmd.size() ? line.substr(cmd.size() + 1) : "";
    std::string pkg = rest.substr(0, rest.find(' '));
    std::string title = rest.size() > pkg.size() ? rest.substr(pkg.size() + 1) : "";

    if (cmd == "launch") {
        if (!valid_pkg(pkg))
            return "error invalid package\n";
        if (Window *w = window_for_pkg(pkg)) {
            if (w->display_id >= 0) {
                jstring jpkg = java.env->NewStringUTF(pkg.c_str());
                java.env->CallBooleanMethod(java.bridge, java.launch, jpkg, w->display_id);
                java.env->DeleteLocalRef(jpkg);
                java.check("launch");
            }
            return "ok existing\n";
        }
        create_window(pkg, title);
        return "ok\n";
    }
    if (cmd == "close") {
        if (!valid_pkg(pkg))
            return "error invalid package\n";
        if (Window *w = window_for_pkg(pkg)) {
            w->closing = true;
            return "ok\n";
        }
        return "error no window\n";
    }
    if (cmd == "list") {
        std::string out;
        for (auto &[id, w] : g.windows)
            out += w->pkg + " display=" + std::to_string(w->display_id) + " " + std::to_string(w->display_w) +
                   "x" + std::to_string(w->display_h) + " dpi=" + std::to_string(w->dpi) + "\n";
        return out.empty() ? "\n" : out;
    }
    return "error unknown command\n";
}

void serve_control() {
    int fd = accept4(g.control, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0)
        return;
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    std::string line;
    char buf[256];
    while (line.find('\n') == std::string::npos && line.size() < 1024) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        line.append(buf, size_t(n));
    }
    line = line.substr(0, line.find('\n'));
    std::string reply = handle_command(line);
    for (size_t off = 0; off < reply.size();) {
        ssize_t n = write(fd, reply.data() + off, reply.size() - off);
        if (n <= 0)
            break;
        off += size_t(n);
    }
    close(fd);
}

int open_control(const char *path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path))
        return -1;
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    unlink(path);
    mode_t old = umask(077);
    int rc = bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    umask(old);
    if (rc < 0 || listen(fd, 8) < 0) {
        ALOGE("control socket %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// ---- Registry ----

void shm_format(void *, wl_shm *, uint32_t format) {
    if (format == WL_SHM_FORMAT_XBGR8888)
        g.shm_xbgr = true;
}
const wl_shm_listener shm_listener = {shm_format};

void wm_ping(void *, xdg_wm_base *base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener wm_listener = {wm_ping};

void registry_global(void *, wl_registry *registry, uint32_t name, const char *iface, uint32_t version) {
    auto bind = [&](const wl_interface *i, uint32_t max) {
        return wl_registry_bind(registry, name, i, std::min(version, max));
    };
    if (!strcmp(iface, wl_compositor_interface.name)) {
        g.compositor = static_cast<wl_compositor *>(bind(&wl_compositor_interface, 4));
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        g.shm = static_cast<wl_shm *>(bind(&wl_shm_interface, 1));
        wl_shm_add_listener(g.shm, &shm_listener, nullptr);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        g.wm_base = static_cast<xdg_wm_base *>(bind(&xdg_wm_base_interface, 5));
        xdg_wm_base_add_listener(g.wm_base, &wm_listener, nullptr);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
        g.dmabuf = static_cast<zwp_linux_dmabuf_v1 *>(bind(&zwp_linux_dmabuf_v1_interface, 3));
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        g.viewporter = static_cast<wp_viewporter *>(bind(&wp_viewporter_interface, 1));
    } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
        g.fractional = static_cast<wp_fractional_scale_manager_v1 *>(bind(&wp_fractional_scale_manager_v1_interface, 1));
    } else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) {
        input_set_cursor_shape_manager(
                static_cast<wp_cursor_shape_manager_v1 *>(bind(&wp_cursor_shape_manager_v1_interface, 1)));
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        input_bind_seat(registry, name, version);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        bind_output(registry, name, version);
    }
}
void registry_remove(void *, wl_registry *, uint32_t name) {
    remove_output(name);
}
const wl_registry_listener registry_listener = {registry_global, registry_remove};

wl_buffer *make_placeholder() {
    int fd = memfd_create("placeholder", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    if (ftruncate(fd, 4) < 0) {
        close(fd);
        return nullptr;
    }
    uint32_t black = 0xff000000;
    (void)!pwrite(fd, &black, sizeof(black), 0);
    wl_shm_pool *pool = wl_shm_create_pool(g.shm, fd, 4);
    wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

// ---- Event loop ----

void drain_messages() {
    Msg msgs[64];
    for (;;) {
        ssize_t n = read(g.msg_r, msgs, sizeof(msgs));
        if (n <= 0)
            return;
        for (size_t i = 0; i < size_t(n) / sizeof(Msg); i++) {
            const Msg &m = msgs[i];
            if (m.what == MSG_FRAME) {
                if (Window *w = window_by_id(m.window))
                    try_acquire(w);
            } else if (m.what == MSG_TASKS_CHANGED && !g.tasks_dirty) {
                g.tasks_dirty = true;
                g.tasks_due = now_ms() + kTaskCheckDelayMs;
            }
        }
    }
}

void fence_ready(Window *w) {
    close(w->pending_fence);
    w->pending_fence = -1;
    AImage *image = w->pending_image;
    w->pending_image = nullptr;
    Reader *r = reader_by_gen(w, w->pending_gen);
    if (r && !r->retired && !w->closing)
        present(w, r, image);
    else
        AImage_delete(image);
    if (w->frame_waiting)
        try_acquire(w);
}

int loop(const char *control_path) {
    g.control = open_control(control_path);
    if (g.control < 0)
        return 1;
    ALOGI("ready; control socket %s", control_path);

    std::vector<pollfd> fds;
    std::vector<uint32_t> fence_windows;
    for (;;) {
        for (auto it = g.windows.begin(); it != g.windows.end();) {
            Window *w = (it++)->second.get();
            if (w->closing)
                destroy_window(w);
        }

        int64_t now = now_ms();
        int64_t due = g.next_task_poll;
        if (g.tasks_dirty)
            due = std::min(due, g.tasks_due);
        for (auto &[id, w] : g.windows) {
            if (w->resize_pending)
                due = std::min(due, w->resize_due);
            if (w->start_pending)
                due = std::min(due, w->start_due);
        }
        due = std::min(due, input_next_deadline());
        if (g.refresh_dirty)
            due = std::min(due, g.refresh_due);
        int timeout = int(std::clamp<int64_t>(due - now, 0, 60000));

        while (wl_display_prepare_read(g.display) != 0) {
            if (wl_display_dispatch_pending(g.display) < 0)
                return 1;
        }
        if (wl_display_flush(g.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(g.display);
            return 1;
        }

        fds.clear();
        fence_windows.clear();
        fds.push_back({wl_display_get_fd(g.display), POLLIN, 0});
        fds.push_back({g.msg_r, POLLIN, 0});
        fds.push_back({g.control, POLLIN, 0});
        for (auto &[id, w] : g.windows) {
            if (w->pending_fence >= 0) {
                fds.push_back({w->pending_fence, POLLIN, 0});
                fence_windows.push_back(id);
            }
        }
        int rc = poll(fds.data(), fds.size(), timeout);
        if (rc < 0 && errno != EINTR) {
            wl_display_cancel_read(g.display);
            return 1;
        }
        if (rc > 0 && (fds[0].revents & POLLIN)) {
            if (wl_display_read_events(g.display) < 0)
                return 1;
        } else {
            wl_display_cancel_read(g.display);
        }
        if (fds[0].revents & (POLLERR | POLLHUP))
            return 1;
        if (wl_display_dispatch_pending(g.display) < 0)
            return 1;

        if (fds[1].revents & POLLIN)
            drain_messages();
        if (g_lost_wakeup.exchange(false)) {
            for (auto &[id, w] : g.windows)
                try_acquire(w.get());
            g.tasks_dirty = true;
            g.tasks_due = now_ms();
        }
        if (fds[2].revents & POLLIN)
            serve_control();
        for (size_t i = 0; i < fence_windows.size(); i++) {
            if (!fds[3 + i].revents)
                continue;
            Window *w = window_by_id(fence_windows[i]);
            if (w && w->pending_fence == fds[3 + i].fd)
                fence_ready(w);
        }

        now = now_ms();
        for (auto &[id, w] : g.windows) {
            if (w->resize_pending && now >= w->resize_due)
                do_resize(w.get());
            if (w->start_pending && now >= w->start_due) {
                w->start_pending = false;
                start_display(w.get());
            }
        }
        input_run_timers(now);
        if (g.refresh_dirty && now >= g.refresh_due)
            publish_refresh();
        if ((g.tasks_dirty && now >= g.tasks_due) || now >= g.next_task_poll) {
            g.tasks_dirty = false;
            g.next_task_poll = now + kTaskPollMs;
            check_tasks();
        }
    }
}

void disconnect(const char *control_path) {
    for (auto it = g.windows.begin(); it != g.windows.end();)
        destroy_window((it++)->second.get());
    if (g.control >= 0) {
        close(g.control);
        unlink(control_path);
        g.control = -1;
    }
    input_reset();
    if (g.placeholder)
        wl_buffer_destroy(g.placeholder);
    // Globals and seat devices are never destroyed one by one; they are unused from here on.
    wl_display_disconnect(g.display);
    g.display = nullptr;
    g.registry = nullptr;
    g.compositor = nullptr;
    g.shm = nullptr;
    g.wm_base = nullptr;
    g.viewporter = nullptr;
    g.fractional = nullptr;
    g.dmabuf = nullptr;
    g.placeholder = nullptr;
    g.shm_xbgr = false;
    g.outputs.clear();
    g.output_names.clear();
    // A new connection may come with other outputs and scales.
    g.base_scale120 = 0;
    g.last_scale120 = 120;
}

int run(const char *runtime_dir, const char *wayland_display, const char *control_path) {
    // The pipe outlives connections: the task listener may write to it at any time.
    if (g.msg_r < 0) {
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0)
            return 1;
        g.msg_r = pipefd[0];
        g.msg_w = pipefd[1];
        // Room for 65536 wake-ups; one that still doesn't fit sets g_lost_wakeup.
        if (fcntl(g.msg_w, F_SETPIPE_SZ, 1 << 20) < 0)
            ALOGW("can't enlarge the wake-up pipe: %s", strerror(errno));
        g_msg_w = g.msg_w;
    }
    setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
    setenv("WAYLAND_DISPLAY", wayland_display, 1);
    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        ALOGE("can't connect to %s/%s: %s", runtime_dir, wayland_display, strerror(errno));
        return 1;
    }
    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &registry_listener, nullptr);
    wl_display_roundtrip(g.display);
    wl_display_roundtrip(g.display);
    int status = 1;
    if (!g.compositor || !g.shm || !g.wm_base || !g.viewporter) {
        ALOGE("compositor lacks wl_compositor, wl_shm, xdg_wm_base or wp_viewporter");
    } else {
        g.placeholder = make_placeholder();
        refresh_changed();
        status = loop(control_path);
    }
    disconnect(control_path);
    return status;
}

} // namespace

int64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

Window *window_by_id(uint32_t id) {
    auto it = g.windows.find(id);
    return it == g.windows.end() ? nullptr : it->second.get();
}

Window *window_for_surface(wl_surface *surface) {
    if (!surface)
        return nullptr;
    Window *w = window_by_id(uint32_t(uintptr_t(wl_surface_get_user_data(surface))));
    return w && w->surface == surface && !w->closing ? w : nullptr;
}

void window_to_display(const Window *w, double sx, double sy, float *x, float *y) {
    *x = float(w->width > 0 ? sx * w->display_w / w->width : sx);
    *y = float(w->height > 0 ? sy * w->display_h / w->height : sy);
}

bool Java::check(const char *what) {
    if (!env->ExceptionCheck())
        return true;
    ALOGE("Java exception in %s", what);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return false;
}

// ---- JNI ----

extern "C" JNIEXPORT jboolean JNICALL Java_waydroid_appdisplay_Native_dropToSystem(JNIEnv *, jclass) {
    constexpr uid_t kSystem = 1000;
    if (getuid() == kSystem)
        return JNI_TRUE;
    // system, graphics, input, inet
    const gid_t groups[] = {1000, 1003, 1004, 3003};
    if (setgroups(4, groups) < 0 || setresgid(kSystem, kSystem, kSystem) < 0 ||
        setresuid(kSystem, kSystem, kSystem) < 0) {
        ALOGE("dropping to the system user failed: %s", strerror(errno));
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL Java_waydroid_appdisplay_Native_post(JNIEnv *, jclass, jint what, jint arg) {
    post(Msg{uint32_t(what), uint32_t(arg), 0, 0});
}

extern "C" JNIEXPORT jint JNICALL Java_waydroid_appdisplay_Native_run(JNIEnv *env, jclass, jobject bridge,
        jstring runtimeDir, jstring waylandDisplay, jstring controlSocket, jint resizeIntervalMs, jint density) {
    java.env = env;
    if (java.bridge)
        env->DeleteGlobalRef(java.bridge);
    java.bridge = env->NewGlobalRef(bridge);
    jclass c = env->GetObjectClass(bridge);
    struct {
        jmethodID *id;
        const char *name, *sig;
    } methods[] = {
            {&java.createDisplay, "createDisplay",
             "(Ljava/lang/String;IIILandroid/view/Surface;)Landroid/hardware/display/VirtualDisplay;"},
            {&java.displayId, "displayId", "(Landroid/hardware/display/VirtualDisplay;)I"},
            {&java.resizeDisplay, "resizeDisplay",
             "(Landroid/hardware/display/VirtualDisplay;IIILandroid/view/Surface;Landroid/view/Surface;III)I"},
            {&java.releaseDisplay, "releaseDisplay", "(Landroid/hardware/display/VirtualDisplay;)V"},
            {&java.launch, "launch", "(Ljava/lang/String;I)Z"},
            {&java.taskCount, "taskCount", "(I)I"},
            {&java.removeTasks, "removeTasks", "(I)V"},
            {&java.focusDisplay, "focusDisplay", "(I)V"},
            {&java.injectMotion, "injectMotion", "(IIIIII[I[FFFJ)V"},
            {&java.injectKey, "injectKey", "(IIIIIJ)V"},
            {&java.setRefreshRate, "setRefreshRate", "(I)V"},
    };
    for (auto &m : methods) {
        *m.id = env->GetMethodID(c, m.name, m.sig);
        if (!*m.id) {
            java.check(m.name);
            ALOGE("AndroidBridge.%s%s is missing", m.name, m.sig);
            return 1;
        }
    }
    env->DeleteLocalRef(c);
    if (!java.ids) {
        java.ids = static_cast<jintArray>(env->NewGlobalRef(env->NewIntArray(10)));
        java.xy = static_cast<jfloatArray>(env->NewGlobalRef(env->NewFloatArray(20)));
    }

    if (!g.get_native_handle)
        if (void *lib = dlopen("libnativewindow.so", RTLD_NOW))
            g.get_native_handle = reinterpret_cast<GetNativeHandle>(dlsym(lib, "AHardwareBuffer_getNativeHandle"));
    if (!g.get_native_handle) {
        ALOGE("AHardwareBuffer_getNativeHandle is unavailable");
        return 1;
    }
    g.resize_interval_ms = std::max(0, int(resizeIntervalMs));
    g.density = density > 0 ? density : 240;

    const char *rd = env->GetStringUTFChars(runtimeDir, nullptr);
    const char *wd = env->GetStringUTFChars(waylandDisplay, nullptr);
    const char *cs = env->GetStringUTFChars(controlSocket, nullptr);
    std::string runtime(rd), wayland(wd), control(cs);
    env->ReleaseStringUTFChars(runtimeDir, rd);
    env->ReleaseStringUTFChars(waylandDisplay, wd);
    env->ReleaseStringUTFChars(controlSocket, cs);
    return run(runtime.c_str(), wayland.c_str(), control.c_str());
}
