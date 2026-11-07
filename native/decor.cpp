// A title bar and resize edges that the window draws itself, for compositors that leave
// decorations to clients. GNOME's offers no xdg-decoration, so its windows would have neither.

#include "appdisplay.h"

#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cursor-shape-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "stb_truetype.h"
#pragma clang diagnostic pop

namespace {

// Logical pixels, after libadwaita's header bar and window controls.
constexpr int kTitleHeight = 46;
constexpr int kEdge = 10;            // invisible resize border around the window
constexpr int kCornerGrab = 16;      // how far along an edge a corner resize reaches
constexpr double kRadius = 12;       // the title bar's top corners
constexpr double kControl = 24;      // window control circles
constexpr double kControlSlot = 34;
constexpr double kBackWidth = 34, kBackHeight = 34, kBackRadius = 8;
constexpr double kSidePadding = 6;
constexpr double kFontEm = 14.67;    // 11 pt, as GNOME's title
constexpr int64_t kDoubleClickMs = 400;

const char *const kFonts[] = {"/system/fonts/SourceSansPro-Bold.ttf", "/system/fonts/Roboto-Regular.ttf"};

enum Part { NONE, TITLE, BACK, MINIMIZE, MAXIMIZE, CLOSE, EDGE };

struct Rgba {
    double r, g, b, a;
};

struct Palette {
    Rgba bg, bg_inactive, fg, shade;
};
// libadwaita's header bar colours
const Palette kDark = {{0x30 / 255.0, 0x30 / 255.0, 0x30 / 255.0, 1}, {0x24 / 255.0, 0x24 / 255.0, 0x24 / 255.0, 1},
                       {1, 1, 1, 1}, {0, 0, 0, 0.36}};
const Palette kLight = {{0xeb / 255.0, 0xeb / 255.0, 0xeb / 255.0, 1}, {0xfa / 255.0, 0xfa / 255.0, 0xfa / 255.0, 1},
                        {0, 0, 0, 0.8}, {0, 0, 0, 0.07}};

struct Button {
    Part part;
    double x, y, w, h; // logical, in the title bar
};

struct {
    wl_compositor *compositor = nullptr;
    wl_subcompositor *subcompositor = nullptr;
    wl_shm *shm = nullptr;
    wp_viewporter *viewporter = nullptr;
    wl_buffer *clear = nullptr; // 1x1 transparent, stretched over the resize border

    bool dark = true;
    std::vector<Part> left = {}, right = {MINIMIZE, MAXIMIZE, CLOSE};
    int layout_gen = 0;

    std::vector<unsigned char> font_data;
    stbtt_fontinfo font;
    bool font_ok = false;
    bool font_tried = false;

    // The pointer or touch on a decoration surface
    uint32_t pointer_window = 0;
    wl_surface *pointer_surface = nullptr;
    uint32_t enter_serial = 0;
    double px = 0, py = 0;
    uint32_t shape = 0;
    int64_t last_title_click = 0;
    double last_click_x = 0, last_click_y = 0;
    uint32_t touch_window = 0;
    int32_t touch_id = -1;
} d;

} // namespace

struct Decor {
    bool client_side = false;
    bool active = false, maximized = false, fullscreen = false, tiled = false;
    wl_surface *title = nullptr, *frame = nullptr;
    wl_subsurface *title_sub = nullptr, *frame_sub = nullptr;
    wp_viewport *title_vp = nullptr, *frame_vp = nullptr;
    std::vector<Button> buttons;
    Part hover = NONE, pressed = NONE;
    // What the title bar's buffer shows, to skip redrawing an unchanged one
    int drawn_w = 0, drawn_scale = 0;
    bool drawn_active = false, drawn_maximized = false, drawn_dark = false;
    int drawn_layout = -1;
    Part drawn_hover = NONE, drawn_pressed = NONE;
    std::string drawn_title;
    bool mapped = false;
    int width = 0, height = 0, scale120 = 120;
};

