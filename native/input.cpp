// Forwards Wayland input on an app's window to that app's display.

#include "appdisplay.h"

#include <linux/input-event-codes.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <map>

#include "cursor-shape-v1-client-protocol.h"

namespace {

// android.view.MotionEvent, InputDevice and KeyEvent constants
enum {
    ACTION_DOWN = 0,
    ACTION_UP = 1,
    ACTION_MOVE = 2,
    ACTION_CANCEL = 3,
    ACTION_POINTER_DOWN = 5,
    ACTION_POINTER_UP = 6,
    ACTION_HOVER_MOVE = 7,
    ACTION_SCROLL = 8,
    ACTION_HOVER_ENTER = 9,
    ACTION_HOVER_EXIT = 10,
    ACTION_BUTTON_PRESS = 11,
    ACTION_BUTTON_RELEASE = 12,
    ACTION_POINTER_INDEX_SHIFT = 8,
};
constexpr int SOURCE_MOUSE = 0x2002, SOURCE_TOUCHSCREEN = 0x1002;
constexpr int BUTTON_PRIMARY = 1, BUTTON_SECONDARY = 2, BUTTON_TERTIARY = 4;
constexpr int KEY_ACTION_DOWN = 0, KEY_ACTION_UP = 1;
constexpr int META_SHIFT = 0x1 | 0x40, META_ALT = 0x2 | 0x10, META_ALT_RIGHT = 0x2 | 0x20, META_CTRL = 0x1000 | 0x2000,
              META_META = 0x10000 | 0x20000, META_CAPS_LOCK = 0x100000, META_NUM_LOCK = 0x200000;
// xkb modifier masks of the default evdev keymap
constexpr uint32_t XKB_SHIFT = 1, XKB_LOCK = 2, XKB_CTRL = 4, XKB_ALT = 8, XKB_NUM = 16, XKB_SUPER = 64, XKB_ALTGR = 128;
constexpr int kMaxTouches = 10;
constexpr double kScrollUnitsPerNotch = 15.0;

struct TouchPoint {
    int32_t wl_id;
    int id;
    double x, y;
};

struct {
    wl_seat *seat = nullptr;
    wl_pointer *pointer = nullptr;
    wl_keyboard *keyboard = nullptr;
    wl_touch *touch = nullptr;
    wp_cursor_shape_manager_v1 *shapes = nullptr;
    wp_cursor_shape_device_v1 *shape_device = nullptr;

    uint32_t pointer_window = 0;
    double px = 0, py = 0;
    int buttons = 0;
    bool pressed = false; // between ACTION_DOWN and ACTION_UP
    int64_t pointer_down = 0;
    std::map<uint32_t, int64_t> side_keys; // BACK and FORWARD held through mouse buttons
    bool moved = false;
    double scroll_v = 0, scroll_h = 0;
    bool v120_v = false, v120_h = false;

    uint32_t key_window = 0;
    int meta = 0;
    std::map<uint32_t, int64_t> keys_down;
    int repeat_rate = 25, repeat_delay = 600;
    uint32_t repeat_key = 0;
    int64_t repeat_due = 0, repeat_down = 0;
    int repeat_count = 0;