namespace {

bool shown(const Decor *c) {
    return c && c->client_side && !c->fullscreen && d.subcompositor;
}

void load_font() {
    if (d.font_tried)
        return;
    d.font_tried = true;
    for (const char *path : kFonts) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        off_t size = lseek(fd, 0, SEEK_END);
        if (size > 0 && size < (64 << 20)) {
            d.font_data.resize(size_t(size));
            if (pread(fd, d.font_data.data(), d.font_data.size(), 0) == size &&
                    stbtt_InitFont(&d.font, d.font_data.data(), stbtt_GetFontOffsetForIndex(d.font_data.data(), 0))) {
                d.font_ok = true;
                close(fd);
                return;
            }
        }
        close(fd);
    }
    ALOGW("no font for window titles");
}

void buffer_release(void *, wl_buffer *buffer) {
    wl_buffer_destroy(buffer);
}
const wl_buffer_listener buffer_listener = {buffer_release};

// Returns a premultiplied ARGB buffer of w x h pixels and its pixels, or nullptr.
wl_buffer *new_buffer(int w, int h, uint32_t **pixels) {
    size_t stride = size_t(w) * 4, size = stride * size_t(h);
    int fd = memfd_create("decor", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    if (ftruncate(fd, off_t(size)) < 0) {
        close(fd);
        return nullptr;
    }
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    wl_shm_pool *pool = wl_shm_create_pool(d.shm, fd, int32_t(size));
    wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, int32_t(stride), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    *pixels = static_cast<uint32_t *>(data);
    wl_buffer_add_listener(buffer, &buffer_listener, nullptr);
    return buffer;
}

// ---- Drawing, in buffer pixels ----

struct Canvas {
    uint32_t *px;
    int w, h;
    double s; // buffer pixels per logical pixel

    void blend(int x, int y, Rgba c, double cover) {
        if (x < 0 || y < 0 || x >= w || y >= h || cover <= 0)
            return;
        double a = c.a * std::min(cover, 1.0);
        uint32_t &p = px[size_t(y) * size_t(w) + size_t(x)];
        double inv = 1 - a;
        auto mix = [&](int shift, double v) {
            return uint32_t(std::lround(v * a * 255 + double((p >> shift) & 0xff) * inv)) << shift;
        };
        p = mix(24, 1) | mix(16, c.r) | mix(8, c.g) | mix(0, c.b);
    }

    // A rectangle with the given corners rounded, in logical pixels.
    void round_rect(double x0, double y0, double x1, double y1, double r, bool top, bool bottom, Rgba c) {
        int ix0 = int(std::floor(x0 * s)), iy0 = int(std::floor(y0 * s));
        int ix1 = int(std::ceil(x1 * s)), iy1 = int(std::ceil(y1 * s));
        double rs = r * s;
        for (int y = iy0; y < iy1; y++)
            for (int x = ix0; x < ix1; x++) {
                double fx = x + 0.5, fy = y + 0.5;
                double cover = std::min({fx - x0 * s + 0.5, x1 * s - fx + 0.5, fy - y0 * s + 0.5, y1 * s - fy + 0.5, 1.0});
                double cx = fx < x0 * s + rs ? x0 * s + rs : fx > x1 * s - rs ? x1 * s - rs : fx;
                bool in_top = top && fy < y0 * s + rs, in_bottom = bottom && fy > y1 * s - rs;
                if ((in_top || in_bottom) && cx != fx) {
                    double cy = in_top ? y0 * s + rs : y1 * s - rs;
                    cover = std::min(cover, rs + 0.5 - std::hypot(fx - cx, fy - cy));
                }
                blend(x, y, c, cover);
            }
    }

    void circle(double cx, double cy, double r, Rgba c) {
        round_rect(cx - r, cy - r, cx + r, cy + r, r, true, true, c);
    }

    // A stroke of the given width between two points, in logical pixels.
    void line(double ax, double ay, double bx, double by, double width, Rgba c) {
        ax *= s, ay *= s, bx *= s, by *= s;
        double half = width * s / 2;
        int x0 = int(std::floor(std::min(ax, bx) - half - 1)), x1 = int(std::ceil(std::max(ax, bx) + half + 1));
        int y0 = int(std::floor(std::min(ay, by) - half - 1)), y1 = int(std::ceil(std::max(ay, by) + half + 1));
        double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                double fx = x + 0.5, fy = y + 0.5;
                double t = len2 > 0 ? std::clamp(((fx - ax) * dx + (fy - ay) * dy) / len2, 0.0, 1.0) : 0;
                double dist = std::hypot(fx - (ax + t * dx), fy - (ay + t * dy));
                blend(x, y, c, half + 0.5 - dist);
            }
    }
};