    uint32_t touch_window = 0;
    std::vector<TouchPoint> touches;
    bool touch_moved = false;
    int64_t touch_down = 0;
} s;

Window *live(uint32_t id) {
    Window *w = window_by_id(id);
    return w && !w->closing && w->display_id >= 0 ? w : nullptr;
}

void inject_motion(const Window *w, int action, int action_button, int buttons, int source, int count, const int *ids,
        const float *xy, float vscroll, float hscroll, int64_t down) {
    JNIEnv *env = java.env;
    env->SetIntArrayRegion(java.ids, 0, count, ids);
    env->SetFloatArrayRegion(java.xy, 0, count * 2, xy);
    env->CallVoidMethod(java.bridge, java.injectMotion, w->display_id, action, action_button, buttons, source, count,
            java.ids, java.xy, vscroll, hscroll, jlong(down));
    java.check("injectMotion");
}

void inject_key(const Window *w, int action, uint32_t key, int repeat, int64_t down) {
    java.env->CallVoidMethod(java.bridge, java.injectKey, w->display_id, action, int(key), s.meta, repeat, jlong(down));
    java.check("injectKey");
}

// ---- Pointer ----

void mouse_event(const Window *w, int action, int action_button = 0, float vscroll = 0, float hscroll = 0) {
    float xy[2];
    window_to_display(w, s.px, s.py, &xy[0], &xy[1]);
    const int id = 0;
    int64_t down = s.pressed ? s.pointer_down : now_ms();
    inject_motion(w, action, action_button, s.buttons, SOURCE_MOUSE, 1, &id, xy, vscroll, hscroll, down);
}

// Ends a press that is still down, as when the pointer leaves or the window goes away.
void end_press(const Window *w) {
    for (auto &[key, down] : s.side_keys)
        inject_key(w, KEY_ACTION_UP, key, 0, down);
    s.side_keys.clear();
    if (!s.pressed)
        return;
    s.buttons = 0;
    mouse_event(w, ACTION_UP);
    s.pressed = false;
}

void flush_motion(Window *w) {
    if (!s.moved)
        return;
    s.moved = false;
    mouse_event(w, s.buttons ? ACTION_MOVE : ACTION_HOVER_MOVE);
}

void pointer_enter(void *, wl_pointer *pointer, uint32_t serial, wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    if (s.shapes && !s.shape_device)
        s.shape_device = wp_cursor_shape_manager_v1_get_pointer(s.shapes, pointer);
    if (s.shape_device)
        wp_cursor_shape_device_v1_set_shape(s.shape_device, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    Window *w = window_for_surface(surface);
    s.pointer_window = w ? w->id : 0;
    s.px = wl_fixed_to_double(x);
    s.py = wl_fixed_to_double(y);
    s.buttons = 0;
    s.pressed = false;
    s.moved = false;
    if ((w = live(s.pointer_window)))
        mouse_event(w, ACTION_HOVER_ENTER);
}

void pointer_leave(void *, wl_pointer *, uint32_t, wl_surface *) {
    if (Window *w = live(s.pointer_window)) {
        bool was_pressed = s.pressed;
        end_press(w);
        if (!was_pressed)
            mouse_event(w, ACTION_HOVER_EXIT);
    }
    s.side_keys.clear();
    s.pointer_window = 0;
    s.buttons = 0;
    s.pressed = false;
}

void pointer_motion(void *, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    s.px = wl_fixed_to_double(x);
    s.py = wl_fixed_to_double(y);
    s.moved = true;
    if (wl_pointer_get_version(s.pointer) < WL_POINTER_FRAME_SINCE_VERSION)
        if (Window *w = live(s.pointer_window))
            flush_motion(w);
}

void pointer_button(void *, wl_pointer *, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    Window *w = live(s.pointer_window);
    if (!w)
        return;
    flush_motion(w);
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (button == BTN_SIDE || button == BTN_EXTRA) {
        // Android's input reader turns these mouse buttons into BACK and FORWARD keys.
        uint32_t key = button == BTN_SIDE ? KEY_BACK : KEY_FORWARD;
        auto held = s.side_keys.find(key);
        if (pressed && held == s.side_keys.end()) {
            int64_t now = now_ms();
            s.side_keys[key] = now;
            inject_key(w, KEY_ACTION_DOWN, key, 0, now);
        } else if (!pressed && held != s.side_keys.end()) {
            inject_key(w, KEY_ACTION_UP, key, 0, held->second);
            s.side_keys.erase(held);
        }
        return;
    }
    int b = button == BTN_LEFT ? BUTTON_PRIMARY : button == BTN_RIGHT ? BUTTON_SECONDARY
          : button == BTN_MIDDLE ? BUTTON_TERTIARY : 0;
    if (!b)
        return;
    if (pressed) {
        if (s.buttons & b)
            return;
        if (!s.pressed) {
            s.pointer_down = now_ms();
            s.pressed = true;
            s.buttons = b;
            mouse_event(w, ACTION_DOWN);
        } else {
            s.buttons |= b;
        }
        mouse_event(w, ACTION_BUTTON_PRESS, b);
    } else {
        if (!(s.buttons & b))
            return;
        s.buttons &= ~b;
        mouse_event(w, ACTION_BUTTON_RELEASE, b);
        if (!s.buttons) {
            mouse_event(w, ACTION_UP);
            s.pressed = false;
        }
    }
}

void flush_scroll(Window *w) {
    if (s.scroll_v == 0 && s.scroll_h == 0)
        return;
    // Wayland scrolls down and right for positive values; Android's VSCROLL is positive upwards.
    mouse_event(w, ACTION_SCROLL, 0, float(-s.scroll_v), float(s.scroll_h));
    s.scroll_v = s.scroll_h = 0;
}

void pointer_axis(void *, wl_pointer *, uint32_t, uint32_t axis, wl_fixed_t value) {
    double notches = wl_fixed_to_double(value) / kScrollUnitsPerNotch;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && !s.v120_v)
        s.scroll_v += notches;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL && !s.v120_h)
        s.scroll_h += notches;
    if (wl_pointer_get_version(s.pointer) < WL_POINTER_FRAME_SINCE_VERSION)
        if (Window *w = live(s.pointer_window))
            flush_scroll(w);
}