std::u32string decode_utf8(const std::string &s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : c & (0x7f >> n);
        for (int k = 1; k < n && i + size_t(k) < s.size(); k++)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3f);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}

double text_width(const std::u32string &text, double scale) {
    double width = 0;
    for (size_t i = 0; i < text.size(); i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&d.font, int(text[i]), &advance, &lsb);
        width += advance * scale;
        if (i + 1 < text.size())
            width += stbtt_GetCodepointKernAdvance(&d.font, int(text[i]), int(text[i + 1])) * scale;
    }
    return width;
}

// Draws `title` centred in the window, kept between x_min and x_max (buffer pixels).
void draw_title(Canvas &cv, const std::string &title, double x_min, double x_max, Rgba fg) {
    load_font();
    if (!d.font_ok || x_max <= x_min)
        return;
    double scale = stbtt_ScaleForMappingEmToPixels(&d.font, float(kFontEm * cv.s));
    std::u32string text = decode_utf8(title);
    double width = text_width(text, scale);
    if (width > x_max - x_min) {
        char32_t ellipsis = stbtt_FindGlyphIndex(&d.font, 0x2026) ? U'\u2026' : U'.';
        std::u32string tail = ellipsis == U'.' ? U"..." : std::u32string(1, ellipsis);
        while (!text.empty() && text_width(text + tail, scale) > x_max - x_min)
            text.pop_back();
        text += tail;
        width = text_width(text, scale);
    }
    double x = std::clamp(cv.w / 2.0 - width / 2, x_min, std::max(x_min, x_max - width));
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&d.font, &ascent, &descent, &gap);
    double baseline = cv.h / 2.0 + (ascent + descent) * scale / 2;
    for (size_t i = 0; i < text.size(); i++) {
        int cp = int(text[i]);
        int advance, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&d.font, cp, &advance, &lsb);
        double shift = x - std::floor(x);
        stbtt_GetCodepointBitmapBoxSubpixel(&d.font, cp, float(scale), float(scale), float(shift), 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> glyph(size_t(gw) * size_t(gh));
            stbtt_MakeCodepointBitmapSubpixel(&d.font, glyph.data(), gw, gh, gw, float(scale), float(scale),
                    float(shift), 0, cp);
            int ox = int(std::floor(x)) + x0, oy = int(std::lround(baseline)) + y0;
            for (int gy = 0; gy < gh; gy++)
                for (int gx = 0; gx < gw; gx++)
                    cv.blend(ox + gx, oy + gy, fg, glyph[size_t(gy) * size_t(gw) + size_t(gx)] / 255.0);
        }
        x += advance * scale;
        if (i + 1 < text.size())
            x += stbtt_GetCodepointKernAdvance(&d.font, cp, int(text[i + 1])) * scale;
    }
}

// 16x16 icons after libadwaita's symbolic ones, with (ox, oy) their top left in logical pixels.
void draw_icon(Canvas &cv, Part part, bool maximized, double ox, double oy, Rgba fg) {
    constexpr double kStroke = 1.6;
    auto ln = [&](double ax, double ay, double bx, double by) { cv.line(ox + ax, oy + ay, ox + bx, oy + by, kStroke, fg); };
    switch (part) {
    case BACK:
        ln(10, 3.5, 5.5, 8);
        ln(5.5, 8, 10, 12.5);
        break;
    case MINIMIZE:
        ln(4.5, 11, 11.5, 11);
        break;
    case MAXIMIZE:
        if (maximized) {
            ln(4.5, 6.5, 9.5, 6.5), ln(9.5, 6.5, 9.5, 11.5), ln(9.5, 11.5, 4.5, 11.5), ln(4.5, 11.5, 4.5, 6.5);
            ln(6.5, 4.5, 11.5, 4.5), ln(11.5, 4.5, 11.5, 9.5);
        } else {
            ln(4.5, 4.5, 11.5, 4.5), ln(11.5, 4.5, 11.5, 11.5), ln(11.5, 11.5, 4.5, 11.5), ln(4.5, 11.5, 4.5, 4.5);
        }
        break;
    case CLOSE:
        ln(5, 5, 11, 11);
        ln(11, 5, 5, 11);
        break;
    default:
        break;
    }
}

void layout_buttons(Decor *c) {
    c->buttons.clear();
    double cy = kTitleHeight / 2.0;
    double x = kSidePadding;
    c->buttons.push_back({BACK, x, cy - kBackHeight / 2, kBackWidth, kBackHeight});
    x += kBackWidth;
    for (Part p : d.left) {
        c->buttons.push_back({p, x, cy - kControlSlot / 2, kControlSlot, kControlSlot});
        x += kControlSlot;
    }
    x = c->width - kSidePadding;
    for (auto it = d.right.rbegin(); it != d.right.rend(); ++it) {
        x -= kControlSlot;
        c->buttons.push_back({*it, x, cy - kControlSlot / 2, kControlSlot, kControlSlot});
    }
}

void draw(Window *w) {
    Decor *c = w->decor;
    int bw = std::max(1, int(std::ceil(c->width * c->scale120 / 120.0)));
    int bh = std::max(1, int(std::ceil(kTitleHeight * c->scale120 / 120.0)));
    uint32_t *pixels;
    wl_buffer *buffer = new_buffer(bw, bh, &pixels);
    if (!buffer)
        return;
    std::memset(pixels, 0, size_t(bw) * size_t(bh) * 4);
    Canvas cv{pixels, bw, bh, bw / double(c->width)};
    const Palette &pal = d.dark ? kDark : kLight;
    Rgba fg = pal.fg;
    if (!c->active)
        fg.a *= 0.5;
    bool round = !c->maximized && !c->tiled;
    cv.round_rect(0, 0, c->width, kTitleHeight, round ? kRadius : 0, round, false, c->active ? pal.bg : pal.bg_inactive);
    cv.round_rect(0, kTitleHeight - 1, c->width, kTitleHeight, 0, false, false, pal.shade);

    layout_buttons(c);
    double left_end = kSidePadding, right_start = c->width - kSidePadding;
    for (const Button &b : c->buttons) {
        double tint = b.part == c->pressed ? 0.3 : b.part == c->hover ? 0.15 : 0;
        Rgba bg = {pal.fg.r, pal.fg.g, pal.fg.b, 0};
        double ix = b.x + b.w / 2 - 8, iy = b.y + b.h / 2 - 8;
        if (b.part == BACK) {
            if (tint > 0) {
                bg.a = tint * 0.7;
                cv.round_rect(b.x, b.y, b.x + b.w, b.y + b.h, kBackRadius, true, true, bg);
            }
        } else {
            bg.a = 0.1 + tint;
            cv.circle(b.x + b.w / 2, b.y + b.h / 2, kControl / 2, bg);
        }
        draw_icon(cv, b.part, c->maximized, ix, iy, fg);
        if (b.x < c->width / 2.0)
            left_end = std::max(left_end, b.x + b.w);
        else
            right_start = std::min(right_start, b.x);
    }
    draw_title(cv, w->title, (left_end + 8) * cv.s, (right_start - 8) * cv.s, fg);

    wl_surface_attach(c->title, buffer, 0, 0);
    wl_surface_damage_buffer(c->title, 0, 0, INT32_MAX, INT32_MAX);
    wp_viewport_set_destination(c->title_vp, c->width, kTitleHeight);
    wl_surface_commit(c->title);
    munmap(pixels, size_t(bw) * size_t(bh) * 4);
    c->drawn_w = c->width;
    c->drawn_scale = c->scale120;
    c->drawn_active = c->active;
    c->drawn_maximized = c->maximized;
    c->drawn_dark = d.dark;
    c->drawn_layout = d.layout_gen;
    c->drawn_hover = c->hover;
    c->drawn_pressed = c->pressed;
    c->drawn_title = w->title;
}

bool stale(const Window *w) {
    const Decor *c = w->decor;
    return c->drawn_w != c->width || c->drawn_scale != c->scale120 || c->drawn_active != c->active ||
           c->drawn_maximized != c->maximized || c->drawn_dark != d.dark || c->drawn_layout != d.layout_gen ||
           c->drawn_hover != c->hover ||
           c->drawn_pressed != c->pressed || c->drawn_title != w->title;
}