void pointer_axis_value120(void *, wl_pointer *, uint32_t axis, int32_t value120) {
    // Arrives before the matching axis event of the same frame, and replaces it.
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        if (!s.v120_v)
            s.scroll_v = 0;
        s.v120_v = true;
        s.scroll_v += value120 / 120.0;
    } else {
        if (!s.v120_h)
            s.scroll_h = 0;
        s.v120_h = true;
        s.scroll_h += value120 / 120.0;
    }
}

void pointer_frame(void *, wl_pointer *) {
    if (Window *w = live(s.pointer_window)) {
        flush_motion(w);
        flush_scroll(w);
    }
    s.scroll_v = s.scroll_h = 0;
    s.v120_v = s.v120_h = false;
}

void pointer_axis_source(void *, wl_pointer *, uint32_t) {}
void pointer_axis_stop(void *, wl_pointer *, uint32_t, uint32_t) {}
void pointer_axis_discrete(void *, wl_pointer *, uint32_t, int32_t) {}
void pointer_axis_relative_direction(void *, wl_pointer *, uint32_t, uint32_t) {}

const wl_pointer_listener pointer_listener = {
        pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis, pointer_frame,
        pointer_axis_source, pointer_axis_stop, pointer_axis_discrete, pointer_axis_value120,
        pointer_axis_relative_direction,
};

// ---- Keyboard ----

bool is_modifier(uint32_t key) {
    switch (key) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL: case KEY_LEFTALT:
    case KEY_RIGHTALT: case KEY_LEFTMETA: case KEY_RIGHTMETA: case KEY_CAPSLOCK: case KEY_NUMLOCK:
        return true;
    default:
        return false;
    }
}

void release_keys(const Window *w) {
    if (w && w->display_id >= 0)
        for (auto &[key, down] : s.keys_down)
            inject_key(w, KEY_ACTION_UP, key, 0, down);
    s.keys_down.clear();
    s.repeat_key = 0;
}

void keyboard_keymap(void *, wl_keyboard *, uint32_t, int32_t fd, uint32_t) {
    close(fd);
}

void keyboard_enter(void *, wl_keyboard *, uint32_t, wl_surface *surface, wl_array *) {
    Window *w = window_for_surface(surface);
    s.key_window = w ? w->id : 0;
    s.keys_down.clear();
    s.repeat_key = 0;
    if ((w = live(s.key_window))) {
        java.env->CallVoidMethod(java.bridge, java.focusDisplay, w->display_id);
        java.check("focusDisplay");
    }
}

void keyboard_leave(void *, wl_keyboard *, uint32_t, wl_surface *) {
    release_keys(live(s.key_window));
    s.key_window = 0;
}