void place(Window *w) {
    Decor *c = w->decor;
    if (!shown(c)) {
        if (c->mapped) {
            wl_surface_attach(c->title, nullptr, 0, 0);
            wl_surface_commit(c->title);
            wl_surface_attach(c->frame, nullptr, 0, 0);
            wl_surface_commit(c->frame);
            c->mapped = false;
        }
        xdg_surface_set_window_geometry(w->xsurface, 0, 0, std::max(1, c->width), std::max(1, c->height));
        return;
    }
    xdg_surface_set_window_geometry(w->xsurface, 0, -kTitleHeight, c->width, c->height + kTitleHeight);
    wl_subsurface_set_position(c->title_sub, 0, -kTitleHeight);
    if (!c->mapped || stale(w))
        draw(w);
    wl_subsurface_set_position(c->frame_sub, -kEdge, -kTitleHeight - kEdge);
    wl_surface_attach(c->frame, d.clear, 0, 0);
    wp_viewport_set_destination(c->frame_vp, c->width + 2 * kEdge, c->height + kTitleHeight + 2 * kEdge);
    bool edges = !c->maximized && !c->tiled;
    wl_region *region = wl_compositor_create_region(d.compositor);
    if (edges)
        wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_input_region(c->frame, region);
    wl_region_destroy(region);
    wl_surface_commit(c->frame);
    c->mapped = true;
}

// Redraws the title bar outside a configure, as for hover; the bar is a synchronized subsurface,
// so the window's surface commits too.
void refresh(Window *w) {
    if (!shown(w->decor) || !w->decor->mapped || !stale(w))
        return;
    draw(w);
    wl_surface_commit(w->surface);
}

Window *window_of(wl_surface *surface) {
    if (!surface)
        return nullptr;
    Window *w = window_by_id(uint32_t(uintptr_t(wl_surface_get_user_data(surface))));
    if (!w || !w->decor || w->closing)
        return nullptr;
    return surface == w->decor->title || surface == w->decor->frame ? w : nullptr;
}

Part part_at(const Window *w, double x, double y) {
    for (const Button &b : w->decor->buttons)
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
            return b.part;
    return TITLE;
}

uint32_t edge_at(const Window *w, double x, double y) {
    const Decor *c = w->decor;
    double fw = c->width + 2 * kEdge, fh = c->height + kTitleHeight + 2 * kEdge;
    bool left = x < kEdge, right = x >= fw - kEdge, top = y < kEdge, bottom = y >= fh - kEdge;
    if (top || bottom) {
        left = left || x < kEdge + kCornerGrab;
        right = right || x >= fw - kEdge - kCornerGrab;
    }
    if (left || right) {
        top = top || y < kEdge + kCornerGrab;
        bottom = bottom || y >= fh - kEdge - kCornerGrab;
    }
    uint32_t edge = 0;
    if (top)
        edge |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    if (bottom)
        edge |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    if (left)
        edge |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    if (right)
        edge |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    return edge;
}

uint32_t edge_shape(uint32_t edge) {
    switch (edge) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
    default: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
}

void update_pointer(Window *w) {
    Decor *c = w->decor;
    uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    Part hover = NONE;
    if (d.pointer_surface == c->frame)
        shape = edge_shape(edge_at(w, d.px, d.py));
    else {
        hover = part_at(w, d.px, d.py);
        if (hover == TITLE)
            hover = NONE;
    }
    if (shape != d.shape) {
        d.shape = shape;
        input_set_cursor_shape(d.enter_serial, shape);
    }
    if (hover != c->hover) {
        c->hover = hover;
        refresh(w);
    }
}

void activate(Window *w, Part part) {
    switch (part) {
    case BACK:
        input_send_back(w);
        break;
    case MINIMIZE:
        xdg_toplevel_set_minimized(w->toplevel);
        break;
    case MAXIMIZE:
        if (w->decor->maximized)
            xdg_toplevel_unset_maximized(w->toplevel);
        else
            xdg_toplevel_set_maximized(w->toplevel);
        break;
    case CLOSE:
        w->closing = true;
        break;
    default:
        break;
    }
}

void toggle_maximized(Window *w) {
    activate(w, MAXIMIZE);
}

std::vector<Part> parse_side(const std::string &side) {
    std::vector<Part> out;
    size_t start = 0;
    while (start <= side.size()) {
        size_t comma = side.find(',', start);
        std::string token = side.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (token == "minimize")
            out.push_back(MINIMIZE);
        else if (token == "maximize")
            out.push_back(MAXIMIZE);
        else if (token == "close")
            out.push_back(CLOSE);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return out;
}

} // namespace

void decor_init(wl_compositor *compositor, wl_subcompositor *subcompositor, wl_shm *shm, wp_viewporter *viewporter) {
    d.compositor = compositor;
    d.subcompositor = subcompositor;
    d.shm = shm;
    d.viewporter = viewporter;
    if (d.clear)
        wl_buffer_destroy(d.clear);
    d.clear = nullptr;
    d.pointer_window = d.touch_window = 0;
    d.pointer_surface = nullptr;
    if (!shm)
        return;
    uint32_t *pixels;
    // A buffer whose release would destroy it can't be shared, so this one has no listener.
    size_t size = 4;
    int fd = memfd_create("decor-clear", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, off_t(size)) < 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data != MAP_FAILED) {
        pixels = static_cast<uint32_t *>(data);
        pixels[0] = 0;
        munmap(data, size);
        wl_shm_pool *pool = wl_shm_create_pool(shm, fd, int32_t(size));
        d.clear = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
    close(fd);
}

void decor_set_appearance(bool dark, const std::string &layout) {
    d.dark = dark;
    size_t colon = layout.find(':');
    if (colon != std::string::npos) {
        d.left = parse_side(layout.substr(0, colon));
        d.right = parse_side(layout.substr(colon + 1));
    }
    d.layout_gen++;
}

void decor_create(Window *w, bool client_side) {
    auto *c = new Decor();
    w->decor = c;
    c->client_side = client_side;
    if (!d.subcompositor || !d.viewporter || !d.clear)
        return;
    void *tag = reinterpret_cast<void *>(uintptr_t(w->id));
    c->frame = wl_compositor_create_surface(d.compositor);
    wl_surface_set_user_data(c->frame, tag);
    c->frame_sub = wl_subcompositor_get_subsurface(d.subcompositor, c->frame, w->surface);
    wl_subsurface_place_below(c->frame_sub, w->surface);
    c->frame_vp = wp_viewporter_get_viewport(d.viewporter, c->frame);
    c->title = wl_compositor_create_surface(d.compositor);
    wl_surface_set_user_data(c->title, tag);
    c->title_sub = wl_subcompositor_get_subsurface(d.subcompositor, c->title, w->surface);
    c->title_vp = wp_viewporter_get_viewport(d.viewporter, c->title);
}

void decor_set_client_side(Window *w, bool on) {
    if (w->decor)
        w->decor->client_side = on;
}

void decor_states(Window *w, wl_array *states) {
    Decor *c = w->decor;
    if (!c)
        return;
    c->active = c->maximized = c->fullscreen = c->tiled = false;
    const auto *state = static_cast<const uint32_t *>(states->data);
    for (size_t i = 0; i < states->size / sizeof(*state); i++) {
        switch (state[i]) {
        case XDG_TOPLEVEL_STATE_ACTIVATED: c->active = true; break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED: c->maximized = true; break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN: c->fullscreen = true; break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM: c->tiled = true; break;
        default: break;
        }
    }
}

int decor_title_height(const Window *w) {
    return shown(w->decor) ? kTitleHeight : 0;
}

void decor_update(Window *w, int scale120) {
    if (!w->decor || !w->decor->title)
        return;
    w->decor->width = w->width;
    w->decor->height = w->height;
    w->decor->scale120 = scale120 > 0 ? scale120 : 120;
    place(w);
}