void keyboard_key(void *, wl_keyboard *, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    Window *w = live(s.key_window);
    if (!w)
        return;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        int64_t now = now_ms();
        s.keys_down[key] = now;
        inject_key(w, KEY_ACTION_DOWN, key, 0, now);
        if (!is_modifier(key) && s.repeat_rate > 0) {
            s.repeat_key = key;
            s.repeat_down = now;
            s.repeat_due = now + s.repeat_delay;
            s.repeat_count = 0;
        }
    } else {
        auto it = s.keys_down.find(key);
        if (it == s.keys_down.end())
            return;
        int64_t down = it->second;
        s.keys_down.erase(it);
        if (s.repeat_key == key)
            s.repeat_key = 0;
        inject_key(w, KEY_ACTION_UP, key, 0, down);
    }
}

void keyboard_modifiers(void *, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked,
        uint32_t) {
    uint32_t held = depressed | latched;
    int meta = 0;
    if (held & XKB_SHIFT)
        meta |= META_SHIFT;
    if (held & XKB_CTRL)
        meta |= META_CTRL;
    if (held & XKB_ALT)
        meta |= META_ALT;
    if (held & XKB_ALTGR)
        meta |= META_ALT_RIGHT;
    if (held & XKB_SUPER)
        meta |= META_META;
    if (locked & XKB_LOCK)
        meta |= META_CAPS_LOCK;
    if (locked & XKB_NUM)
        meta |= META_NUM_LOCK;
    s.meta = meta;
}

void keyboard_repeat_info(void *, wl_keyboard *, int32_t rate, int32_t delay) {
    s.repeat_rate = std::max(0, rate);
    s.repeat_delay = std::max(0, delay);
    if (!s.repeat_rate)
        s.repeat_key = 0;
}

const wl_keyboard_listener keyboard_listener = {
        keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat_info,
};

// ---- Touch ----

void send_touches(const Window *w, int action) {
    int ids[kMaxTouches];
    float xy[kMaxTouches * 2];
    int n = int(s.touches.size());
    for (int i = 0; i < n; i++) {
        ids[i] = s.touches[i].id;
        window_to_display(w, s.touches[i].x, s.touches[i].y, &xy[2 * i], &xy[2 * i + 1]);
    }
    inject_motion(w, action, 0, 0, SOURCE_TOUCHSCREEN, n, ids, xy, 0, 0, s.touch_down);
}

int touch_index(int32_t wl_id) {
    for (size_t i = 0; i < s.touches.size(); i++)
        if (s.touches[i].wl_id == wl_id)
            return int(i);
    return -1;
}

void touch_down(void *, wl_touch *, uint32_t, uint32_t, wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    Window *w = window_for_surface(surface);
    if (!w)
        return;
    if (s.touches.empty())
        s.touch_window = w->id;
    if (w->id != s.touch_window || int(s.touches.size()) >= kMaxTouches || touch_index(id) >= 0 || !live(w->id))
        return;
    int android_id = 0;
    while (std::any_of(s.touches.begin(), s.touches.end(), [&](const TouchPoint &t) { return t.id == android_id; }))
        android_id++;
    s.touches.push_back({id, android_id, wl_fixed_to_double(x), wl_fixed_to_double(y)});
    int index = int(s.touches.size()) - 1;
    if (index == 0) {
        s.touch_down = now_ms();
        send_touches(w, ACTION_DOWN);
    } else {
        send_touches(w, ACTION_POINTER_DOWN | (index << ACTION_POINTER_INDEX_SHIFT));
    }
}

void touch_up(void *, wl_touch *, uint32_t, uint32_t, int32_t id) {
    int index = touch_index(id);
    if (index < 0)
        return;
    if (Window *w = live(s.touch_window)) {
        if (s.touch_moved) {
            send_touches(w, ACTION_MOVE);
            s.touch_moved = false;
        }
        send_touches(w, s.touches.size() == 1 ? ACTION_UP : ACTION_POINTER_UP | (index << ACTION_POINTER_INDEX_SHIFT));
    }
    s.touches.erase(s.touches.begin() + index);
}