void decor_destroy(Window *w) {
    Decor *c = w->decor;
    if (!c)
        return;
    if (d.pointer_window == w->id) {
        d.pointer_window = 0;
        d.pointer_surface = nullptr;
    }
    if (d.touch_window == w->id)
        d.touch_window = 0;
    if (c->title) {
        wp_viewport_destroy(c->title_vp);
        wl_subsurface_destroy(c->title_sub);
        wl_surface_destroy(c->title);
        wp_viewport_destroy(c->frame_vp);
        wl_subsurface_destroy(c->frame_sub);
        wl_surface_destroy(c->frame);
    }
    delete c;
    w->decor = nullptr;
}

Window *decor_window_for_surface(wl_surface *surface) {
    return window_of(surface);
}

bool decor_pointer_enter(wl_surface *surface, uint32_t serial, double x, double y) {
    Window *w = window_of(surface);
    if (!w)
        return false;
    d.pointer_window = w->id;
    d.pointer_surface = surface;
    d.enter_serial = serial;
    d.px = x;
    d.py = y;
    d.shape = UINT32_MAX;
    update_pointer(w);
    return true;
}

void decor_pointer_motion(double x, double y) {
    d.px = x;
    d.py = y;
    if (Window *w = window_by_id(d.pointer_window); w && w->decor)
        update_pointer(w);
}

void decor_pointer_leave() {
    Window *w = window_by_id(d.pointer_window);
    d.pointer_window = 0;
    d.pointer_surface = nullptr;
    if (w && w->decor) {
        w->decor->hover = w->decor->pressed = NONE;
        refresh(w);
    }
}

void decor_pointer_button(uint32_t serial, uint32_t button, bool pressed) {
    Window *w = window_by_id(d.pointer_window);
    if (!w || !w->decor || w->closing)
        return;
    Decor *c = w->decor;
    wl_seat *seat = input_seat();
    if (d.pointer_surface == c->frame) {
        uint32_t edge = edge_at(w, d.px, d.py);
        if (pressed && button == BTN_LEFT && edge && seat)
            xdg_toplevel_resize(w->toplevel, seat, serial, edge);
        return;
    }
    Part part = part_at(w, d.px, d.py);
    if (part == TITLE) {
        if (!pressed || !seat)
            return;
        if (button == BTN_RIGHT) {
            xdg_toplevel_show_window_menu(w->toplevel, seat, serial, int32_t(d.px), int32_t(d.py - kTitleHeight));
        } else if (button == BTN_MIDDLE) {
            toggle_maximized(w);
        } else if (button == BTN_LEFT) {
            int64_t now = now_ms();
            bool twice = now - d.last_title_click < kDoubleClickMs && std::abs(d.px - d.last_click_x) < 8 &&
                         std::abs(d.py - d.last_click_y) < 8;
            d.last_title_click = twice ? 0 : now;
            d.last_click_x = d.px;
            d.last_click_y = d.py;
            if (twice)
                toggle_maximized(w);
            else
                xdg_toplevel_move(w->toplevel, seat, serial);
        }
        return;
    }
    if (button != BTN_LEFT)
        return;
    if (pressed) {
        c->pressed = part;
    } else {
        Part was = c->pressed;
        c->pressed = NONE;
        if (was == part)
            activate(w, part);
    }
    if (!w->closing)
        refresh(w);
}

bool decor_touch_down(wl_surface *surface, uint32_t serial, int32_t id, double x, double y) {
    Window *w = window_of(surface);
    if (!w)
        return false;
    if (d.touch_window)
        return true;
    Decor *c = w->decor;
    wl_seat *seat = input_seat();
    if (surface == c->frame) {
        uint32_t edge = edge_at(w, x, y);
        if (edge && seat)
            xdg_toplevel_resize(w->toplevel, seat, serial, edge);
        return true;
    }
    Part part = part_at(w, x, y);
    if (part == TITLE) {
        if (seat)
            xdg_toplevel_move(w->toplevel, seat, serial);
        return true;
    }
    d.touch_window = w->id;
    d.touch_id = id;
    c->pressed = part;
    refresh(w);
    return true;
}

bool decor_touch_up(int32_t id) {
    if (!d.touch_window || id != d.touch_id)
        return false;
    Window *w = window_by_id(d.touch_window);
    d.touch_window = 0;
    if (w && w->decor && !w->closing) {
        Part part = w->decor->pressed;
        w->decor->pressed = NONE;
        activate(w, part);
        if (!w->closing)
            refresh(w);
    }
    return true;
}