void touch_motion(void *, wl_touch *, uint32_t, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    int index = touch_index(id);
    if (index < 0)
        return;
    s.touches[index].x = wl_fixed_to_double(x);
    s.touches[index].y = wl_fixed_to_double(y);
    s.touch_moved = true;
}

void touch_frame(void *, wl_touch *) {
    if (!s.touch_moved)
        return;
    s.touch_moved = false;
    if (Window *w = live(s.touch_window); w && !s.touches.empty())
        send_touches(w, ACTION_MOVE);
}

void touch_cancel(void *, wl_touch *) {
    if (Window *w = live(s.touch_window); w && !s.touches.empty())
        send_touches(w, ACTION_CANCEL);
    s.touches.clear();
    s.touch_moved = false;
}

void touch_shape(void *, wl_touch *, int32_t, wl_fixed_t, wl_fixed_t) {}
void touch_orientation(void *, wl_touch *, int32_t, wl_fixed_t) {}

const wl_touch_listener touch_listener = {
        touch_down, touch_up, touch_motion, touch_frame, touch_cancel, touch_shape, touch_orientation,
};

// ---- Seat ----

void seat_capabilities(void *, wl_seat *seat, uint32_t caps) {
    bool want_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
    if (want_pointer && !s.pointer) {
        s.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(s.pointer, &pointer_listener, nullptr);
    } else if (!want_pointer && s.pointer) {
        if (s.shape_device)
            wp_cursor_shape_device_v1_destroy(s.shape_device);
        s.shape_device = nullptr;
        wl_pointer_release(s.pointer);
        s.pointer = nullptr;
        s.pointer_window = 0;
    }
    bool want_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (want_keyboard && !s.keyboard) {
        s.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(s.keyboard, &keyboard_listener, nullptr);
    } else if (!want_keyboard && s.keyboard) {
        release_keys(live(s.key_window));
        wl_keyboard_release(s.keyboard);
        s.keyboard = nullptr;
        s.key_window = 0;
    }
    bool want_touch = caps & WL_SEAT_CAPABILITY_TOUCH;
    if (want_touch && !s.touch) {
        s.touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(s.touch, &touch_listener, nullptr);
    } else if (!want_touch && s.touch) {
        wl_touch_release(s.touch);
        s.touch = nullptr;
        s.touches.clear();
    }
}

void seat_name(void *, wl_seat *, const char *) {}
const wl_seat_listener seat_listener = {seat_capabilities, seat_name};

} // namespace

void input_bind_seat(wl_registry *registry, uint32_t name, uint32_t version) {
    if (s.seat)
        return;
    s.seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 8u)));
    wl_seat_add_listener(s.seat, &seat_listener, nullptr);
}

void input_reset() {
    s = {};
}

void input_set_cursor_shape_manager(wp_cursor_shape_manager_v1 *manager) {
    s.shapes = manager;
}

void input_forget_window(const Window *w) {
    bool display = w->display_id >= 0;
    if (s.pointer_window == w->id) {
        if (display)
            end_press(w);
        s.pointer_window = 0;
        s.buttons = 0;
        s.pressed = false;
        s.side_keys.clear();
    }
    if (s.key_window == w->id) {
        release_keys(w);
        s.key_window = 0;
    }
    if (s.touch_window == w->id) {
        if (display && !s.touches.empty())
            send_touches(w, ACTION_CANCEL);
        s.touches.clear();
        s.touch_window = 0;
    }
}

int64_t input_next_deadline() {
    return s.repeat_key ? s.repeat_due : INT64_MAX;
}

void input_run_timers(int64_t now) {
    if (!s.repeat_key || now < s.repeat_due)
        return;
    Window *w = live(s.key_window);
    if (!w || !s.repeat_rate) {
        s.repeat_key = 0;
        return;
    }
    s.repeat_count++;
    inject_key(w, KEY_ACTION_DOWN, s.repeat_key, s.repeat_count, s.repeat_down);
    s.repeat_due = std::max(now, s.repeat_due) + std::max(1, 1000 / s.repeat_rate);
}
