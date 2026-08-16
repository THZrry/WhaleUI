/* Renderer: CPU paint into an RGBA framebuffer, uploaded to an offscreen GPU
 * texture and blitted to the swapchain (SDL built-in blit pipeline; custom
 * shaders are a later step). Text comes from SDL3_ttf using fonts registered
 * through the font module. */

#include "render/render.h"
#include "render/fsr_shaders.h"
#include "render/fsr_dxil.h"
#include "render/fsr_demo_spv.h"
#include "render/fsr_rcas_custom_spv.h"
#include "render/gpu.h"
#include "render/gpu_shaders.h"
#include "animate/animate.h"
#include "font/font.h"
#include "style/style.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#ifdef WHALEUI_BUILD_FULL
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>
#else
/* stb_truetype text backend for lite/minimal builds (header-only) */
#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
#include <stb/stb_truetype.h>
#include <stb/stb_image.h>

#include <lexbor/dom/dom.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace {

/* GPU draw-list target for the current paint pass (single-threaded; set by
 * render_frame before painting, cleared after). When set, every fill/rect
 * call appends a batched command instead of writing CPU pixels. */
whaleui_gpu_t* g_gpu = nullptr;

/* backdrop-filter pass C runs only on full repaints (flush skips it on
 * scroll/partial frames); paint_node must then paint the backdrop element's
 * own background normally instead of leaving the region transparent. */
int g_backdrop_active = 0;

/* font style bits (mirror SDL_ttf's TTF_STYLE_*; local constants so lite/
 * minimal builds don't need SDL3_ttf headers - the values match) */
enum { kFontBold = 1, kFontItalic = 2 };

/* shadow parsing (defined below; forward-declared for paint_text) */
int parse_shadow_any(const std::string& v, int& ox, int& oy, int& blur,
                     unsigned int& col);
std::vector<std::string> split_space2(const std::string& v);

/* last layout tree rendered by any window (DOM geometry queries);
 * NULL until the first frame. The tree itself is owned by the render
 * context that produced it. */
whaleui_layout_tree_t* g_last_tree = nullptr;

/* --- color --- */

struct NamedColor { const char* name; unsigned int value; };

const NamedColor k_named[] = {
    {"black", 0xFF000000}, {"silver", 0xFFC0C0C0}, {"gray", 0xFF808080},
    {"white", 0xFFFFFFFF}, {"maroon", 0xFF800000}, {"red", 0xFFFF0000},
    {"purple", 0xFF800080}, {"fuchsia", 0xFFFF00FF}, {"green", 0xFF008000},
    {"lime", 0xFF00FF00}, {"olive", 0xFF808000}, {"yellow", 0xFFFFFF00},
    {"navy", 0xFF000080}, {"blue", 0xFF0000FF}, {"teal", 0xFF008080},
    {"aqua", 0xFF00FFFF}, {"orange", 0xFFFFA500}, {"pink", 0xFFFFC0CB},
    {"brown", 0xFFA52A2A}, {"transparent", 0x00000000},
};

unsigned int hex_byte(const char* s, int* ok)
{
    unsigned int v = 0;
    for (int i = 0; i < 2; ++i) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= static_cast<unsigned>(c - 'A' + 10);
        } else {
            *ok = 0;
            return 0;
        }
    }
    return v;
}

unsigned int hex_nib(char c, int* ok)
{
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned>(c - 'A' + 10);
    }
    *ok = 0;
    return 0;
}

/* --- framebuffer helpers (RGBA8, 0xAARRGGBB) --- */

struct Clip { int x, y, w, h; };

/* intersect [x0,x1)x[y0,y1) with clip (NULL = no clip) */
/* grow a clip rect to also contain (x0,y0)-(x1,y1); when out is empty it
 * takes the rect */
void dirty_rect(int x0, int y0, int x1, int y1, Clip* out)
{
    if (!out) {
        return;
    }
    if (out->w <= 0 || out->h <= 0) {
        out->x = x0;
        out->y = y0;
        out->w = x1 - x0;
        out->h = y1 - y0;
        return;
    }
    int ax0 = out->x < x0 ? out->x : x0;
    int ay0 = out->y < y0 ? out->y : y0;
    int ax1 = (out->x + out->w) > x1 ? (out->x + out->w) : x1;
    int ay1 = (out->y + out->h) > y1 ? (out->y + out->h) : y1;
    out->x = ax0;
    out->y = ay0;
    out->w = ax1 - ax0;
    out->h = ay1 - ay0;
}

void clip_rect(int& x0, int& y0, int& x1, int& y1, const Clip* clip)
{
    if (!clip || clip->w <= 0 || clip->h <= 0) {
        return;
    }
    int cx0 = clip->x, cy0 = clip->y;
    int cx1 = clip->x + clip->w, cy1 = clip->y + clip->h;
    if (x0 < cx0) { x0 = cx0; }
    if (y0 < cy0) { y0 = cy0; }
    if (x1 > cx1) { x1 = cx1; }
    if (y1 > cy1) { y1 = cy1; }
}

void fill_rect(std::vector<unsigned int>& fb, int fbw, int fbh,
               int x, int y, int w, int h, unsigned int color, const Clip* clip)
{
    if (g_gpu) {
        int c[4];
        const int* cp = nullptr;
        if (clip) {
            c[0] = clip->x;
            c[1] = clip->y;
            c[2] = clip->w;
            c[3] = clip->h;
            cp = c;
        }
        whaleui_gpu_rect(g_gpu, static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(w), static_cast<float>(h),
                         0.0f, color, cp);
        return;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > fbw ? fbw : x + w;
    int y1 = y + h > fbh ? fbh : y + h;
    clip_rect(x0, y0, x1, y1, clip);
    unsigned int a = (color >> 24) & 0xFF;
    if (a == 0 || x1 <= x0 || y1 <= y0) {
        return;
    }
    if (a == 255) {
        for (int yy = y0; yy < y1; ++yy) {
            std::fill(fb.begin() + static_cast<size_t>(yy) * fbw + x0,
                      fb.begin() + static_cast<size_t>(yy) * fbw + x1, color);
        }
        return;
    }
    unsigned int r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            unsigned int& d = fb[static_cast<size_t>(yy) * fbw + xx];
            unsigned int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            unsigned int nr = (r * a + dr * (255 - a)) / 255;
            unsigned int ng = (g * a + dg * (255 - a)) / 255;
            unsigned int nb = (b * a + db * (255 - a)) / 255;
            d = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
        }
    }
}

/* --- CSS gradients (linear/radial) --- */

struct GradStop
{
    unsigned int c;
    float pos; /* 0..1 */
};

struct Gradient
{
    int type;                  /* 0=linear, 1=radial */
    float angle_deg;           /* linear direction */
    float rx, ry;              /* radial radii (px) */
    float cx, cy;              /* radial center (fraction of the box) */
    std::vector<GradStop> stops;
};

/* split on top-level commas (function calls keep their own) */
void split_grad_parts(const std::string& s, std::vector<std::string>& out)
{
    size_t depth = 0, start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i < s.size() ? s[i] : ',';
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth > 0) {
                --depth;
            }
        } else if (c == ',' && depth == 0) {
            std::string part = s.substr(start, i - start);
            size_t b = part.find_first_not_of(" \t");
            size_t e = part.find_last_not_of(" \t");
            if (b != std::string::npos) {
                out.push_back(part.substr(b, e - b + 1));
            }
            start = i + 1;
        }
    }
}

/* parse one gradient from a background value; returns 1 when found */
int parse_gradient(const std::string& v, Gradient& out)
{
    size_t li = v.find("linear-gradient(");
    size_t ri = v.find("radial-gradient(");
    if (li == std::string::npos && ri == std::string::npos) {
        return 0;
    }
    bool lin = li != std::string::npos &&
               (ri == std::string::npos || li < ri);
    size_t start = lin ? li : ri;
    /* "linear-gradient(" / "radial-gradient(" are both 16 chars */
    size_t open = start + 16;
    size_t depth = 1;
    size_t end = std::string::npos;
    for (size_t i = open; i < v.size(); ++i) {
        if (v[i] == '(') {
            ++depth;
        } else if (v[i] == ')') {
            if (--depth == 0) {
                end = i;
                break;
            }
        }
    }
    if (end == std::string::npos) {
        return 0;
    }
    out = Gradient();
    out.type = lin ? 0 : 1;
    out.angle_deg = 180.0f; /* default: to bottom */
    out.rx = 0;
    out.ry = 0;
    out.cx = 0.5f;
    out.cy = 0.5f;
    std::vector<std::string> parts;
    split_grad_parts(v.substr(open, end - open), parts);
    if (parts.empty()) {
        return 0;
    }
    size_t first = 0;
    if (!lin) {
        /* radial first part: "1100px 560px at 72% -12%" / "circle 200px" */
        const std::string& head = parts[0];
        size_t at = head.find(" at ");
        std::string size_s = at == std::string::npos ? head : head.substr(0, at);
        std::string pos_s = at == std::string::npos ? std::string() : head.substr(at + 4);
        if (!pos_s.empty()) {
            float px = 0, py = 0;
            if (std::sscanf(pos_s.c_str(), "%f%% %f%%", &px, &py) == 2) {
                out.cx = px / 100.0f;
                out.cy = py / 100.0f;
            } else if (std::sscanf(pos_s.c_str(), "%fpx %fpx", &px, &py) == 2) {
                out.cx = px;
                out.cy = py;
                out.cx += 100.0f; /* marker: absolute px center */
            }
        }
        float rx = 0, ry = 0;
        if (std::sscanf(size_s.c_str(), "%fpx %fpx", &rx, &ry) == 2) {
            out.rx = rx;
            out.ry = ry;
        } else if (size_s.find("circle") != std::string::npos) {
            const char* c = std::strchr(size_s.c_str(), ' ');
            if (c) {
                out.rx = out.ry = static_cast<float>(std::atof(c));
            }
        }
        first = 1;
    } else {
        /* linear first part: "90deg" / "to right" */
        const std::string& head = parts[0];
        if (head.find("deg") != std::string::npos) {
            out.angle_deg = static_cast<float>(std::atof(head.c_str()));
        } else if (head == "to right") {
            out.angle_deg = 90.0f;
        } else if (head == "to left") {
            out.angle_deg = 270.0f;
        } else if (head == "to top") {
            out.angle_deg = 0.0f;
        } /* "to bottom" = default 180 */
        first = 1;
    }
    /* stops: "color pos" / "color" */
    std::vector<size_t> auto_pos;
    for (size_t i = first; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        size_t sp = std::string::npos;
        for (size_t j = 1; j < p.size(); ++j) {
            if (p[j] == ' ' && p[j - 1] != ' ') {
                sp = j;
                break;
            }
        }
        std::string col_s = sp == std::string::npos ? p : p.substr(0, sp);
        unsigned int c = 0;
        if (whaleui_render_parse_color(col_s.c_str(), &c) != 0) {
            continue;
        }
        GradStop st;
        st.c = c;
        st.pos = -1.0f;
        if (sp != std::string::npos) {
            std::string pos_s = p.substr(sp);
            const char* pc = pos_s.c_str();
            while (*pc == ' ' || *pc == '\t') {
                ++pc;
            }
            if (std::strchr(pc, '%')) {
                st.pos = static_cast<float>(std::atof(pc)) / 100.0f;
            } else {
                st.pos = static_cast<float>(std::atof(pc));
            }
        }
        out.stops.push_back(st);
        if (st.pos < 0) {
            auto_pos.push_back(out.stops.size() - 1);
        }
    }
    /* fill unspecified positions evenly */
    size_t n = out.stops.size();
    if (n == 0) {
        return 0;
    }
    if (auto_pos.empty()) {
        for (size_t i = 0; i < n; ++i) {
            out.stops[i].pos = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
        }
    } else {
        for (size_t i = 0; i < auto_pos.size(); ++i) {
            size_t idx = auto_pos[i];
            float lo = idx == 0 ? 0.0f : out.stops[idx - 1].pos;
            float hi = (idx == 0 && n > 1) ? 1.0f
                                           : (idx + 1 < n ? out.stops[idx + 1].pos : 1.0f);
            if (idx > 0 && idx + 1 < n && out.stops[idx + 1].pos >= 0 &&
                out.stops[idx - 1].pos >= 0) {
                lo = out.stops[idx - 1].pos;
                hi = out.stops[idx + 1].pos;
            } else if (idx == 0) {
                lo = 0.0f;
            } else {
                hi = 1.0f;
            }
            out.stops[idx].pos = hi >= lo ? (lo + hi) / 2.0f : lo;
        }
    }
    return 1;
}

unsigned int grad_color(const Gradient& g, float t)
{
    size_t n = g.stops.size();
    if (n == 0) {
        return 0;
    }
    if (t <= 0.0f || t <= g.stops[0].pos) {
        return g.stops[0].c;
    }
    if (t >= g.stops[n - 1].pos) {
        return g.stops[n - 1].c;
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        if (t <= g.stops[i + 1].pos) {
            float span = g.stops[i + 1].pos - g.stops[i].pos;
            float p = span > 0 ? (t - g.stops[i].pos) / span : 0.0f;
            unsigned int f = g.stops[i].c, g2 = g.stops[i + 1].c;
            auto ch = [p](unsigned int a, unsigned int b) -> unsigned int {
                return static_cast<unsigned int>(a + (static_cast<float>(b) - a) * p + 0.5f);
            };
            return (ch((f >> 24) & 0xFF, (g2 >> 24) & 0xFF) << 24) |
                   (ch((f >> 16) & 0xFF, (g2 >> 16) & 0xFF) << 16) |
                   (ch((f >> 8) & 0xFF, (g2 >> 8) & 0xFF) << 8) |
                   ch(f & 0xFF, g2 & 0xFF);
        }
    }
    return g.stops[n - 1].c;
}

/* paint a gradient over (x,y,w,h); alpha-blends over existing pixels */
void fill_gradient(std::vector<unsigned int>& fb, int fbw, int fbh,
                   int x, int y, int w, int h, const Gradient& g,
                   const Clip* clip)
{
    if (g_gpu) {
        /* linear gradients: corner colors (projected onto the gradient
         * axis) interpolated by the solid pipeline - free from the
         * rasterizer. Stops beyond the first two approximate with the
         * first/last color (ponytail: per-stop segments when a page needs
         * them). Radial needs per-pixel work the graphics pipeline cannot
         * do (SDL 3.4 D3D12 graphics SRV is broken), so it is skipped -
         * the flat element background underneath stays correct. */
        if (g.type == 0 && g.stops.size() >= 2) {
            float rad = g.angle_deg * 3.14159265f / 180.0f;
            float ddx = std::sin(rad), ddy = -std::cos(rad);
            float ccx = x + w * 0.5f, ccy = y + h * 0.5f;
            float t0 = 1e30f, t1 = -1e30f;
            const float corners[4][2] = {
                {static_cast<float>(x), static_cast<float>(y)},
                {static_cast<float>(x + w), static_cast<float>(y)},
                {static_cast<float>(x), static_cast<float>(y + h)},
                {static_cast<float>(x + w), static_cast<float>(y + h)}};
            for (int i = 0; i < 4; ++i) {
                float t = (corners[i][0] - ccx) * ddx +
                          (corners[i][1] - ccy) * ddy;
                if (t < t0) {
                    t0 = t;
                }
                if (t > t1) {
                    t1 = t;
                }
            }
            float span = t1 - t0;
            if (span <= 0.0f) {
                span = 1.0f;
            }
            auto tcol = [&](float px, float py) {
                float t = ((px - ccx) * ddx + (py - ccy) * ddy - t0) / span;
                return grad_color(g, t);
            };
            unsigned int c_tl = tcol(static_cast<float>(x), static_cast<float>(y));
            unsigned int c_tr = tcol(static_cast<float>(x + w), static_cast<float>(y));
            unsigned int c_bl = tcol(static_cast<float>(x), static_cast<float>(y + h));
            unsigned int c_br = tcol(static_cast<float>(x + w), static_cast<float>(y + h));
            int c2[4];
            const int* cp = nullptr;
            if (clip) {
                c2[0] = clip->x;
                c2[1] = clip->y;
                c2[2] = clip->w;
                c2[3] = clip->h;
                cp = c2;
            }
            whaleui_gpu_gradient_rect(g_gpu, static_cast<float>(x),
                                      static_cast<float>(y),
                                      static_cast<float>(w),
                                      static_cast<float>(h), c_tl, c_tr, c_bl,
                                      c_br, cp);
        } else if (g.type == 1 && g.stops.size() >= 2) {
            /* radial: concentric rounded-rect ellipses, alpha fading
             * outwards - a coarse approximation of the per-pixel ellipse
             * distance (10 layers, GPU-trivial) */
            float rx = g.rx > 0 ? g.rx : w * 0.5f;
            float ry = g.ry > 0 ? g.ry : h * 0.5f;
            float rcx = g.cx > 1.0f ? g.cx - 100.0f : x + g.cx * w;
            float rcy = g.cy * h + y;
            int c2[4];
            const int* cp = nullptr;
            if (clip) {
                c2[0] = clip->x;
                c2[1] = clip->y;
                c2[2] = clip->w;
                c2[3] = clip->h;
                cp = c2;
            }
            const int kLayers = 10;
            for (int i = kLayers; i >= 1; --i) {
                float t = static_cast<float>(i) / kLayers;
                unsigned int c = grad_color(g, t);
                if (((c >> 24) & 0xFF) == 0) {
                    continue;
                }
                float sx = rcx - rx * t;
                float sy = rcy - ry * t;
                whaleui_gpu_rect(g_gpu, sx, sy, rx * 2.0f * t,
                                 ry * 2.0f * t, rx * t, c, cp);
            }
        }
        return;
    }
    if (w <= 0 || h <= 0 || g.stops.empty()) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > fbw ? fbw : x + w;
    int y1 = y + h > fbh ? fbh : y + h;
    clip_rect(x0, y0, x1, y1, clip);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    auto blend = [&](int px, int py, unsigned int c) {
        unsigned int a = (c >> 24) & 0xFF;
        if (a == 0) {
            return;
        }
        unsigned int& d = fb[static_cast<size_t>(py) * fbw + px];
        if (a == 255) {
            d = 0xFF000000 | (c & 0x00FFFFFF);
            return;
        }
        unsigned int cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
        unsigned int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
        d = 0xFF000000 |
            (((cr * a + dr * (255 - a)) / 255) << 16) |
            (((cg * a + dg * (255 - a)) / 255) << 8) |
            ((cb * a + db * (255 - a)) / 255);
    };
    if (g.type == 0) {
        /* linear: project pixels onto the direction axis */
        float rad = g.angle_deg * 3.14159265f / 180.0f;
        float dx = std::sin(rad), dy = -std::cos(rad);
        float cx = x + w * 0.5f, cy = y + h * 0.5f;
        float t0 = 1e30f, t1 = -1e30f;
        const float corners[4][2] = {
            {static_cast<float>(x), static_cast<float>(y)},
            {static_cast<float>(x + w), static_cast<float>(y)},
            {static_cast<float>(x), static_cast<float>(y + h)},
            {static_cast<float>(x + w), static_cast<float>(y + h)}};
        for (int i = 0; i < 4; ++i) {
            float t = (corners[i][0] - cx) * dx + (corners[i][1] - cy) * dy;
            if (t < t0) {
                t0 = t;
            }
            if (t > t1) {
                t1 = t;
            }
        }
        float span = t1 - t0;
        if (span <= 0.0f) {
            span = 1.0f;
        }
        for (int py = y0; py < y1; ++py) {
            float t = ((static_cast<float>(x0) - cx) * dx +
                       (static_cast<float>(py) - cy) * dy - t0) /
                      span;
            float step = dx / span;
            for (int px = x0; px < x1; ++px) {
                blend(px, py, grad_color(g, t));
                t += step;
            }
        }
    } else {
        /* radial: ellipse distance from the center */
        float rx = g.rx > 0 ? g.rx : w * 0.5f;
        float ry = g.ry > 0 ? g.ry : h * 0.5f;
        float rcx = g.cx > 1.0f ? g.cx - 100.0f : x + g.cx * w;
        float rcy = g.cy * h + y;
        if (rx <= 0 || ry <= 0) {
            return;
        }
        for (int py = y0; py < y1; ++py) {
            float ny = (static_cast<float>(py) - rcy) / ry;
            for (int px = x0; px < x1; ++px) {
                float nx = (static_cast<float>(px) - rcx) / rx;
                float t = std::sqrt(nx * nx + ny * ny);
                blend(px, py, grad_color(g, t));
            }
        }
    }
}

/* is (px,py) inside a rounded rect (x,y,w,h,radius)? */
bool inside_rounded(int px, int py, int x, int y, int w, int h, int radius)
{
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (radius <= 0) {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    long dx = 0, dy = 0;
    if (px < x + radius) {
        dx = static_cast<long>(x + radius - px);
    } else if (px >= x + w - radius) {
        dx = static_cast<long>(px - (x + w - radius)) + 1;
    }
    if (py < y + radius) {
        dy = static_cast<long>(y + radius - py);
    } else if (py >= y + h - radius) {
        dy = static_cast<long>(py - (y + h - radius)) + 1;
    }
    if (dx != 0 || dy != 0) {
        long r2 = static_cast<long>(radius) * radius;
        return dx * dx + dy * dy <= r2;
    }
    return true; /* in the middle band */
}

/* rounded rect: radius clipped to half the smaller side */
void fill_round_rect(std::vector<unsigned int>& fb, int fbw, int fbh,
                     int x, int y, int w, int h, int radius,
                     unsigned int color, const Clip* clip)
{
    if (g_gpu) {
        int c[4];
        const int* cp = nullptr;
        if (clip) {
            c[0] = clip->x;
            c[1] = clip->y;
            c[2] = clip->w;
            c[3] = clip->h;
            cp = c;
        }
        whaleui_gpu_rect(g_gpu, static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(w), static_cast<float>(h),
                         static_cast<float>(radius), color, cp);
        return;
    }
    if (radius <= 0) {
        fill_rect(fb, fbw, fbh, x, y, w, h, color, clip);
        return;
    }
    int half = w < h ? w / 2 : h / 2;
    if (radius > half) {
        radius = half;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > fbw ? fbw : x + w;
    int y1 = y + h > fbh ? fbh : y + h;
    clip_rect(x0, y0, x1, y1, clip);
    unsigned int a = (color >> 24) & 0xFF;
    if (a == 0 || x1 <= x0 || y1 <= y0) {
        return;
    }
    unsigned int r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            if (!inside_rounded(xx, yy, x, y, w, h, radius)) {
                continue; /* outside the rounded corner */
            }
            unsigned int& d = fb[static_cast<size_t>(yy) * fbw + xx];
            unsigned int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            if (a == 255) {
                d = color;
            } else {
                unsigned int nr = (r * a + dr * (255 - a)) / 255;
                unsigned int ng = (g * a + dg * (255 - a)) / 255;
                unsigned int nb = (b * a + db * (255 - a)) / 255;
                d = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
            }
        }
    }
}

/* rounded border ring: area between the outer rounded rect and the same rect
 * inset by `bw`, so borders follow the corner arcs instead of going square */
void fill_round_border(std::vector<unsigned int>& fb, int fbw, int fbh,
                       int x, int y, int w, int h, int radius, int bw,
                       unsigned int color, unsigned int bg, const Clip* clip)
{
    if (g_gpu) {
        /* ring via two rounded rects: outer in the border color, then the
         * inner rect re-filled with the element's background color */
        fill_round_rect(fb, fbw, fbh, x, y, w, h, radius, color, clip);
        int ix = x + bw, iy = y + bw, iw = w - 2 * bw, ih = h - 2 * bw;
        int irad = radius - bw;
        if (irad < 0) {
            irad = 0;
        }
        fill_round_rect(fb, fbw, fbh, ix, iy, iw, ih, irad, bg, clip);
        return;
    }
    if (bw <= 0) {
        return;
    }
    if (radius <= 0) {
        fill_rect(fb, fbw, fbh, x, y, w, bw, color, clip);               /* top */
        fill_rect(fb, fbw, fbh, x, y + h - bw, w, bw, color, clip);      /* bottom */
        fill_rect(fb, fbw, fbh, x + w - bw, y, bw, h, color, clip);      /* right */
        fill_rect(fb, fbw, fbh, x, y, bw, h, color, clip);               /* left */
        return;
    }
    int half = w < h ? w / 2 : h / 2;
    if (radius > half) {
        radius = half;
    }
    int ix = x + bw, iy = y + bw, iw = w - 2 * bw, ih = h - 2 * bw;
    int irad = radius - bw;
    if (irad < 0) {
        irad = 0;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > fbw ? fbw : x + w;
    int y1 = y + h > fbh ? fbh : y + h;
    clip_rect(x0, y0, x1, y1, clip);
    unsigned int a = (color >> 24) & 0xFF;
    if (a == 0 || x1 <= x0 || y1 <= y0) {
        return;
    }
    unsigned int r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            if (!inside_rounded(xx, yy, x, y, w, h, radius)) {
                continue; /* outside the outer rounded rect */
            }
            if (inside_rounded(xx, yy, ix, iy, iw, ih, irad)) {
                continue; /* inside the inner rect: that is the fill area */
            }
            unsigned int& d = fb[static_cast<size_t>(yy) * fbw + xx];
            unsigned int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            if (a == 255) {
                d = color;
            } else {
                unsigned int nr = (r * a + dr * (255 - a)) / 255;
                unsigned int ng = (g * a + dg * (255 - a)) / 255;
                unsigned int nb = (b * a + db * (255 - a)) / 255;
                d = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
            }
        }
    }
}

void blend_surface(std::vector<unsigned int>& fb, int fbw, int fbh,
                   const SDL_Surface* surf, int dx, int dy, const Clip* clip,
                   const unsigned int* tint)
{
    if (!surf || !surf->pixels) {
        return;
    }
    const unsigned char* src = static_cast<const unsigned char*>(surf->pixels);
    int pitch = surf->pitch;
    unsigned int tr = tint ? (*tint >> 16) & 0xFF : 0;
    unsigned int tg = tint ? (*tint >> 8) & 0xFF : 0;
    unsigned int tb = tint ? *tint & 0xFF : 0;
    for (int y = 0; y < surf->h; ++y) {
        int ty = dy + y;
        if (ty < 0 || ty >= fbh) {
            continue;
        }
        if (clip && (ty < clip->y || ty >= clip->y + clip->h)) {
            continue;
        }
        for (int x = 0; x < surf->w; ++x) {
            int tx = dx + x;
            if (tx < 0 || tx >= fbw) {
                continue;
            }
            if (clip && (tx < clip->x || tx >= clip->x + clip->w)) {
                continue;
            }
            const unsigned char* s = src + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
            unsigned int a = s[3];
            if (a == 0) {
                continue;
            }
            unsigned int sr = tint ? tr : s[0];
            unsigned int sg = tint ? tg : s[1];
            unsigned int sb = tint ? tb : s[2];
            unsigned int& d = fb[static_cast<size_t>(ty) * fbw + tx];
            if (a == 255) {
                /* opaque: no blending math (hot path for anti-aliased
                 * glyph cores; text is the biggest per-frame cost) */
                d = 0xFF000000 | (sr << 16) | (sg << 8) | sb;
                continue;
            }
            unsigned int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            unsigned int nr = (sr * a + dr * (255 - a)) / 255;
            unsigned int ng = (sg * a + dg * (255 - a)) / 255;
            unsigned int nb = (sb * a + db * (255 - a)) / 255;
            d = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
        }
    }
}

/* style helpers */
std::string sget(const WhaleUIComputedStyle& s, const char* k)
{
    auto it = s.find(k);
    return it == s.end() ? std::string() : it->second;
}

/* --- editable elements + text editing --- */

bool tag_eq(lxb_dom_element* el, const char* tag)
{
    if (!el) {
        return false;
    }
    size_t n = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(el, &n);
    if (!name) {
        return false;
    }
    size_t tlen = std::strlen(tag);
    return n == tlen && std::memcmp(name, tag, tlen) == 0;
}

/* input[type=checkbox/radio]: native control drawn + toggled by the engine */
bool is_check_radio(lxb_dom_element* el)
{
    if (!tag_eq(el, "input")) {
        return false;
    }
    size_t alen = 0;
    const lxb_char_t* t = lxb_dom_element_get_attribute(
        el, (const lxb_char_t*)"type", 4, &alen);
    return t && ((alen == 8 && std::memcmp(t, "checkbox", 8) == 0) ||
                 (alen == 5 && std::memcmp(t, "radio", 5) == 0));
}

/* is this element a text-editing target? input(type=text)/textarea, or any
 * element with contenteditable != "false" */
bool is_editable(lxb_dom_element* el)
{
    if (!el) {
        return false;
    }
    if (tag_eq(el, "input") || tag_eq(el, "textarea")) {
        if (tag_eq(el, "input")) {
            size_t alen = 0;
            const lxb_char_t* t = lxb_dom_element_get_attribute(
                el, (const lxb_char_t*)"type", 4, &alen);
            /* non-text input types (button/checkbox/radio/...) are not
             * text-editable */
            if (t && alen > 0 &&
                !(alen == 4 && std::memcmp(t, "text", 4) == 0)) {
                return false;
            }
        }
        return true;
    }
    size_t alen = 0;
    const lxb_char_t* ce = lxb_dom_element_get_attribute(
        el, (const lxb_char_t*)"contenteditable", 15, &alen);
    if (ce && alen > 0) {
        return !(alen == 5 && std::memcmp(ce, "false", 5) == 0);
    }
    return false;
}

/* editable check via the layout node's tag component (O(1), skips the
 * local_name call for every painted node) */
bool is_editable_node(whaleui_layout_node_t* n)
{
    if (!n || !n->el) {
        return false;
    }
    if (n->tag_id == WUI_TAG_INPUT || n->tag_id == WUI_TAG_TEXTAREA) {
        if (n->tag_id == WUI_TAG_INPUT) {
            size_t alen = 0;
            const lxb_char_t* t = lxb_dom_element_get_attribute(
                n->el, (const lxb_char_t*)"type", 4, &alen);
            /* non-text input types (button/checkbox/radio/...) are not
             * text-editable */
            if (t && alen > 0 &&
                !(alen == 4 && std::memcmp(t, "text", 4) == 0)) {
                return false;
            }
        }
        return true;
    }
    size_t alen = 0;
    const lxb_char_t* ce = lxb_dom_element_get_attribute(
        n->el, (const lxb_char_t*)"contenteditable", 15, &alen);
    if (ce && alen > 0) {
        return !(alen == 5 && std::memcmp(ce, "false", 5) == 0);
    }
    return false;
}

/* editable value: input -> value attribute; textarea/contenteditable -> the
 * concatenated text children */
std::string edit_value(lxb_dom_element* el)
{
    if (tag_eq(el, "input")) {
        size_t alen = 0;
        const lxb_char_t* v = lxb_dom_element_get_attribute(
            el, (const lxb_char_t*)"value", 5, &alen);
        return v ? std::string(reinterpret_cast<const char*>(v), alen)
                 : std::string();
    }
    std::string out;
    lxb_dom_node* n = el->node.first_child;
    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_TEXT ||
            n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
            const lexbor_str_t* s = &lxb_dom_interface_text(n)->char_data.data;
            if (s->data) {
                out.append(reinterpret_cast<const char*>(s->data), s->length);
            }
        }
        n = n->next;
    }
    return out;
}

/* write an editable value back into the DOM (input -> value attr; the rest
 * -> one text node replacing all children) */
void edit_set_value(lxb_dom_element* el, const std::string& s)
{
    if (tag_eq(el, "input")) {
        lxb_dom_element_set_attribute(el, (const lxb_char_t*)"value", 5,
                                      reinterpret_cast<const lxb_char_t*>(s.c_str()),
                                      s.size());
        return;
    }
    lxb_dom_node* n = el->node.first_child;
    while (n) {
        lxb_dom_node* nx = n->next;
        lxb_dom_node_remove(n);
        lxb_dom_node_destroy(n);
        n = nx;
    }
    lxb_dom_text* tn = lxb_dom_document_create_text_node(
        el->node.owner_document,
        reinterpret_cast<const lxb_char_t*>(s.c_str()), s.size());
    if (tn) {
        lxb_dom_node_insert_child(&el->node, lxb_dom_interface_node(tn));
    }
}

/* UTF-8: byte offset of the previous/next character boundary */
size_t utf8_prev(const std::string& s, size_t b)
{
    if (b == 0) {
        return 0;
    }
    if (b > s.size()) {
        b = s.size();
    }
    --b;
    while (b > 0 && (static_cast<unsigned char>(s[b]) & 0xC0) == 0x80) {
        --b;
    }
    return b;
}
size_t utf8_next(const std::string& s, size_t b)
{
    if (b >= s.size()) {
        return s.size();
    }
    ++b;
    while (b < s.size() && (static_cast<unsigned char>(s[b]) & 0xC0) == 0x80) {
        ++b;
    }
    return b;
}

} // namespace

/* window -> framebuffer coordinate scale (FSR); defined in the C API area */
static void fb_coords(whaleui_render_t* r, int& x, int& y);

extern "C" int whaleui_render_parse_color(const char* s, unsigned int* out)
{
    if (!s || !out) {
        return -1;
    }
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    if (*s == '#') {
        ++s;
        int ok = 1;
        size_t n = std::strlen(s);
        if (n == 3) { /* #rgb */
            int ok = 1;
            unsigned int r = hex_nib(s[0], &ok);
            unsigned int g = hex_nib(s[1], &ok);
            unsigned int b = hex_nib(s[2], &ok);
            *out = 0xFF000000 | ((r * 0x11) << 16) | ((g * 0x11) << 8) | (b * 0x11);
            return ok ? 0 : -2;
        }
        if (n == 6 || n == 8) {
            unsigned int r = hex_byte(s, &ok);
            unsigned int g = hex_byte(s + 2, &ok);
            unsigned int b = hex_byte(s + 4, &ok);
            unsigned int a = n == 8 ? hex_byte(s + 6, &ok) : 0xFF;
            *out = (a << 24) | (r << 16) | (g << 8) | b;
            return ok ? 0 : -2;
        }
        return -3;
    }
    if (std::strncmp(s, "rgba(", 5) == 0 || std::strncmp(s, "rgb(", 4) == 0) {
        int rr = 0, gg = 0, bb = 0;
        float faf = -1.0f;
        if (std::sscanf(s, "rgba(%d,%d,%d,%f)", &rr, &gg, &bb, &faf) >= 4) {
            unsigned int r = rr < 0 ? 0 : static_cast<unsigned>(rr);
            unsigned int g = gg < 0 ? 0 : static_cast<unsigned>(gg);
            unsigned int b = bb < 0 ? 0 : static_cast<unsigned>(bb);
            /* alpha: 0..1 float, or legacy 0..255 integer */
            unsigned int a = faf <= 1.0f
                                 ? static_cast<unsigned>(faf * 255.0f + 0.5f)
                                 : static_cast<unsigned>(faf);
            if (r > 255) { r = 255; }
            if (g > 255) { g = 255; }
            if (b > 255) { b = 255; }
            if (a > 255) { a = 255; }
            *out = (a << 24) | (r << 16) | (g << 8) | b;
            return 0;
        }
        if (std::sscanf(s, "rgb(%d,%d,%d)", &rr, &gg, &bb) >= 3) {
            unsigned int r = rr < 0 ? 0 : static_cast<unsigned>(rr);
            unsigned int g = gg < 0 ? 0 : static_cast<unsigned>(gg);
            unsigned int b = bb < 0 ? 0 : static_cast<unsigned>(bb);
            if (r > 255) { r = 255; }
            if (g > 255) { g = 255; }
            if (b > 255) { b = 255; }
            *out = (255 << 24) | (r << 16) | (g << 8) | b;
            return 0;
        }
        return -4;
    }
    for (const NamedColor& nc : k_named) {
        if (std::strcmp(s, nc.name) == 0) {
            *out = nc.value;
            return 0;
        }
    }
    return -5;
}

namespace {

/* --- fonts --- */

/* split a font-family value ("Segoe UI, \"MS YaHei\", sans-serif") into a
 * list of family names (quotes and whitespace stripped) */
std::vector<std::string> split_families(const std::string& s)
{
    std::vector<std::string> out;
    const char* p = s.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char* start = p;
        bool quoted = *p == '"' || *p == '\'';
        if (quoted) {
            ++start;
            ++p;
            while (*p && *p != '"' && *p != '\'') {
                ++p;
            }
            out.emplace_back(start, static_cast<size_t>(p - start));
            if (*p) {
                ++p;
            }
            continue;
        }
        while (*p && *p != ',') {
            ++p;
        }
        std::string fam(start, static_cast<size_t>(p - start));
        size_t b = fam.find_first_not_of(" \t");
        size_t e = fam.find_last_not_of(" \t");
        if (b != std::string::npos) {
            out.push_back(fam.substr(b, e - b + 1));
        }
    }
    return out;
}

#ifdef WHALEUI_BUILD_FULL

/* open a font for a family (no fallback chain); "" or generic families pick
 * the default font. style: TTF_STYLE_* bits (BOLD/ITALIC). */
TTF_Font* render_open_font(whaleui_render_t* r, const std::string& family, int size,
                           int style, bool use_cache)
{
    if (size <= 0) {
        size = 16;
    }
    std::string key = family + "|" + std::to_string(size) + "|" +
                      std::to_string(style);
    if (use_cache) {
        for (auto& f : r->fonts) {
            if (f.first == key) {
                return f.second;
            }
        }
    }
    /* find the font file in the registry */
    const unsigned char* data = nullptr;
    size_t len = 0;
    bool generic = family == "sans-serif" || family == "serif" || family == "monospace";
    whaleui_font_registry* reg = whaleui_font_registry_get();
    if (!family.empty() && !generic) {
        for (size_t i = 0; i < reg->count; ++i) {
            if (std::strcmp(reg->fonts[i].family, family.c_str()) == 0) {
                data = reg->fonts[i].data;
                len = reg->fonts[i].len;
                break;
            }
        }
    } else if (reg->count > 0) {
        data = reg->fonts[0].data;
        len = reg->fonts[0].len;
    }
    TTF_Font* font = nullptr;
    if (data && len) {
        SDL_IOStream* io = SDL_IOFromMem(const_cast<unsigned char*>(data), len);
        if (io) {
            font = TTF_OpenFontIO(io, true, static_cast<float>(size));
        }
    }
    if (font && style) {
        TTF_SetFontStyle(font, style);
    }
    if (use_cache && font) {
        r->fonts.emplace_back(key, font);
    }
    return font;
}

/* attach every other registered font as a fallback (same size) so glyphs
 * missing from `font` (CJK, emoji, ...) resolve through the library */
void render_build_fallback(whaleui_render_t* r, TTF_Font* font, int size, int style)
{
    if (!font) {
        return;
    }
    if (size <= 0) {
        size = 16;
    }
    whaleui_font_registry* reg = whaleui_font_registry_get();
    for (size_t i = 0; i < reg->count; ++i) {
        TTF_Font* fb = render_open_font(r, reg->fonts[i].family, size, style, true);
        if (fb && fb != font) {
            TTF_AddFallbackFont(font, fb);
        }
    }
    /* the default font itself is a last-resort fallback */
    if (r->font_default && r->font_default != font) {
        TTF_AddFallbackFont(font, r->font_default);
    }
}

TTF_Font* render_get_font(whaleui_render_t* r, const std::string& family, int size,
                          int style)
{
    if (size <= 0) {
        size = 16;
    }
    std::string key = family + "|" + std::to_string(size) + "|" +
                      std::to_string(style);
    for (auto& f : r->fonts) {
        if (f.first == key) {
            return f.second;
        }
    }
    /* try each family in the CSS list, in order */
    std::vector<std::string> fams = split_families(family);
    if (fams.empty()) {
        fams.push_back("");
    }
    TTF_Font* font = nullptr;
    for (const std::string& fam : fams) {
        font = render_open_font(r, fam, size, style, false);
        if (font) {
            break;
        }
    }
    if (!font) {
        /* nothing matched: use the default font */
        if (style && r->font_default) {
            TTF_SetFontStyle(r->font_default, style);
        }
        font = r->font_default;
    }
    if (font) {
        render_build_fallback(r, font, size, style);
        r->fonts.emplace_back(key, font);
    }
    return font;
}
#else /* !WHALEUI_BUILD_FULL: text rendering needs SDL3_ttf (full only).
         stb_font text lands with the lite/minimal font path. */
TTF_Font* render_get_font(whaleui_render_t*, const std::string&, int, int) { return nullptr; }
#endif

/* --- painting --- */

unsigned int color_of(const WhaleUIComputedStyle& s, const char* prop, unsigned int def)
{
    std::string v = sget(s, prop);
    if (v.empty()) {
        return def;
    }
    unsigned int c = 0;
    if (whaleui_render_parse_color(v.c_str(), &c) == 0) {
        return c;
    }
    return def;
}

/* border shorthand "1px solid #rrggbb" / "2px dashed red": scan tokens for a
 * parseable color instead of requiring a plain color value */
unsigned int border_color_of(const WhaleUIComputedStyle& s, unsigned int def)
{
    std::string v = sget(s, "border");
    if (v.empty()) {
        return def;
    }
    unsigned int c = 0;
    if (whaleui_render_parse_color(v.c_str(), &c) == 0) {
        return c;
    }
    const char* p = v.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        const char* s = p;
        while (*s && *s != ' ' && *s != '\t' && *s != ';') {
            ++s;
        }
        std::string tok(p, static_cast<size_t>(s - p));
        if (whaleui_render_parse_color(tok.c_str(), &c) == 0) {
            return c;
        }
        p = s;
    }
    return def;
}

/* --- text measuring / hit-testing ---
 * Shared by selection and caret placement. Must match what draw_text_at
 * paints (same font, same wrapping). Full build: TTF_Text (fallback chain +
 * wrapping handled natively). Lite/minimal: stb_truetype per-glyph metrics,
 * simpler but functionally equivalent. */

struct TRect { int x, y, w, h; };

#ifdef WHALEUI_BUILD_FULL
TTF_Text* text_obj(whaleui_render_t* r, const std::string& text, int fs,
                   const std::string& family, bool bold)
{
    if (text.empty()) {
        return nullptr;
    }
    TTF_Font* font = render_get_font(r, family, fs,
                                     bold ? kFontBold : 0);
    if (!font) {
        return nullptr;
    }
    if (!r->text_engine) {
        r->text_engine = TTF_CreateSurfaceTextEngine();
    }
    if (!r->text_engine) {
        return nullptr;
    }
    return TTF_CreateText(r->text_engine, font, text.c_str(), text.size());
}
#else
/* stb font table for measuring (mirrors the draw path in draw_text_at) */
struct StbFonts
{
    struct F { stbtt_fontinfo info; float scale; int asc; int line_h; bool ok; };
    std::vector<F> fonts;
    size_t pref;
    int line_h;
    explicit StbFonts(const std::string& family, int fs)
        : pref(static_cast<size_t>(-1)), line_h(fs > 0 ? fs : 16)
    {
        whaleui_font_registry* reg = whaleui_font_registry_get();
        if (!reg) {
            return;
        }
        for (size_t fi = 0; fi < reg->count; ++fi) {
            F f;
            f.ok = false;
            f.scale = 1.0f;
            f.asc = 0;
            f.line_h = line_h;
            const unsigned char* d = reg->fonts[fi].data;
            size_t l = reg->fonts[fi].len;
            if (d && l >= 4) {
                int off = stbtt_GetFontOffsetForIndex(d, 0);
                if (off >= 0 && stbtt_InitFont(&f.info, d, off)) {
                    f.scale = stbtt_ScaleForPixelHeight(&f.info, static_cast<float>(fs));
                    int desc = 0, linegap = 0;
                    stbtt_GetFontVMetrics(&f.info, &f.asc, &desc, &linegap);
                    f.line_h = static_cast<int>((f.asc - desc + linegap) * f.scale + 0.5f);
                    f.ok = true;
                    line_h = f.line_h;
                }
            }
            fonts.push_back(f);
        }
        /* preferred font: first CSS family match, else first usable font */
        std::vector<std::string> fams = split_families(family);
        for (const std::string& fam : fams) {
            if (fam.empty()) {
                continue;
            }
            bool generic = fam == "sans-serif" || fam == "serif" ||
                           fam == "monospace";
            if (generic) {
                continue;
            }
            for (size_t fi = 0; fi < fonts.size(); ++fi) {
                if (fonts[fi].ok && reg->fonts[fi].family &&
                    std::strcmp(reg->fonts[fi].family, fam.c_str()) == 0) {
                    pref = fi;
                    break;
                }
            }
            if (pref != static_cast<size_t>(-1)) {
                break;
            }
        }
        if (pref == static_cast<size_t>(-1)) {
            for (size_t fi = 0; fi < fonts.size(); ++fi) {
                if (fonts[fi].ok) {
                    pref = fi;
                    break;
                }
            }
        }
    }
    size_t pick(size_t start, unsigned int cp) const
    {
        if (start != static_cast<size_t>(-1) && fonts[start].ok &&
            stbtt_FindGlyphIndex(&fonts[start].info, cp) != 0) {
            return start;
        }
        for (size_t fi = 0; fi < fonts.size(); ++fi) {
            if (fi == start || !fonts[fi].ok) {
                continue;
            }
            if (stbtt_FindGlyphIndex(&fonts[fi].info, cp) != 0) {
                return fi;
            }
        }
        return static_cast<size_t>(-1);
    }
    /* decode UTF-8 into lines of codepoints + byte starts; measures widths */
    struct TLine { std::vector<unsigned int> cps; std::vector<size_t> starts; int w; };
    std::vector<TLine> lines(const std::string& text) const
    {
        std::vector<TLine> out;
        out.push_back(TLine());
        const unsigned char* sb = reinterpret_cast<const unsigned char*>(text.c_str());
        size_t i = 0;
        while (i < text.size()) {
            size_t start = i;
            unsigned char c = sb[i];
            unsigned int cp = c;
            int len = 1;
            if (c >= 0x80) {
                if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
                    cp = ((c & 0x1F) << 6) | (sb[i + 1] & 0x3F);
                    len = 2;
                } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
                    cp = ((c & 0x0F) << 12) | ((sb[i + 1] & 0x3F) << 6) |
                         (sb[i + 2] & 0x3F);
                    len = 3;
                } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
                    cp = ((c & 0x07) << 18) | ((sb[i + 1] & 0x3F) << 12) |
                         ((sb[i + 2] & 0x3F) << 6) | (sb[i + 3] & 0x3F);
                    len = 4;
                } else {
                    cp = 0;
                }
            }
            i += len;
            if (cp == '\n') {
                out.push_back(TLine());
                continue;
            }
            if (cp < 0x20 || cp == 0x7F) {
                continue;
            }
            out.back().cps.push_back(cp);
            out.back().starts.push_back(start);
        }
        for (size_t li = 0; li < out.size(); ++li) {
            TLine& ln = out[li];
            for (size_t gi = 0; gi < ln.cps.size(); ++gi) {
                size_t fi = pick(pref, ln.cps[gi]);
                if (fi == static_cast<size_t>(-1)) {
                    continue;
                }
                int adv = 0, lsb = 0;
                stbtt_GetCodepointHMetrics(&fonts[fi].info, ln.cps[gi], &adv, &lsb);
                ln.w += static_cast<int>(adv * fonts[fi].scale + 0.5f);
            }
        }
        return out;
    }
};
#endif

/* total text size in px (0 when empty) */
void text_size(whaleui_render_t* r, const std::string& text, int fs,
               const std::string& family, bool bold, int* tw, int* th)
{
    *tw = *th = 0;
    if (text.empty()) {
        return;
    }
#ifdef WHALEUI_BUILD_FULL
    TTF_Text* t = text_obj(r, text, fs, family, bold);
    if (t) {
        TTF_GetTextSize(t, tw, th);
        TTF_DestroyText(t);
    }
#else
    StbFonts stb(family, fs);
    std::vector<StbFonts::TLine> ls = stb.lines(text);
    *th = static_cast<int>(ls.size()) * stb.line_h;
    for (size_t i = 0; i < ls.size(); ++i) {
        if (ls[i].w > *tw) {
            *tw = ls[i].w;
        }
    }
#endif
}

/* per-line height in px for the caret/highlight rectangles */
int text_line_h(whaleui_render_t* r, int fs, const std::string& family, bool bold)
{
#ifdef WHALEUI_BUILD_FULL
    TTF_Font* font = render_get_font(r, family, fs,
                                     bold ? kFontBold : 0);
    if (font) {
        int h = TTF_GetFontHeight(font);
        if (h > 0) {
            return h;
        }
    }
    return fs > 0 ? fs : 16;
#else
    StbFonts stb(family, fs > 0 ? fs : 16);
    return stb.line_h;
#endif
}

/* byte offset of the text under (px,py), relative to the text top-left */
size_t byte_at_text(whaleui_render_t* r, const std::string& text, int fs,
                    const std::string& family, bool bold, int px, int py)
{
    if (text.empty()) {
        return 0;
    }
#ifdef WHALEUI_BUILD_FULL
    TTF_Text* t = text_obj(r, text, fs, family, bold);
    if (!t) {
        return 0;
    }
    int tw = 0, th = 0;
    TTF_GetTextSize(t, &tw, &th);
    if (px < 0) {
        px = 0;
    }
    if (py < 0) {
        py = 0;
    }
    /* at/past the right edge of the text: the caret goes to the very end,
     * so the last character can be selected */
    if (px >= tw) {
        TTF_DestroyText(t);
        return text.size();
    }
    if (py >= th) {
        py = th > 0 ? th - 1 : 0;
    }
    TTF_SubString sub;
    size_t off = text.size();
    if (TTF_GetTextSubStringForPoint(t, static_cast<float>(px),
                                     static_cast<float>(py), &sub)) {
        off = static_cast<size_t>(sub.offset);
        /* clicking the right half of a character (or beyond it) places the
         * caret AFTER that character instead of before it */
        if (px >= sub.rect.x + sub.rect.w / 2) {
            off = static_cast<size_t>(sub.offset + sub.length);
        }
    }
    TTF_DestroyText(t);
    return off;
#else
    StbFonts stb(family, fs);
    std::vector<StbFonts::TLine> ls = stb.lines(text);
    if (ls.empty()) {
        return 0;
    }
    int lh = stb.line_h;
    int line = lh > 0 ? py / lh : 0;
    if (line < 0) {
        line = 0;
    }
    if (line >= static_cast<int>(ls.size())) {
        line = static_cast<int>(ls.size()) - 1;
    }
    const StbFonts::TLine& ln = ls[static_cast<size_t>(line)];
    int xacc = 0;
    for (size_t gi = 0; gi < ln.cps.size(); ++gi) {
        size_t fi = stb.pick(stb.pref, ln.cps[gi]);
        int adv = 0, lsb = 0;
        if (fi != static_cast<size_t>(-1)) {
            stbtt_GetCodepointHMetrics(&stb.fonts[fi].info, ln.cps[gi], &adv, &lsb);
        }
        int w = static_cast<int>(adv * (fi == static_cast<size_t>(-1) ? 0.5f : stb.fonts[fi].scale) + 0.5f);
        if (px < xacc + w / 2) {
            return ln.starts[gi];
        }
        xacc += w;
    }
    return text.size();
#endif
}

/* highlight rectangles for the byte range [a,b), relative to text top-left */
std::vector<TRect> sel_rects(whaleui_render_t* r, const std::string& text,
                             int fs, const std::string& family, bool bold,
                             size_t a, size_t b)
{
    std::vector<TRect> out;
    if (a > text.size()) {
        a = text.size();
    }
    if (b > text.size()) {
        b = text.size();
    }
    if (a > b) {
        std::swap(a, b);
    }
    if (a == b || text.empty()) {
        return out;
    }
#ifdef WHALEUI_BUILD_FULL
    TTF_Text* t = text_obj(r, text, fs, family, bold);
    if (!t) {
        return out;
    }
    int count = 0;
    TTF_SubString** subs = TTF_GetTextSubStringsForRange(
        t, static_cast<int>(a), static_cast<int>(b - a), &count);
    if (subs) {
        for (int i = 0; i < count; ++i) {
            TRect rc;
            rc.x = subs[i]->rect.x;
            rc.y = subs[i]->rect.y;
            rc.w = subs[i]->rect.w;
            rc.h = subs[i]->rect.h;
            out.push_back(rc);
        }
        /* single allocation; the whole array is freed once (see SDL_ttf) */
        SDL_free(subs);
    }
    TTF_DestroyText(t);
#else
    StbFonts stb(family, fs);
    std::vector<StbFonts::TLine> ls = stb.lines(text);
    int lh = stb.line_h;
    for (size_t li = 0; li < ls.size(); ++li) {
        const StbFonts::TLine& ln = ls[li];
        int x0 = -1, x1 = -1;
        int xacc = 0;
        for (size_t gi = 0; gi < ln.cps.size(); ++gi) {
            size_t fs2 = stb.pick(stb.pref, ln.cps[gi]);
            int adv = 0, lsb2 = 0;
            if (fs2 != static_cast<size_t>(-1)) {
                stbtt_GetCodepointHMetrics(&stb.fonts[fs2].info, ln.cps[gi], &adv, &lsb2);
            }
            int w = static_cast<int>(adv * (fs2 == static_cast<size_t>(-1) ? 0.5f : stb.fonts[fs2].scale) + 0.5f);
            if (x0 < 0 && ln.starts[gi] >= a) {
                x0 = xacc;
            }
            if (ln.starts[gi] >= b && x1 < 0) {
                x1 = xacc;
                break;
            }
            xacc += w;
        }
        if (x0 < 0) {
            continue; /* line ends before a */
        }
        if (x1 < 0) {
            x1 = xacc; /* range extends past this line's end */
        }
        if (x1 > x0) {
            TRect rc = {x0, static_cast<int>(li) * lh, x1 - x0, lh};
            out.push_back(rc);
        }
        if (x1 >= xacc) {
            continue; /* keep scanning later lines (range continues) */
        }
        break;
    }
#endif
    return out;
}

/* caret (cursor) position for byte offset `off`, relative to text top-left */
void caret_pos(whaleui_render_t* r, const std::string& text, int fs,
               const std::string& family, bool bold, size_t off,
               int* cx, int* cy, int* ch)
{
    int lh = text_line_h(r, fs, family, bold);
    if (off == 0) {
        *cx = 0;
        *cy = 0;
        *ch = lh;
        return;
    }
    std::vector<TRect> rs = sel_rects(r, text, fs, family, bold, 0, off);
    if (rs.empty()) {
        *cx = 0;
        *cy = 0;
        *ch = lh;
        return;
    }
    const TRect& last = rs.back();
    *cx = last.x + last.w;
    *cy = last.y;
    *ch = last.h;
}

/* defined below (editing helpers); needed by the paint path above */
void text_origin(whaleui_render_t* r, whaleui_layout_node_t* n,
                 const std::string& text, int fs, const std::string& family,
                 bool bold, int* tx, int* ty);
struct whaleui_layout_node;
whaleui_layout_node_t* editable_geo(whaleui_layout_node_t* n);

/* byte length of the UTF-8 sequence starting with c (ASCII = 1) */
size_t utf8_char_len(unsigned char c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

/* ASCII-only text-transform; multi-byte UTF-8 passes through untouched and
 * never changes the byte length, so selection/caret offsets stay valid */
void apply_text_transform(std::string& s, const std::string& t)
{
    if (t == "uppercase") {
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80 && c >= 'a' && c <= 'z') {
                s[i] = static_cast<char>(c - 32);
            }
        }
    } else if (t == "lowercase") {
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80 && c >= 'A' && c <= 'Z') {
                s[i] = static_cast<char>(c + 32);
            }
        }
    } else if (t == "capitalize") {
        bool word_start = true;
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c >= 0x80) {
                i += utf8_char_len(c) - 1;
                word_start = false;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '-') {
                word_start = true;
            } else if (word_start) {
                if (c >= 'a' && c <= 'z') {
                    s[i] = static_cast<char>(c - 32);
                }
                word_start = false;
            }
        }
    }
}

/* render one text string inside the box (bx,by,bw,bh). align: 0=left,
 * 1=center, 2=right. Shared by text runs and <select> controls.
 * style: TTF_STYLE_* bits (bold/italic).
 * ckey: element to cache the TTF_Text against (NULL = no caching).
 * lsp: letter-spacing in px (>0 paints glyph by glyph with that gap;
 * TTF_Text has no spacing control, so this is a per-glyph path).
 * wrap: wrap long text to bw (multi-line; height grows with line count). */
void draw_text_at(whaleui_render_t* r, const std::string& text,
                  int bx, int by, int bw, int bh,
                  int fs, const std::string& family, unsigned int color,
                  int style, int align, lxb_dom_element* ckey,
                  const Clip* clip, int lsp = 0, bool wrap = false)
{
#ifdef WHALEUI_BUILD_FULL
    if (fs <= 0) {
        fs = 16;
    }
    TTF_Font* font = render_get_font(r, family, fs, style);
    if (!font) {
        return;
    }
    /* TTF_Text honors the fallback chain (CJK/emoji glyphs) */
    TTF_TextEngine* engine = r->text_engine;
    if (!engine) {
        engine = TTF_CreateSurfaceTextEngine();
        r->text_engine = engine;
    }
    if (!engine) {
        return;
    }
    if (lsp > 0 && !text.empty()) {
        /* letter-spacing: split into UTF-8 chars, measure once, then paint
         * each glyph shifted by lsp. Short text in practice (headings,
         * nav labels), so no caching here. */
        std::vector<std::string> chs;
        std::vector<int> cws;
        int total = 0, th = 0;
        for (size_t i = 0; i < text.size();) {
            size_t n = utf8_char_len(static_cast<unsigned char>(text[i]));
            if (i + n > text.size()) {
                n = 1;
            }
            std::string ch = text.substr(i, n);
            i += n;
            TTF_Text* ct = TTF_CreateText(engine, font, ch.c_str(), ch.size());
            int w = 0, h = 0;
            if (ct) {
                TTF_GetTextSize(ct, &w, &h);
                TTF_DestroyText(ct);
            }
            if (h > th) {
                th = h;
            }
            chs.push_back(ch);
            cws.push_back(w);
            total += w;
        }
        int tw = total + lsp * (static_cast<int>(chs.size()) - 1);
        int tx = bx;
        if (align == 1) {
            tx = bx + (bw - tw) / 2;
        } else if (align == 2) {
            tx = bx + bw - tw;
        }
        int ty = by + (bh - th) / 2;
        for (size_t i = 0; i < chs.size(); ++i) {
            if (cws[i] <= 0) {
                continue;
            }
            TTF_Text* ct = TTF_CreateText(engine, font, chs[i].c_str(), chs[i].size());
            if (!ct) {
                continue;
            }
            SDL_Surface* cs = SDL_CreateSurface(cws[i], th, SDL_PIXELFORMAT_RGBA8888);
            if (cs) {
                SDL_FillSurfaceRect(cs, nullptr, 0);
                TTF_DrawSurfaceText(ct, 0, 0, cs);
                if (g_gpu) {
                    blend_surface(r->text_layer, r->fb_w, r->fb_h, cs, tx, ty,
                                  clip, &color);
                } else {
                    blend_surface(r->pixels, r->fb_w, r->fb_h, cs, tx, ty,
                                  clip, &color);
                }
                SDL_DestroySurface(cs);
            }
            TTF_DestroyText(ct);
            tx += cws[i] + lsp;
        }
        return;
    }
    TTF_Text* t = nullptr;
    bool owned = true;
    std::string cache_key;
    if (ckey) {
        /* reuse the cached text object; recreate when the style changed
         * (key) or the content changed (TTF_Text is immutable). Wrapped
         * runs also key on the wrap width so they don't share objects. */
        char key[96];
        std::snprintf(key, sizeof(key), "%p|%d|%d|%s|%d",
                      static_cast<void*>(ckey), fs, style,
                      family.c_str(), wrap ? bw : -1);
        cache_key = key;
        whaleui_render_t::TextCacheEntry& e = r->text_cache[cache_key];
        if (!e.t) {
            e.t = TTF_CreateText(engine, font, text.c_str(), text.size());
            e.text = text;
        } else if (e.text != text) {
            TTF_DestroyText(e.t);
            e.t = TTF_CreateText(engine, font, text.c_str(), text.size());
            e.text = text;
            /* the rasterized bitmap is stale (same size may hold other
             * glyphs): drop it so the next frame re-rasterizes */
            if (e.surf) {
                SDL_DestroySurface(e.surf);
                e.surf = nullptr;
            }
        }
        t = e.t;
        owned = false;
    } else {
        t = TTF_CreateText(engine, font, text.c_str(), text.size());
    }
    if (!t) {
        return;
    }
    if (wrap && bw > 0) {
        /* text runs wrap to their content width; the layout pass estimates
         * the resulting height, the real size comes from TTF_Text here */
        TTF_SetTextWrapWidth(t, bw);
    }
    /* NOTE: TTF_SetTextColor (and Float) on the 3.2.2 prebuilt break
     * TTF_DrawSurfaceText (draws nothing, no error). Instead we render with
     * the default color and tint during blending. */
    int tw = 0, th = 0;
    TTF_GetTextSize(t, &tw, &th);
    if (tw > 0 && th > 0) {
        int tx = bx;
        if (align == 1) {
            tx = bx + (bw - tw) / 2;
        } else if (align == 2) {
            tx = bx + bw - tw;
        }
        /* line-box centering, shared by every text path; the <select>
         * value nudges itself up in paint_select_value instead, so this
         * function has no global side effects on other text. Wrapped text
         * taller than the box stays top-aligned (no negative centering). */
        int ty = th <= bh ? by + (bh - th) / 2 : by;
        SDL_Surface* surf = nullptr;
        if (!cache_key.empty()) {
            /* reuse the rasterized surface; recreate on size change */
            whaleui_render_t::TextCacheEntry& e =
                r->text_cache[cache_key];
            surf = e.surf;
            if (!surf || surf->w != tw || surf->h != th) {
                if (surf) {
                    SDL_DestroySurface(surf);
                }
                surf = SDL_CreateSurface(tw, th, SDL_PIXELFORMAT_RGBA8888);
                if (surf) {
                    SDL_FillSurfaceRect(surf, nullptr, 0);
                    TTF_DrawSurfaceText(t, 0, 0, surf);
                }
                e.surf = surf;
                e.ax = -1; /* atlas slot is stale after a re-rasterize */
            }
        } else {
            surf = SDL_CreateSurface(tw, th, SDL_PIXELFORMAT_RGBA8888);
            if (surf) {
                SDL_FillSurfaceRect(surf, nullptr, 0);
                TTF_DrawSurfaceText(t, 0, 0, surf);
            }
        }
        if (surf) {
            if (g_gpu) {
                /* text goes to the CPU layer (geometries stay on the GPU
                 * draw list); the layer is composited into the target by a
                 * compute pass after the geometry render pass */
                blend_surface(r->text_layer, r->fb_w, r->fb_h, surf, tx, ty,
                              clip, &color);
            } else {
                blend_surface(r->pixels, r->fb_w, r->fb_h, surf, tx, ty,
                              clip, &color);
            }
        }
        if (cache_key.empty() && surf) {
            SDL_DestroySurface(surf);
        }
    }
    if (owned) {
        TTF_DestroyText(t);
    }
#else /* !WHALEUI_BUILD_FULL: stb_truetype text (lite/minimal) */
    (void)ckey;
    if (fs <= 0) {
        fs = 16;
    }
    whaleui_font_registry* reg = whaleui_font_registry_get();
    if (!reg || reg->count == 0) {
        return;
    }
    /* init every registered font once; each gets its own pixel-height scale
     * so fallback glyphs (CJK/emoji) keep the same visual size */
    struct F { stbtt_fontinfo info; float scale; int asc; int line_h; bool ok; };
    std::vector<F> fonts;
    for (size_t fi = 0; fi < reg->count; ++fi) {
        F f;
        f.ok = false;
        f.scale = 1.0f;
        f.asc = 0;
        f.line_h = fs;
        const unsigned char* d = reg->fonts[fi].data;
        size_t l = reg->fonts[fi].len;
        if (!d || l < 4) {
            fonts.push_back(f);
            continue;
        }
        int off = stbtt_GetFontOffsetForIndex(d, 0);
        if (off < 0 || !stbtt_InitFont(&f.info, d, off)) {
            fonts.push_back(f);
            continue;
        }
        f.scale = stbtt_ScaleForPixelHeight(&f.info, static_cast<float>(fs));
        int desc = 0, linegap = 0;
        stbtt_GetFontVMetrics(&f.info, &f.asc, &desc, &linegap);
        f.line_h = static_cast<int>((f.asc - desc + linegap) * f.scale + 0.5f);
        f.ok = true;
        fonts.push_back(f);
    }
    /* preferred font: first CSS family match, else the first font that
     * initialized (mirrors the full build's family matching) */
    size_t pref = static_cast<size_t>(-1);
    std::vector<std::string> fams = split_families(family);
    for (const std::string& fam : fams) {
        if (fam.empty()) {
            continue;
        }
        bool generic = fam == "sans-serif" || fam == "serif" ||
                       fam == "monospace";
        if (generic) {
            continue;
        }
        for (size_t fi = 0; fi < fonts.size(); ++fi) {
            if (fonts[fi].ok && reg->fonts[fi].family &&
                std::strcmp(reg->fonts[fi].family, fam.c_str()) == 0) {
                pref = fi;
                break;
            }
        }
        if (pref != static_cast<size_t>(-1)) {
            break;
        }
    }
    if (pref == static_cast<size_t>(-1)) {
        for (size_t fi = 0; fi < fonts.size(); ++fi) {
            if (fonts[fi].ok) {
                pref = fi;
                break;
            }
        }
    }
    if (pref == static_cast<size_t>(-1)) {
        return;
    }
    /* pick a font that actually has `cp`; fall back across the registry so
     * CJK/emoji glyphs resolve even when the preferred font lacks them */
    auto pick_font = [&](size_t start, unsigned int cp) -> size_t {
        if (stbtt_FindGlyphIndex(&fonts[start].info, cp) != 0) {
            return start;
        }
        for (size_t fi = 0; fi < fonts.size(); ++fi) {
            if (fi == start || !fonts[fi].ok) {
                continue;
            }
            if (stbtt_FindGlyphIndex(&fonts[fi].info, cp) != 0) {
                return fi;
            }
        }
        return static_cast<size_t>(-1);
    };
    /* decode UTF-8 into per-line codepoints; control chars are skipped
     * entirely (a stray byte or control code in a long string must not
     * draw tofu or crash), '\n' starts a new line, '\r' is dropped so
     * CRLF counts once */
    struct TLine { std::vector<unsigned int> cps; int w; };
    std::vector<TLine> lines;
    lines.push_back(TLine());
    const unsigned char* sb = reinterpret_cast<const unsigned char*>(text.c_str());
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = sb[i];
        unsigned int cp = c;
        int len = 1;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
                cp = ((c & 0x1F) << 6) | (sb[i + 1] & 0x3F);
                len = 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
                cp = ((c & 0x0F) << 12) | ((sb[i + 1] & 0x3F) << 6) |
                     (sb[i + 2] & 0x3F);
                len = 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
                cp = ((c & 0x07) << 18) | ((sb[i + 1] & 0x3F) << 12) |
                     ((sb[i + 2] & 0x3F) << 6) | (sb[i + 3] & 0x3F);
                len = 4;
            } else {
                cp = 0; /* malformed byte: skip it entirely */
            }
        }
        i += len;
        if (cp == '\n') {
            lines.push_back(TLine());
            continue;
        }
        if (cp < 0x20 || cp == 0x7F) {
            continue; /* C0/DEL control chars: no advance, no glyph */
        }
        lines.back().cps.push_back(cp);
    }
    /* measure each line: per-glyph font fallback, advances accumulate */
    const int line_h = fonts[pref].line_h;
    for (size_t li = 0; li < lines.size(); ++li) {
        TLine& ln = lines[li];
        for (size_t gi = 0; gi < ln.cps.size(); ++gi) {
            size_t fi = pick_font(pref, ln.cps[gi]);
            if (fi == static_cast<size_t>(-1)) {
                continue; /* no registered font has this glyph */
            }
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&fonts[fi].info, ln.cps[gi], &adv, &lsb);
            ln.w += static_cast<int>(adv * fonts[fi].scale + 0.5f);
        }
    }
    int total_w = 0, total_h = static_cast<int>(lines.size()) * line_h;
    for (size_t li = 0; li < lines.size(); ++li) {
        if (lines[li].w > total_w) {
            total_w = lines[li].w;
        }
    }
    if (total_w <= 0 || total_h <= 0) {
        return;
    }
    /* rasterize into an alpha-only buffer (same layout as the framebuffer).
     * bold is a fake bold: the glyph bitmap is blitted once more 1px to the
     * right (alpha max), since stb_truetype has no style synthesis. */
    std::vector<unsigned int> buf(static_cast<size_t>(total_w) * total_h, 0);
    const float baseline_off = fonts[pref].asc * fonts[pref].scale;
    for (size_t li = 0; li < lines.size(); ++li) {
        const TLine& ln = lines[li];
        const int baseline = static_cast<int>(li * line_h + baseline_off + 0.5f);
        float px = 0;
        for (size_t gi = 0; gi < ln.cps.size(); ++gi) {
            unsigned int cp = ln.cps[gi];
            size_t fi = pick_font(pref, cp);
            if (fi == static_cast<size_t>(-1)) {
                continue;
            }
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&fonts[fi].info, cp, &adv, &lsb);
            int gw = 0, gh = 0, xoff = 0, yoff = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(
                &fonts[fi].info, fonts[fi].scale, fonts[fi].scale,
                cp, &gw, &gh, &xoff, &yoff);
            if (bmp && gw > 0 && gh > 0) {
                const int dy = baseline + yoff;
                for (int pass = 0;
                     pass < ((style & kFontBold) ? 2 : 1); ++pass) {
                    const int dx = static_cast<int>(px) + xoff + pass;
                    for (int yy = 0; yy < gh; ++yy) {
                        const int ty2 = dy + yy;
                        if (ty2 < 0 || ty2 >= total_h) {
                            continue;
                        }
                        for (int xx = 0; xx < gw; ++xx) {
                            const int tx2 = dx + xx;
                            if (tx2 < 0 || tx2 >= total_w) {
                                continue;
                            }
                            const unsigned int a = bmp[yy * gw + xx];
                            if (a == 0) {
                                continue;
                            }
                            unsigned int& dst = buf[ty2 * total_w + tx2];
                            if (a > dst) {
                                dst = a;
                            }
                        }
                    }
                }
                stbtt_FreeBitmap(bmp, nullptr);
            }
            px += adv * fonts[fi].scale;
        }
    }
    /* place + blend (single pass, tinted by `color`) */
    int tx = bx;
    if (align == 1) {
        tx = bx + (bw - total_w) / 2;
    } else if (align == 2) {
        tx = bx + bw - total_w;
    }
    int ty = by + (bh - fonts[pref].line_h) / 2;
    const unsigned int tr = (color >> 16) & 0xFF;
    const unsigned int tg = (color >> 8) & 0xFF;
    const unsigned int tb = color & 0xFF;
    const unsigned int ta = (color >> 24) & 0xFF;
    for (int yy = 0; yy < total_h; ++yy) {
        const int fy = ty + yy;
        if (fy < 0 || fy >= r->height) {
            continue;
        }
        if (clip && (fy < clip->y || fy >= clip->y + clip->h)) {
            continue;
        }
        for (int xx = 0; xx < total_w; ++xx) {
            const unsigned int a = buf[yy * total_w + xx];
            if (a == 0) {
                continue;
            }
            const int fx = tx + xx;
            if (fx < 0 || fx >= r->width) {
                continue;
            }
            if (clip && (fx < clip->x || fx >= clip->x + clip->w)) {
                continue;
            }
            const unsigned int fa = (a * ta) / 255;
            if (fa == 0) {
                continue;
            }
            unsigned int& d = r->pixels[fy * r->width + fx];
            const unsigned int dr = (d >> 16) & 0xFF;
            const unsigned int dg = (d >> 8) & 0xFF;
            const unsigned int db = d & 0xFF;
            const unsigned int nr = (tr * fa + dr * (255 - fa)) / 255;
            const unsigned int ng = (tg * fa + dg * (255 - fa)) / 255;
            const unsigned int nb = (tb * fa + db * (255 - fa)) / 255;
            d = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
        }
    }
#endif /* WHALEUI_BUILD_FULL */
}

/* --- text selection + editing overlay --- */

/* selection byte range [a,b) for el's text; false when el is outside the
 * selection (or the range is empty). Cross-element selections highlight the
 * endpoint elements partially and everything in between fully.
 * seq/sel_lo/sel_hi are pre-order layout-tree sequence numbers computed once
 * per frame (O(1) membership test instead of a per-run document walk). */
bool sel_range_for(whaleui_render_t* r, lxb_dom_element* el, size_t len,
                   size_t* a, size_t* b, int seq, int sel_lo, int sel_hi)
{    lxb_dom_element* sa = r->sel_anchor_el;
    lxb_dom_element* sf = r->sel_focus_el;
    if (!sa || !sf || !el) {
        return false;
    }
    if (sa == sf) {
        /* collapsed (caret / plain click): nothing is highlighted anywhere */
        if (r->sel_anchor == r->sel_focus) {
            return false;
        }
        if (el == sa) {
            size_t lo = static_cast<size_t>(r->sel_anchor);
            size_t hi = static_cast<size_t>(r->sel_focus);
            if (lo > hi) {
                std::swap(lo, hi);
            }
            *a = lo;
            *b = hi;
            return *a < *b && *a < len;
        }
        return false; /* same-element selection touches only that element */
    }
    /* cross-element: endpoint elements partially, everything between fully */
    if (el == sa) {
        *a = static_cast<size_t>(r->sel_anchor);
        *b = len;
        return *a < len;
    }
    if (el == sf) {
        *a = 0;
        *b = static_cast<size_t>(r->sel_focus);
        return *b > 0;
    }
    if (sel_lo >= 0 && sel_hi >= 0 && seq > sel_lo && seq < sel_hi) {
        *a = 0;
        *b = len;
        return len > 0;
    }
    return false;
}

/* paint-time scroll offset of a scrollable box (defined below) */
int scroll_delta(whaleui_render_t* r, whaleui_layout_node_t* n);

/* partial-repaint subtree culler (defined with paint_node below) */
bool paint_cull(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
                int off_y, const Clip* clip);

/* pre-order sequence numbers of the anchor/focus layout nodes, computed by
 * walking the tree EXACTLY like paint_node does (same visibility skip, same
 * clipped-subtree skip, same scroll offsets), so the sequence assigned
 * during painting matches. -1 when same-element or collapsed. */
void sel_seq(whaleui_render_t* r, int* lo, int* hi, const Clip* clip)
{
    *lo = *hi = -1;
    lxb_dom_element* sa = r->sel_anchor_el;
    lxb_dom_element* sf = r->sel_focus_el;
    if (!sa || !sf || sa == sf || !r->tree) {
        return;
    }
    int idx = 0;
    std::function<void(whaleui_layout_node_t*, int, int, bool)> walk =
        [&](whaleui_layout_node_t* n, int off_x, int off_y, bool tc) {
            if (!n->visible) {
                return;
            }
            const int my = idx++;
            if (n->el == sa && *lo < 0) {
                *lo = my;
            }
            if (n->el == sf && *hi < 0) {
                *hi = my;
            }
            /* same transform/fixed rule as paint_node, so the sequence
             * numbers stay aligned with what actually gets painted */
            if (!tc && n->el && !n->is_text) {
                std::string tv = sget(n->style, "transform");
                if (!tv.empty() && tv != "none") {
                    tc = true;
                } else if (sget(n->style, "position") == "fixed") {
                    tc = true;
                }
            }
            /* a selection endpoint outside the repaint region must still
             * receive its sequence number (paint_node does the same), so
             * the in-viewport middle of the selection keeps highlighting.
             * Text runs skip the cull exactly like paint_node. */
            if (!tc && !n->is_text &&
                !(n->el && (n->el == sa || n->el == sf)) &&
                paint_cull(r, n, off_x, off_y, clip)) {
                return;
            }
            if (n->is_text) {
                return;
            }
            /* clipped containers fully off-screen are skipped by paint too */
            std::string ov = sget(n->style, "overflow");
            if (ov == "hidden" || ov == "auto" || ov == "scroll") {
                int y0 = n->border.y + off_y;
                if (y0 + n->border.h <= 0 || y0 >= r->fb_h) {
                    return;
                }
            }
            int child_off = off_y + scroll_delta(r, n);
            for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
                walk(c, off_x, child_off, tc);
            }
        };
    walk(r->tree->root, 0, 0, false);
    if (*lo > *hi) {
        std::swap(*lo, *hi);
    }
}

unsigned int accent_hl(whaleui_render_t* r, unsigned int alpha)
{
    unsigned int hl = (alpha << 24);
    auto it = r->theme_vars.find("--accent");
    if (it != r->theme_vars.end()) {
        unsigned int c = 0;
        if (whaleui_render_parse_color(it->second.c_str(), &c) == 0) {
            hl = (alpha << 24) | (c & 0x00FFFFFF);
        }
    }
    return hl;
}

/* paint-time scroll offset of a scrollable box: the layout tree already
 * shifted the content up by the scroll_y baked in at build time; the paint
 * path adds (current - baked) MORE shift. Since a larger scroll_y moves
 * content UP, the extra offset is negative: baked - current. */
int scroll_delta(whaleui_render_t* r, whaleui_layout_node_t* n)
{
    if (n->scroll_max > 0 && n->el) {
        auto it = r->scrolls.find(n->el);
        if (it != r->scrolls.end()) {
            return n->scroll_y - it->second;
        }
    }
    return 0;
}

/* grow glyph-level highlight rects up to the line box so the selection wash
 * covers ascenders/descenders with a bit of padding (TTF substring rects
 * only bound the glyphs) */
void expand_hl_rects(std::vector<TRect>& rects, int lh)
{
    for (size_t i = 0; i < rects.size(); ++i) {
        TRect& rc = rects[i];
        if (rc.h < lh) {
            int d = lh - rc.h;
            rc.y -= d / 2 + 2;
            rc.h = lh;
        } else {
            rc.y -= 2;
            rc.h += 4;
        }
    }
}

/* highlight the selected part of a text run (drawn before the glyphs) */
/* text-selection highlight color: ::selection background if set, else the
 * theme accent */
unsigned int sel_hl_color(whaleui_render_t* r, whaleui_layout_node_t* n,
                          unsigned int alpha);

void paint_text_selection(whaleui_render_t* r, whaleui_layout_node_t* n,
                          int fs, const std::string& family, bool bold,
                          int off_x, int off_y, int seq, int sel_lo,
                          int sel_hi, const Clip* clip)
{
        size_t a = 0, b = 0;
    int tw = 0, th = 0;
    text_size(r, n->text, fs, family, bold, &tw, &th);
    if (th <= 0) {
        return;
    }
    int tx = 0, ty = 0;
    text_origin(r, n, n->text, fs, family, bold, &tx, &ty);
    tx += off_x;
    ty += off_y;
    std::vector<TRect> rects = sel_rects(r, n->text, fs, family, bold, a, b);
    expand_hl_rects(rects, text_line_h(r, fs, family, bold));
    unsigned int hl = sel_hl_color(r, n, 0x3C);
    for (size_t i = 0; i < rects.size(); ++i) {
        fill_rect(r->pixels, r->fb_w, r->fb_h,
                  tx + rects[i].x, ty + rects[i].y,
                  rects[i].w, rects[i].h, hl, clip);
    }
}

/* text-selection highlight color: ::selection background if set, else the
 * theme accent */
unsigned int sel_hl_color(whaleui_render_t* r, whaleui_layout_node_t* n,
                          unsigned int alpha)
{
    std::string v = sget(n->style, "selection-bg");
    if (!v.empty()) {
        unsigned int c = 0;
        if (whaleui_render_parse_color(v.c_str(), &c) == 0) {
            return (alpha << 24) | (c & 0x00FFFFFF);
        }
    }
    return accent_hl(r, alpha);
}

/* blinking caret at byte offset `off` (text-relative coordinates tx,ty) */
void paint_caret(whaleui_render_t* r, int tx, int ty, const std::string& text,
                 int fs, const std::string& family, bool bold,
                 size_t off, const Clip* clip)
{
    if ((SDL_GetTicks() / 500) & 1) {
        return; /* blink: off half the time */
    }
    int cx = 0, cy = 0, ch = 16;
    caret_pos(r, text, fs, family, bold, off, &cx, &cy, &ch);
    fill_rect(r->pixels, r->fb_w, r->fb_h, tx + cx, ty + cy, 1, ch,
              accent_hl(r, 0xFF), clip);
}

/* move the IME text-input area to the caret so the candidate window
 * follows the cursor (SDL_SetTextInputArea; fb coords scaled to window) */
void update_ime_area(whaleui_render_t* r, const std::string& val, int fs,
                     const std::string& family, bool bold, size_t caret,
                     int tx, int ty)
{
    if (!r->edit_el || !r->window) {
        return;
    }
    int cxx = 0, cyy = 0, chh = 16;
    caret_pos(r, val, fs, family, bold, caret, &cxx, &cyy, &chh);
    int wx = tx + cxx;
    int wy = ty + cyy;
    if (r->fb_w != r->width && r->width > 0) {
        wx = wx * r->width / r->fb_w;
    }
    if (r->fb_h != r->height && r->height > 0) {
        wy = wy * r->height / r->fb_h;
    }
    SDL_Rect rect = {wx, wy, 2, chh};
    SDL_SetTextInputArea(r->window, &rect, 0);
}

/* checkbox/radio native control (no field chrome; 16px box from layout) */
void paint_checkbox(whaleui_render_t* r, whaleui_layout_node_t* n,
                    int off_x, int off_y, const Clip* clip)
{
    lxb_dom_element* el = n->el;
    if (!el) {
        return;
    }
    size_t alen = 0;
    const lxb_char_t* t = lxb_dom_element_get_attribute(
        el, (const lxb_char_t*)"type", 4, &alen);
    bool is_radio = t && alen == 5 && std::memcmp(t, "radio", 5) == 0;
    bool checked = lxb_dom_element_has_attribute(
        el, (const lxb_char_t*)"checked", 7);
    int bx = n->border.x + off_x;
    int by = n->border.y + off_y;
    int bw = n->border.w, bh = n->border.h;
    if (bw <= 0 || bh <= 0) {
        return;
    }
    unsigned int bg = 0xFFFFFFFF, fg = 0xFF8a8a8a, acc = 0xFF0067C0;
    auto it = r->theme_vars.find("--field");
    if (it != r->theme_vars.end()) {
        whaleui_render_parse_color(it->second.c_str(), &bg);
    }
    auto itb = r->theme_vars.find("--border");
    if (itb != r->theme_vars.end()) {
        whaleui_render_parse_color(itb->second.c_str(), &fg);
    }
    auto ita = r->theme_vars.find("--accent");
    if (ita != r->theme_vars.end()) {
        whaleui_render_parse_color(ita->second.c_str(), &acc);
    }
    int cx = bx + bw / 2, cy = by + bh / 2;
    if (is_radio) {
        int rad = bw / 2;
        fill_round_rect(r->pixels, r->fb_w, r->fb_h, bx, by, bw, bh, rad,
                        bg, clip);
        /* ring: 1px dark outline via a smaller inner fill */
        fill_round_rect(r->pixels, r->fb_w, r->fb_h, bx + 1, by + 1,
                        bw - 2, bh - 2, rad - 1, bg, clip);
        /* outline */
        for (int k = 0; k < 1; ++k) {
            fill_round_rect(r->pixels, r->fb_w, r->fb_h, bx + 1, by + 1,
                            bw - 2, bh - 2, rad - 1, fg, clip);
        }
        fill_round_rect(r->pixels, r->fb_w, r->fb_h, bx + 1, by + 1,
                        bw - 2, bh - 2, rad - 1, bg, clip);
        if (checked) {
            fill_round_rect(r->pixels, r->fb_w, r->fb_h, cx - bw / 4,
                            cy - bh / 4, bw / 2, bh / 2, bw / 4, acc, clip);
        }
    } else {
        int rad = 3;
        fill_round_rect(r->pixels, r->fb_w, r->fb_h, bx, by, bw, bh, rad,
                        bg, clip);
        fill_round_rect(r->pixels, r->fb_w, r->fb_h, bx + 1, by + 1,
                        bw - 2, bh - 2, rad, fg, clip);
        fill_round_rect(r->pixels, r->fb_w, r->fb_h, bx + 1, by + 1,
                        bw - 2, bh - 2, rad, bg, clip);
        if (checked) {
            /* check mark: two strokes drawn as thick lines */
            int sw = bw / 5;
            if (sw < 1) {
                sw = 1;
            }
            fill_rect(r->pixels, r->fb_w, r->fb_h,
                      bx + bw / 5, cy, bw / 4, sw, acc, clip);
            fill_rect(r->pixels, r->fb_w, r->fb_h,
                      bx + bw / 2 - sw / 2, cy, sw, bh / 3, acc, clip);
            fill_rect(r->pixels, r->fb_w, r->fb_h,
                      bx + bw / 2, cy - bh / 4, bw / 3, sw, acc, clip);
        }
    }
}

/* <progress>/<meter>: a track with a filled portion (value/max, clamped) */
void paint_progress(whaleui_render_t* r, whaleui_layout_node_t* n,
                    int off_x, int off_y, const Clip* clip)
{
    lxb_dom_element* el = n->el;
    if (!el) {
        return;
    }
    double value = 0, max = 1;
    size_t alen = 0;
    const lxb_char_t* v = lxb_dom_element_get_attribute(
        el, (const lxb_char_t*)"value", 5, &alen);
    if (v && alen > 0) {
        value = std::atof(reinterpret_cast<const char*>(v));
    }
    const lxb_char_t* m = lxb_dom_element_get_attribute(
        el, (const lxb_char_t*)"max", 3, &alen);
    if (m && alen > 0) {
        double mx = std::atof(reinterpret_cast<const char*>(m));
        if (mx > 0) {
            max = mx;
        }
    }
    if (value < 0) {
        value = 0;
    }
    if (value > max) {
        value = max;
    }
    int bx = n->content.x + off_x;
    int by = n->content.y + off_y;
    int bw = n->content.w, bh = n->content.h;
    if (bw <= 0 || bh <= 0) {
        return;
    }
    unsigned int track = 0xFFE0E0E0, fill = 0xFF0067C0;
    auto it = r->theme_vars.find("--field");
    if (it != r->theme_vars.end()) {
        whaleui_render_parse_color(it->second.c_str(), &track);
    }
    auto ita = r->theme_vars.find("--accent");
    if (ita != r->theme_vars.end()) {
        whaleui_render_parse_color(ita->second.c_str(), &fill);
    }
    int rad = bh / 2;
    fill_round_rect(r->pixels, r->fb_w, r->fb_h, bx, by, bw, bh, rad,
                    track, clip);
    int fw2 = static_cast<int>(bw * (value / max));
    if (fw2 > 0) {
        fill_round_rect(r->pixels, r->fb_w, r->fb_h, bx, by, fw2, bh, rad,
                        fill, clip);
    }
}

/* editable controls: input paints its value text (always); when focused, a
 * selection highlight + caret + IME composition overlay are drawn on top.
 * textarea/contenteditable glyphs come from the layout text run - this only
 * adds the interaction layer. */
void paint_editable(whaleui_render_t* r, whaleui_layout_node_t* n,
                    int off_x, int off_y, const Clip* clip)
{
    lxb_dom_element* el = n->el;
    if (!el || !is_editable(el)) {
        return;
    }
    int fs = 16;
    std::string fsv = sget(n->style, "font-size");
    if (!fsv.empty()) {
        fs = std::atoi(fsv.c_str());
    }
    std::string family = sget(n->style, "font-family");
    std::string fw = sget(n->style, "font-weight");
    bool bold = fw == "bold" || fw == "bolder" ||
                (!fw.empty() && std::atoi(fw.c_str()) >= 600);
    std::string fst = sget(n->style, "font-style");
    int style = (bold ? kFontBold : 0) |
                ((fst == "italic" || fst == "oblique") ? kFontItalic : 0);
    std::string val = edit_value(el);
    bool focused = (el == r->edit_el);
    /* caret/highlight geometry: input centers in its content box; textarea/
     * contenteditable follow the layout text run so they line up with the
     * painted glyphs (which sit at the run's top, not the box center) */
    whaleui_layout_node_t* geo = editable_geo(n);
    int tx = 0, ty = 0;
    text_origin(r, geo, val, fs, family, bold, &tx, &ty);
    tx += off_x;
    ty += off_y;
    int th = text_line_h(r, fs, family, bold);

    if (tag_eq(el, "input")) {
        /* input has no text children: paint the value here, always */
        unsigned int fg = color_of(n->style, "color", 0xFF1a1a1a);
        draw_text_at(r, val, n->content.x + off_x, n->content.y,
                     n->content.w, n->content.h,
                     fs, family, fg, style, 0, el, clip);
    }
    if (focused) {
        size_t a = 0, b = 0;
        if (sel_range_for(r, el, val.size(), &a, &b, -1, -1, -1)) {
            std::vector<TRect> rects = sel_rects(r, val, fs, family, bold, a, b);
            expand_hl_rects(rects, text_line_h(r, fs, family, bold));
            unsigned int hl = sel_hl_color(r, n, 0x3C);
            for (size_t i = 0; i < rects.size(); ++i) {
                fill_rect(r->pixels, r->fb_w, r->fb_h,
                          tx + rects[i].x, ty + rects[i].y,
                          rects[i].w, rects[i].h, hl, clip);
            }
        }
        size_t caret = static_cast<size_t>(r->sel_focus);
        if (caret > val.size()) {
            caret = val.size();
        }
        /* keep the IME candidate window anchored at the caret */
        update_ime_area(r, val, fs, family, bold, caret, tx, ty);
        paint_caret(r, tx, ty, val, fs, family, bold, caret, clip);
        if (!r->compose.empty()) {
            unsigned int fg = color_of(n->style, "color", 0xFF1a1a1a);
            unsigned int comp = (0xC8 << 24) | (fg & 0x00FFFFFF);
            int cxx = 0, cyy = 0, chh = 16;
            caret_pos(r, val, fs, family, bold, caret, &cxx, &cyy, &chh);
            draw_text_at(r, r->compose, tx + cxx, ty + cyy, 300, chh,
                         fs, family, comp, style, 0, el, clip);
        }
    }
}

/* vertical scrollbar for scrollable containers and the page (html root).
 * Painted over the content on the container's right edge; thumb height
 * tracks the visible fraction, position tracks scroll_y/scroll_max. */
void paint_scrollbar(whaleui_render_t* r, whaleui_layout_node_t* n,
                     int off_x, int off_y, const Clip* clip)
{
    if (n->scroll_max <= 0 || n->border.h < 24) {
        return;
    }
    const int bw = 8;
    int track_x = n->border.x + off_x + n->border.w - bw;
    int track_y = n->border.y + off_y;
    int track_h = n->border.h;
    int content_h = n->scroll_max + n->content.h;
    if (content_h <= 0) {
        return;
    }
    int thumb_h = track_h * n->content.h / content_h;
    if (thumb_h < 10) {
        thumb_h = 10;
    }
    if (thumb_h > track_h) {
        thumb_h = track_h;
    }
    /* live scroll position (layout tree may be stale between scrolls) */
    int sy = n->scroll_y;
    auto sit = r->scrolls.find(n->el);
    if (sit != r->scrolls.end()) {
        sy = sit->second;
    }
    int thumb_y = track_y +
                  (track_h - thumb_h) * sy /
                      (n->scroll_max > 0 ? n->scroll_max : 1);
    unsigned int track = 0x10000000;
    unsigned int thumb = 0x50000000;
    auto it = r->theme_vars.find("--border");
    if (it != r->theme_vars.end()) {
        unsigned int c = 0;
        if (whaleui_render_parse_color(it->second.c_str(), &c) == 0) {
            track = (0x18 << 24) | (c & 0x00FFFFFF);
            thumb = (0x55 << 24) | (c & 0x00FFFFFF);
        }
    }
    fill_rect(r->pixels, r->fb_w, r->fb_h, track_x, track_y, bw, track_h,
              track, clip);
    fill_rect(r->pixels, r->fb_w, r->fb_h, track_x, thumb_y, bw, thumb_h,
              thumb, clip);
}

void paint_text(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
                int off_y, int seq, int sel_lo, int sel_hi, const Clip* clip)
{
    /* cull runs fully outside the framebuffer: creating a TTF_Text per run
     * per frame is the dominant paint cost on scroll-heavy pages */
    int ty = n->border.y + off_y;
    if (ty + n->border.h <= 0 || ty >= r->fb_h) {
        return;
    }
    unsigned int color = color_of(n->style, "color", 0xFF000000);
    float alpha = (color >> 24) & 0xFF;
    unsigned int a8 = static_cast<unsigned>(alpha * n->opacity);
    color = (a8 << 24) | (color & 0x00FFFFFF);

    int fs = 16;
    std::string fsv = sget(n->style, "font-size");
    if (!fsv.empty()) {
        fs = std::atoi(fsv.c_str());
    }
    std::string family = sget(n->style, "font-family");
    /* font-weight: bold/bolder/600-900 render bold; font-style italic/
     * oblique render italic (TTF style bits, per-tag UA defaults make
     * <strong>/<em> work without any page CSS) */
    std::string fw = sget(n->style, "font-weight");
    bool bold = fw == "bold" || fw == "bolder" ||
                (!fw.empty() && std::atoi(fw.c_str()) >= 600);
    std::string fst = sget(n->style, "font-style");
    int style = (bold ? kFontBold : 0) |
                ((fst == "italic" || fst == "oblique") ? kFontItalic : 0);
    /* text-align aligns within the parent element's content box; inline
     * line members (mixed with siblings) stay left-aligned at their laid
     * out x so the line flows as one run sequence */
    std::string ta = sget(n->style, "text-align");
    int align = 0;
    if (ta == "center") {
        align = 1;
    } else if (ta == "right") {
        align = 2;
    }
    if (n->in_inline) {
        align = 0;
    }
    /* text-transform + letter-spacing apply to the painted text (ASCII
     * transforms keep the byte length, so selection offsets stay valid) */
    std::string shown = n->text;
    std::string tt = sget(n->style, "text-transform");
    if (!tt.empty() && tt != "none") {
        apply_text_transform(shown, tt);
    }
    int lsp = 0;
    std::string lsv = sget(n->style, "letter-spacing");
    if (!lsv.empty() && lsv != "normal") {
        float v = static_cast<float>(std::atof(lsv.c_str()));
        lsp = static_cast<int>(lsv.find("em") != std::string::npos
                                   ? v * fs
                                   : v);
    }
    whaleui_layout_node_t* box = n;
    if (n->parent && !n->parent->is_text) {
        box = n->parent;
    }
    paint_text_selection(r, n, fs, family, bold, off_x, off_y, seq, sel_lo,
                         sel_hi, clip);
    bool wrap = sget(n->style, "white-space") != "nowrap";
    /* paint at the run's laid-out position: block runs sit at the parent
     * content origin, inline-line runs at their accumulated x. The wrap
     * width is the line remainder (parent right edge - run x). */
    int tx0 = n->border.x + off_x;
    int ty0 = n->border.y + off_y;
    int avail_w = box->content.x + box->content.w - n->border.x;
    if (avail_w < 1) {
        avail_w = 1;
    }

    /* text-shadow: offset copy (plus 2 spread layers approximating the
     * blur) painted under the glyphs. The glyph raster is cached per
     * element, so the copies only re-tint at blend time. */
    std::string tsh = sget(n->style, "text-shadow");
    if (!tsh.empty() && tsh != "none") {
        int sox = 0, soy = 0, sblur = 0;
        unsigned int scol = 0;
        if (parse_shadow_any(tsh, sox, soy, sblur, scol) == 0) {
            unsigned int sa = (scol >> 24) & 0xFF;
            if (sblur > 0) {
                for (int k = 1; k <= 2 && k <= sblur; ++k) {
                    unsigned int ka =
                        static_cast<unsigned>(sa * (sblur - k + 1) /
                                              (sblur + 1));
                    unsigned int sc = (ka << 24) | (scol & 0x00FFFFFF);
                    draw_text_at(r, shown, tx0 + sox - k, ty0 + soy - k,
                                 avail_w, n->border.h, fs, family, sc,
                                 style, align, n->el, clip, lsp, wrap);
                }
            }
            draw_text_at(r, shown, tx0 + sox, ty0 + soy, avail_w,
                         n->border.h, fs, family, scol, style, align, n->el,
                         clip, lsp, wrap);
        }
    }
    /* -webkit-text-stroke: 8-direction outline copies (stroke width is
     * usually 1px; outline color replaces the transparent fill) */
    std::string tsk = sget(n->style, "-webkit-text-stroke");
    if (tsk.empty()) {
        tsk = sget(n->style, "text-stroke");
    }
    if (!tsk.empty() && tsk != "none") {
        std::vector<std::string> toks = split_space2(tsk);
        int sw = 1;
        unsigned int scc = 0xFF000000;
        if (!toks.empty()) {
            char* end = nullptr;
            float w = std::strtof(toks[0].c_str(), &end);
            if (end != toks[0].c_str()) {
                sw = w > 0 ? static_cast<int>(w) : 1;
            }
            std::string cstr;
            for (size_t i = 1; i < toks.size(); ++i) {
                if (!cstr.empty()) {
                    cstr += ' ';
                }
                cstr += toks[i];
            }
            if (!cstr.empty()) {
                whaleui_render_parse_color(cstr.c_str(), &scc);
            }
        }
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) {
                    continue;
                }
                draw_text_at(r, shown, tx0 + dx * sw, ty0 + dy * sw,
                             avail_w, n->border.h, fs, family, scc,
                             style, align, n->el, clip, lsp, wrap);
            }
        }
    }

    /* the run's own box carries the scroll shift; draw_text_at centers the
     * glyphs in it, matching text_origin's hit/highlight geometry */
    draw_text_at(r, shown, tx0, ty0, avail_w, n->border.h,
                 fs, family, color, style, align, n->el, clip, lsp, wrap);

    /* text-decoration: underline / line-through lines over the box (width
     * = the run's own text extent, clamped to the line remainder) */
    std::string td = sget(n->style, "text-decoration");
    if (!td.empty() && td != "none") {
        int lw = 0;
        for (size_t i = 0; i < shown.size();) {
            unsigned char c2 = static_cast<unsigned char>(shown[i]);
            if (c2 < 0x80) {
                lw += fs / 2;
                ++i;
            } else {
                size_t n2 = (c2 & 0xE0) == 0xC0 ? 2 : (c2 & 0xF0) == 0xE0 ? 3 : 4;
                lw += fs;
                i += n2;
            }
        }
        if (lw > avail_w) {
            lw = avail_w;
        }
        if (lw < 1) {
            lw = 1;
        }
        if (td.find("underline") != std::string::npos) {
            fill_rect(r->pixels, r->fb_w, r->fb_h, tx0,
                      ty0 + n->border.h - 2, lw, 1, color, clip);
        }
        if (td.find("line-through") != std::string::npos) {
            fill_rect(r->pixels, r->fb_w, r->fb_h, tx0,
                      ty0 + n->border.h / 2, lw, 1, color, clip);
        }
    }
}

/* --- <select> support --- */

/* collect <option> texts and values of a select element (from the DOM) */
void select_options(lxb_dom_element* sel, std::vector<std::string>& texts,
                    std::vector<std::string>& values)
{
    lxb_dom_node* c = sel->node.first_child;
    while (c) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element* el = lxb_dom_interface_element(c);
            size_t len = 0;
            const lxb_char_t* name = lxb_dom_element_local_name(el, &len);
            if (name && len == 6 && std::memcmp(name, "option", 6) == 0) {
                /* text = concatenated text children */
                std::string txt;
                lxb_dom_node* t = el->node.first_child;
                while (t) {
                    if (t->type == LXB_DOM_NODE_TYPE_TEXT) {
                        const lexbor_str_t* s = &lxb_dom_interface_text(t)->char_data.data;
                        if (s->data) {
                            txt.append(reinterpret_cast<const char*>(s->data), s->length);
                        }
                    }
                    t = t->next;
                }
                texts.push_back(txt);
                size_t vlen = 0;
                const lxb_char_t* v = lxb_dom_element_get_attribute(
                    el, (const lxb_char_t*)"value", 5, &vlen);
                values.push_back(v ? std::string(reinterpret_cast<const char*>(v), vlen) : txt);
            }
        }
        c = c->next;
    }
}

bool is_select_node(whaleui_layout_node_t* n)
{
    if (!n || !n->el) {
        return false;
    }
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(n->el, &len);
    return name && len == 6 && std::memcmp(name, "select", 6) == 0;
}

const int kSelectItemH = 26;

/* draw the current value + arrow (always, from paint_node) */
void paint_select_value(whaleui_render_t* r, whaleui_layout_node_t* n,
                        int off_x, int off_y, const Clip* clip)
{
    std::vector<std::string> texts, values;
    select_options(n->el, texts, values);
    if (texts.empty()) {
        return;
    }
    int sel = r->select_index.count(n->el) ? r->select_index[n->el] : 0;
    if (sel < 0 || sel >= static_cast<int>(texts.size())) {
        sel = 0;
    }
    int fs = 13;
    unsigned int fg = color_of(n->style, "color", 0xFF1a1a1a);
    /* value text left, arrow pinned to the right edge. Both are nudged up
     * 2px: segoe's glyphs sit ~2px below the line-box center, so metric
     * centering alone leaves the text looking low inside the control. */
    int arrow_x = n->content.x + off_x + n->content.w - 16;
    int text_w = arrow_x - n->content.x - off_x - 8;
    if (text_w < 10) {
        text_w = 10;
    }
    int vy = n->content.y + off_y - 2;
    draw_text_at(r, texts[sel], n->content.x + off_x + 2, vy,
                 text_w, n->content.h, fs, "", fg, 0, 0, n->el, clip);
    /* large solid triangle ¨‹: the small ? is only a few px tall and looks
     * like it floats above the text baseline */
    draw_text_at(r, "\xe2\x96\xbc", arrow_x, vy,
                 16, n->content.h, fs, "", fg, 0, 0, n->el, clip);
}

/* the expanded option list. Painted LAST (highest z), after the whole
 * document, so later siblings cannot cover it. */
void paint_select_list(whaleui_render_t* r, whaleui_layout_node_t* n,
                       int off_y, const Clip* clip)
{
    std::vector<std::string> texts, values;
    select_options(n->el, texts, values);
    if (texts.empty()) {
        return;
    }
    int sel = r->select_index.count(n->el) ? r->select_index[n->el] : 0;
    if (sel < 0 || sel >= static_cast<int>(texts.size())) {
        sel = 0;
    }
    int fs = 13;
    unsigned int fg = color_of(n->style, "color", 0xFF1a1a1a);
    int list_x = n->border.x;
    int list_y = n->border.y + off_y + n->border.h;
    int list_w = n->border.w;
    int list_h = static_cast<int>(texts.size()) * kSelectItemH;
    /* the popup follows the select's corner radius (default theme radius),
     * instead of square corners that clash with the rounded control */
    int radius = 0;
    std::string br = sget(n->style, "border-radius");
    if (!br.empty()) {
        radius = std::atoi(br.c_str());
    }
    unsigned int bg = 0xFF000000;
    auto it = r->theme_vars.find("--card");
    if (it != r->theme_vars.end()) {
        whaleui_render_parse_color(it->second.c_str(), &bg);
    }
    /* soft shadow under the popup (before the card background) */
    {
        const int blur = 6;
        for (int k = blur; k >= 1; --k) {
            unsigned int ka = static_cast<unsigned>(0x28 * (blur - k + 1) / (blur + 1));
            unsigned int c = (ka << 24); /* black, alpha faded outwards */
            if (radius > 0) {
                fill_round_rect(r->pixels, r->fb_w, r->fb_h,
                                list_x + 0 - k, list_y + 2 - k,
                                list_w + 2 * k, list_h + 2 * k, radius, c, clip);
            } else {
                fill_rect(r->pixels, r->fb_w, r->fb_h,
                          list_x + 0 - k, list_y + 2 - k,
                          list_w + 2 * k, list_h + 2 * k, c, clip);
            }
        }
    }
    if (radius > 0) {
        fill_round_rect(r->pixels, r->fb_w, r->fb_h, list_x, list_y,
                        list_w, list_h, radius, bg, clip);
    } else {
        fill_rect(r->pixels, r->fb_w, r->fb_h, list_x, list_y, list_w,
                  list_h, bg, clip);
    }
    unsigned int border_c = 0xFF000000;
    auto itb = r->theme_vars.find("--border");
    if (itb != r->theme_vars.end()) {
        whaleui_render_parse_color(itb->second.c_str(), &border_c);
    }
    /* selected + hovered rows get a translucent accent wash */
    unsigned int acc = 0xFF0067C0;
    auto ita = r->theme_vars.find("--accent");
    if (ita != r->theme_vars.end()) {
        whaleui_render_parse_color(ita->second.c_str(), &acc);
    }
    unsigned int sel_wash = (0x26 << 24) | (acc & 0x00FFFFFF); /* ~15% accent */
    unsigned int hover_wash = (0x14 << 24) | (acc & 0x00FFFFFF);
    for (int i = 0; i < static_cast<int>(texts.size()); ++i) {
        int iy = list_y + i * kSelectItemH;
        if (i == r->open_select_hover) {
            if (radius > 0) {
                fill_round_rect(r->pixels, r->fb_w, r->fb_h, list_x, iy,
                                list_w, kSelectItemH, radius, hover_wash, clip);
            } else {
                fill_rect(r->pixels, r->fb_w, r->fb_h, list_x, iy,
                          list_w, kSelectItemH, hover_wash, clip);
            }
        }
        if (i == sel) {
            if (radius > 0) {
                fill_round_rect(r->pixels, r->fb_w, r->fb_h, list_x, iy,
                                list_w, kSelectItemH, radius, sel_wash, clip);
            } else {
                fill_rect(r->pixels, r->fb_w, r->fb_h, list_x, iy,
                          list_w, kSelectItemH, sel_wash, clip);
            }
            draw_text_at(r, texts[i], list_x + 8, iy, list_w - 16,
                         kSelectItemH, fs, "", acc, true, 0, n->el, clip);
        } else {
            draw_text_at(r, texts[i], list_x + 8, iy, list_w - 16,
                         kSelectItemH, fs, "", fg, 0, 0, n->el, clip);
        }
    }
    /* border around the list (follows the corner arcs when rounded) */
    fill_round_border(r->pixels, r->fb_w, r->fb_h, list_x, list_y,
                      list_w, list_h, radius, 1, border_c, bg, clip);
}

/* depth-first hit test; coordinates are absolute (layout boxes are absolute).
 * Children are checked BEFORE the parent box: a child may stick out of the
 * parent (e.g. space-between overflow), and must still be clickable.
 * off_y carries the paint-time scroll offset (see scroll_delta). */
whaleui_layout_node_t* hit_test(whaleui_render_t* r, whaleui_layout_node_t* n,
                                int x, int y, int off_y)
{
    if (!n || !n->visible) {
        return nullptr;
    }
    int child_off = off_y + scroll_delta(r, n);
    for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
        whaleui_layout_node_t* hit = hit_test(r, c, x, y, child_off);
        if (hit) {
            return hit;
        }
    }
    if (x >= n->border.x && x < n->border.x + n->border.w &&
        y >= n->border.y + off_y && y < n->border.y + off_y + n->border.h) {
        return n;
    }
    return nullptr;
}

/* parse "ox oy blur color" (one shadow; color may be rgba(), written
 * without spaces). Returns 0 when no usable shadow is present. */
int parse_shadow(const std::string& v, int& ox, int& oy, int& blur,
                 unsigned int& col)
{
    ox = oy = blur = 0;
    col = 0;
    std::vector<std::string> tok;
    const char* p = v.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char* s = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
        tok.emplace_back(s, static_cast<size_t>(p - s));
    }
    int nums[3] = {0, 0, 0};
    size_t ni = 0;
    std::string colstr;
    for (size_t i = 0; i < tok.size(); ++i) {
        const std::string& t = tok[i];
        if (ni < 3 && !t.empty() &&
            (t[0] == '-' || (t[0] >= '0' && t[0] <= '9'))) {
            nums[ni++] = std::atoi(t.c_str());
        } else if (!colstr.empty()) {
            colstr += ' ';
            colstr += t;
        } else {
            colstr = t;
        }
    }
    ox = nums[0];
    oy = nums[1];
    blur = nums[2];
    if (blur <= 0 || colstr.empty()) {
        return -1;
    }
    return whaleui_render_parse_color(colstr.c_str(), &col);
}

/* parse_shadow variant that accepts blur == 0 (text-shadow without a
 * blur radius) */
int parse_shadow_any(const std::string& v, int& ox, int& oy, int& blur,
                     unsigned int& col)
{
    int rc = parse_shadow(v, ox, oy, blur, col);
    if (rc != 0) {
        /* retry allowing zero blur: split tokens and treat the color as
         * the last token */
        std::vector<std::string> tok;
        const char* p = v.c_str();
        while (*p) {
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            if (!*p) {
                break;
            }
            const char* s = p;
            while (*p && *p != ' ' && *p != '\t') {
                ++p;
            }
            tok.emplace_back(s, static_cast<size_t>(p - s));
        }
        ox = oy = blur = 0;
        col = 0;
        int nums[2] = {0, 0};
        size_t ni = 0;
        std::string colstr;
        for (size_t i = 0; i < tok.size(); ++i) {
            const std::string& t = tok[i];
            if (ni < 2 && !t.empty() &&
                (t[0] == '-' || (t[0] >= '0' && t[0] <= '9'))) {
                nums[ni++] = std::atoi(t.c_str());
            } else if (!colstr.empty()) {
                colstr += ' ';
                colstr += t;
            } else {
                colstr = t;
            }
        }
        ox = nums[0];
        oy = nums[1];
        if (colstr.empty()) {
            return -1;
        }
        return whaleui_render_parse_color(colstr.c_str(), &col);
    }
    return rc;
}

/* whitespace tokenizer */
std::vector<std::string> split_space2(const std::string& v)
{
    std::vector<std::string> out;
    const char* p = v.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char* s = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
        out.emplace_back(s, static_cast<size_t>(p - s));
    }
    return out;
}

/* soft shadow under a box: concentric rounded rects expanding up to `blur`
 * px, alpha fading outwards. Painted BEFORE the background so the element
 * body covers the inner layers. */
void paint_shadow(whaleui_render_t* r, whaleui_layout_node_t* n,
                  int off_x, int off_y, const Clip* clip)
{
    std::string v = sget(n->style, "box-shadow");
    if (v.empty() || v == "none") {
        return;
    }
    int ox = 0, oy = 0, blur = 0;
    unsigned int col = 0;
    if (parse_shadow(v, ox, oy, blur, col) != 0) {
        return;
    }
    /* off-screen shadows cost nothing */
    int sx0 = n->border.x + off_x + ox - blur;
    int sy0 = n->border.y + off_y + oy - blur;
    if (sx0 + n->border.w + 2 * blur <= 0 || sx0 >= r->fb_w ||
        sy0 + n->border.h + 2 * blur <= 0 || sy0 >= r->fb_h) {
        return;
    }
    int radius = 0;
    std::string br = sget(n->style, "border-radius");
    if (!br.empty()) {
        radius = std::atoi(br.c_str());
    }
    unsigned int a = (col >> 24) & 0xFF;
    if (a == 0) {
        return;
    }
    /* GPU path: the mipmap blur approximation (blur_tex + multi-level
     * resampling) - one shape + one sampling quad instead of blur/2
     * concentric fills */
    if (g_gpu) {
        whaleui_gpu_shadow(g_gpu, static_cast<float>(n->border.x + off_x + ox),
                           static_cast<float>(n->border.y + off_y + oy),
                           static_cast<float>(n->border.w),
                           static_cast<float>(n->border.h),
                           static_cast<float>(radius),
                           static_cast<float>(blur), col);
        return;
    }
    /* concentric layers, alpha fading outwards; skip the near-invisible
     * outermost ring and step by 2px (half the fill cost, gradient still
     * smooth) */
    for (int k = (blur > 0 ? blur - 1 : 0); k >= 1; k -= 2) {
        unsigned int ka = static_cast<unsigned>(a * (blur - k + 1) / (blur + 1));
        if (ka < 4) {
            continue;
        }
        unsigned int c = (ka << 24) | (col & 0x00FFFFFF);
        int sx = n->border.x + off_x + ox - k;
        int sy = n->border.y + off_y + oy - k;
        int sw = n->border.w + 2 * k;
        int sh = n->border.h + 2 * k;
        if (radius > 0) {
            fill_round_rect(r->pixels, r->fb_w, r->fb_h, sx, sy, sw, sh,
                            radius, c, clip);
        } else {
            fill_rect(r->pixels, r->fb_w, r->fb_h, sx, sy, sw, sh, c, clip);
        }
    }
}

/* decode an <img> src into a surface. Only local sources load (file:// or
 * a plain path); http(s) and data: URIs have no network stack here and fall
 * back to the placeholder box. Results cached per src (NULL = not
 * loadable). */
SDL_Surface* img_surface(whaleui_render_t* r, const std::string& src)
{
    if (src.empty()) {
        return nullptr;
    }
    auto it = r->images.find(src);
    if (it != r->images.end()) {
        return it->second;
    }
    SDL_Surface* s = nullptr;
    if (src.compare(0, 7, "http://") != 0 &&
        src.compare(0, 8, "https://") != 0 &&
        src.compare(0, 5, "data:") != 0) {
        std::string path = src;
        if (path.compare(0, 7, "file://") == 0) {
            path = path.substr(7);
            if (!path.empty() && (path[0] == '/' || path[0] == '\\')) {
                path = path.substr(1); /* file:///C:/x -> C:/x */
            }
        }
#ifdef WHALEUI_BUILD_FULL
        s = IMG_Load(path.c_str());
#else
        /* stb_image decode (lite/minimal have no SDL_image) */
        int iw = 0, ih = 0, ich = 0;
        unsigned char* px = stbi_load(path.c_str(), &iw, &ih, &ich, 4);
        if (px && iw > 0 && ih > 0) {
            s = SDL_CreateSurface(iw, ih, SDL_PIXELFORMAT_RGBA32);
            if (s) {
                std::memcpy(s->pixels, px, static_cast<size_t>(iw) * ih * 4);
            }
        }
        if (px) {
            stbi_image_free(px);
        }
#endif
    }
    r->images[src] = s;
    return s;
}

/* paint an <img> element: decoded bitmap honoring object-fit, or a
 * placeholder box when the source is missing/remote/undecodable. The bitmap
 * blends into the CPU text layer (GPU path) / framebuffer (CPU path) - the
 * layer composites above geometry, which suits page content images
 * (ponytail: z-order above the parent border is a known corner). */
void paint_img(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
               int off_y, const Clip* clip)
{
    size_t alen = 0;
    const lxb_char_t* src = lxb_dom_element_get_attribute(
        n->el, (const lxb_char_t*)"src", 3, &alen);
    std::string srcs = src ? std::string(reinterpret_cast<const char*>(src),
                                         alen)
                           : std::string();
    int x = n->border.x + off_x;
    int y = n->border.y + off_y;
    int w = n->border.w;
    int h = n->border.h;
    if (x + w <= 0 || y + h <= 0 || x >= r->fb_w || y >= r->fb_h) {
        return;
    }
    SDL_Surface* img = img_surface(r, srcs);
    if (img && img->w > 0 && img->h > 0) {
        /* object-fit: cover keeps the aspect and crops, contain letterboxes,
         * fill (default) stretches */
        std::string of = sget(n->style, "object-fit");
        int dw = w, dh = h, dx = x, dy = y;
        if (of == "cover" || of == "contain") {
            float ar = static_cast<float>(img->w) / img->h;
            float dar = w > 0 ? static_cast<float>(w) / h : 1.0f;
            if ((of == "cover") == (ar > dar)) {
                dh = static_cast<int>(w / ar);
                dy = y + (h - dh) / 2;
            } else {
                dw = static_cast<int>(h * ar);
                dx = x + (w - dw) / 2;
            }
        }
        if (dw > 0 && dh > 0) {
            SDL_Surface* scaled = SDL_ScaleSurface(img, dw, dh,
                                                   SDL_SCALEMODE_LINEAR);
            if (scaled) {
                if (g_gpu) {
                    blend_surface(r->text_layer, r->fb_w, r->fb_h, scaled,
                                  dx, dy, clip, nullptr);
                } else {
                    blend_surface(r->pixels, r->fb_w, r->fb_h, scaled,
                                  dx, dy, clip, nullptr);
                }
                SDL_DestroySurface(scaled);
            }
        }
        return;
    }
    /* placeholder box: dim fill + hairline border, so broken images still
     * occupy their laid-out space visibly */
    unsigned int dim = 0xFF2A2E38;
    fill_rect(r->pixels, r->fb_w, r->fb_h, x, y, w, h, dim, clip);
    unsigned int edge = 0xFF3A4150;
    fill_rect(r->pixels, r->fb_w, r->fb_h, x, y, w, 1, edge, clip);
    fill_rect(r->pixels, r->fb_w, r->fb_h, x, y + h - 1, w, 1, edge, clip);
    fill_rect(r->pixels, r->fb_w, r->fb_h, x, y, 1, h, edge, clip);
    fill_rect(r->pixels, r->fb_w, r->fb_h, x + w - 1, y, 1, h, edge, clip);
}


/* subtree paint bounds: union of this node's border box and every visible
 * descendant (absolute viewport coords - the layout pass already positions
 * nodes absolutely, so no offsets are needed). Computed once per layout
 * pass; partial repaints then cull whole subtrees instead of walking them. */
void compute_paint_bounds(whaleui_layout_node_t* n)
{
    whaleui_rect_t b = n->border;
    for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
        if (!c->visible) {
            continue;
        }
        compute_paint_bounds(c);
        int x0 = b.x, y0 = b.y, x1 = b.x + b.w, y1 = b.y + b.h;
        int cx0 = c->bounds.x, cy0 = c->bounds.y;
        int cx1 = c->bounds.x + c->bounds.w, cy1 = c->bounds.y + c->bounds.h;
        if (cx0 < x0) x0 = cx0;
        if (cy0 < y0) y0 = cy0;
        if (cx1 > x1) x1 = cx1;
        if (cy1 > y1) y1 = cy1;
        b.x = x0;
        b.y = y0;
        b.w = x1 - x0;
        b.h = y1 - y0;
    }
    n->bounds = b;
}

/* cull a subtree from a repaint when its paint bounds (shifted by the
 * off_x/off_y offset chain: transform translation + scroll delta) are
 * entirely outside the clip. The clip is the dirty region for partial
 * repaints and the whole viewport for full repaints - on a scrolled page
 * most of the document is off-screen either way, so skipping their subtrees
 * is the biggest paint win. A margin covers box-shadow bleed and selection
 * padding. Full-viewport clips cull too; transformed / fixed subtrees are
 * handled by paint_node and sel_seq through the `tc` flag instead (their
 * painted box differs from the laid-out bounds). */
const int kCullMargin = 64;
bool paint_cull(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
                int off_y, const Clip* clip)
{
    (void)r;
    if (!clip || clip->w <= 0 || clip->h <= 0) {
        return false;
    }
    int x0 = n->bounds.x + off_x - kCullMargin;
    int y0 = n->bounds.y + off_y - kCullMargin;
    int x1 = n->bounds.x + n->bounds.w + off_x + kCullMargin;
    int y1 = n->bounds.y + n->bounds.h + off_y + kCullMargin;
    return x1 <= clip->x || x0 >= clip->x + clip->w ||
           y1 <= clip->y || y0 >= clip->y + clip->h;
}

void paint_node(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
                int off_y, int& seq, int sel_lo, int sel_hi,
                const Clip* clip, bool tc)
{
    if (!n->visible) {
        return;
    }
    const int my_seq = seq++;
    /* transform value read ONCE per node (the tc check below and the paint
     * transform block both need it); position:fixed is checked only when no
     * transform is present. Transformed / fixed subtrees are painted at
     * offset positions the layout bounds don't know about: never cull them,
     * and propagate the flag down so descendants use the same rule. */
    std::string ntv; /* "transform" value of this node ("" = none) */
    bool has_xf = false;
    if (n->el && !n->is_text) {
        ntv = sget(n->style, "transform");
        has_xf = !ntv.empty() && ntv != "none";
        if (!tc && has_xf) {
            tc = true;
        } else if (!tc && sget(n->style, "position") == "fixed") {
            tc = true;
        } else if (!tc && sget(n->style, "position") == "sticky") {
            /* sticky pins the box at top:N while scrolling, so its painted
             * box leaves the layout bounds - culling on the laid-out
             * position would drop it (and its subtree) mid-scroll */
            tc = true;
        }
    }
    /* a selection endpoint outside the repaint region must still receive
     * its sequence number (sel_seq does the same), so the in-viewport
     * middle of the selection keeps highlighting. Text runs skip the
     * subtree cull entirely: their laid-out bounds carry the estimated
     * text width, which can fall short of the real glyph extent (inline
     * lines, letter-spacing) - culling on it drops glyphs that actually
     * reach the repaint strip. paint_text has its own framebuffer check
     * and the clip clips the horizontal overflow. */
    if (!tc && !n->is_text &&
        !(n->el && (n->el == r->sel_anchor_el ||
                    n->el == r->sel_focus_el)) &&
        paint_cull(r, n, off_x, off_y, clip)) {
        return; /* whole subtree outside the repaint region */
    }
    if (n->is_text) {
        paint_text(r, n, off_x, off_y, my_seq, sel_lo, sel_hi, clip);
        return;
    }
    /* position:fixed elements are laid out against the viewport and must
     * not move with ancestor scroll offsets */
    if (sget(n->style, "position") == "fixed") {
        off_x = 0;
        off_y = 0;
    }
    /* position:sticky: keep the box pinned at top:N once scrolling would
     * push it past that edge (the layout pass already shifted the box by
     * the scroll amount, so border.y + off_y is the current viewport
     * position). Simplified to the page/root scroll; container-bottom
     * clamping is skipped (ponytail: add when a page needs it). */
    if (sget(n->style, "position") == "sticky") {
        /* top is usually px; %/vh are rare on sticky (ponytail) */
        int st = std::atoi(sget(n->style, "top").c_str());
        int cur = n->border.y + off_y;
        if (cur < st) {
            off_y += st - cur;
        }
    }
    /* transform: translate (px/%) + uniform scale around the element's
     * center (default transform-origin). The shifted box is drawn here and
     * off_x/off_y carry the translation into the subtree. Child coordinates
     * are NOT scaled. ponytail: scale is visual on this box; true 2D
     * transform stacks + hit-testing are a later step. */
    float tdx = 0.0f, tdy = 0.0f, ts = 1.0f;
    if (has_xf) {
        whaleui_transform_t tf;
        if (whaleui_transform_eval(ntv.c_str(),
                                   static_cast<float>(n->border.w),
                                   static_cast<float>(n->border.h), &tf) == 0) {
            tdx = tf.tx;
            tdy = tf.ty;
            ts = tf.sx;
        }
    }
    int bw = n->border.w;
    int bh = n->border.h;
    if (ts != 1.0f) {
        bw = static_cast<int>(n->border.w * ts);
        bh = static_cast<int>(n->border.h * ts);
    }
    int nox = off_x + static_cast<int>(tdx) + (n->border.w - bw) / 2;
    int noy = off_y + static_cast<int>(tdy) + (n->border.h - bh) / 2;
    /* overflow: hidden/auto/scroll clips descendants to the border box
     * (auto/scroll containers also shift children by scroll_y at layout) */
    Clip self;
    const Clip* eff = clip;
    std::string ov = sget(n->style, "overflow");
    if (ov == "hidden" || ov == "auto" || ov == "scroll") {
        self.x = n->border.x + nox;
        self.y = n->border.y + noy;
        self.w = bw;
        self.h = bh;
        eff = &self;
        /* cull clipped containers fully outside the framebuffer: on a
         * scrolled page most of the document is off-screen, so skip their
         * whole subtree (the biggest scroll-paint win) */
        if (self.y + self.h <= 0 || self.y >= r->fb_h) {
            return;
        }
    }
    /* soft shadow first, so the element body covers the inner layers */
    paint_shadow(r, n, nox, noy, eff);
    /* <img>: bitmap (object-fit) or placeholder replaces the background */
    if (n->el && tag_eq(n->el, "img")) {
        paint_img(r, n, nox, noy, eff);
    }
    /* background (border-radius supported). A gradient in `background`/
     * `background-image` paints over the plain color (which shows through
     * the gradient's transparent stops). */
    unsigned int bg = color_of(n->style, "background-color", 0);
    unsigned int bg2 = color_of(n->style, "background", 0);
    if (bg == 0) {
        bg = bg2;
    }
    int radius = 0;
    std::string br = sget(n->style, "border-radius");
    if (!br.empty()) {
        radius = std::atoi(br.c_str());
    }
    unsigned int bg_c = 0; /* inner re-fill color for rounded border rings */
    if (bg != 0) {
        unsigned int a = (bg >> 24) & 0xFF;
        unsigned int a8 = static_cast<unsigned>(a * n->opacity);
        unsigned int c = (a8 << 24) | (bg & 0x00FFFFFF);
        bg_c = c;
    }
    /* backdrop-filter: blur the already-painted background under this box.
     * GPU path: pass C (in gpu_flush) overwrites the region with the
     * blurred geometry and blends the body color on top, so the plain body
     * paint below is skipped - but only on full repaints (scroll/partial
     * frames skip pass C, so the body background paints normally). CPU
     * path has no access to the GPU geometry: the body background still
     * paints (ponytail: real CPU backdrop blur when needed). */
    std::string bdf = sget(n->style, "backdrop-filter");
    bool backdrop = !bdf.empty() && bdf != "none";
    if (backdrop && g_gpu && g_backdrop_active) {
        float bblur = 8.0f;
        size_t bp = bdf.find("blur(");
        if (bp != std::string::npos) {
            bblur = static_cast<float>(std::atof(bdf.c_str() + bp + 5));
        }
        whaleui_gpu_backdrop(g_gpu, static_cast<float>(n->border.x + nox),
                             static_cast<float>(n->border.y + noy),
                             static_cast<float>(bw), static_cast<float>(bh),
                             static_cast<float>(radius), bblur, bg_c);
    } else {
    if (bg != 0) {
        if (radius > 0) {
            fill_round_rect(r->pixels, r->fb_w, r->fb_h, n->border.x + nox,
                            n->border.y + noy,
                            bw, bh, radius, bg_c, eff);
        } else {
            fill_rect(r->pixels, r->fb_w, r->fb_h, n->border.x + nox,
                      n->border.y + noy,
                      bw, bh, bg_c, eff);
        }
    }
    Gradient grad;
    std::string bgv = sget(n->style, "background-image");
    if (bgv.empty()) {
        bgv = sget(n->style, "background");
    }
    if (parse_gradient(bgv, grad)) {
        if (radius > 0) {
            /* gradients ignore the corner arcs (ponytail: fill the full
             * rect; clipped inside rounded containers already) */
        }
        fill_gradient(r->pixels, r->fb_w, r->fb_h, n->border.x + nox,
                      n->border.y + noy, bw, bh, grad, eff);
    }
    /* border (follows the corner arcs when border-radius is set) */
    int bw2[4] = {n->border_w[0], n->border_w[1], n->border_w[2], n->border_w[3]};
    bool any = bw2[0] || bw2[1] || bw2[2] || bw2[3];
    if (any) {
        unsigned int bc = color_of(n->style, "border-color",
                                   border_color_of(n->style, 0xFF000000));
        if (bc != 0) {
            unsigned int a = (bc >> 24) & 0xFF;
            unsigned int a8 = static_cast<unsigned>(a * n->opacity);
            unsigned int c = (a8 << 24) | (bc & 0x00FFFFFF);
            int radius = 0;
            std::string br2 = sget(n->style, "border-radius");
            if (!br2.empty()) {
                radius = std::atoi(br2.c_str());
            }
            /* uniform border width for the ring */
            int brw = bw2[1] > 0 ? bw2[1] : (bw2[3] > 0 ? bw2[3] : (bw2[0] > 0 ? bw2[0] : bw2[2]));
            if (radius > 0 && brw > 0) {
                fill_round_border(r->pixels, r->fb_w, r->fb_h, n->border.x + nox,
                                  n->border.y + noy,
                                  bw, bh, radius, brw, c, bg_c, eff);
            } else {
                if (bw2[0]) { /* top */
                    fill_rect(r->pixels, r->fb_w, r->fb_h, n->border.x + nox,
                              n->border.y + noy, bw, bw2[0], c, eff);
                }
                if (bw2[2]) { /* bottom */
                    fill_rect(r->pixels, r->fb_w, r->fb_h, n->border.x + nox,
                              n->border.y + noy + bh - bw2[2], bw, bw2[2], c, eff);
                }
                if (bw2[1]) { /* right */
                    fill_rect(r->pixels, r->fb_w, r->fb_h,
                              n->border.x + nox + bw - bw2[1],
                              n->border.y + noy, bw2[1], bh, c, eff);
                }
                if (bw2[3]) { /* left */
                    fill_rect(r->pixels, r->fb_w, r->fb_h, n->border.x + nox,
                              n->border.y + noy, bw2[3], bh, c, eff);
                }
            }
        }
    }
    } /* end else: plain body paint (skipped for GPU backdrop-filter) */
    int child_off_y = noy + scroll_delta(r, n);
    for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
        paint_node(r, c, nox, child_off_y, seq, sel_lo, sel_hi, eff, tc);
    }
    /* scrollbar sits on top of the content */
    if (n->scroll_max > 0) {
        paint_scrollbar(r, n, nox, noy, eff);
    }
    /* native controls drawn by the engine (not editable): checkbox/radio
     * boxes and progress/meter tracks. tag_id pre-filters before the
     * attribute checks. */
    const int tid = n->tag_id;
    if (n->el && tid == WUI_TAG_INPUT && is_check_radio(n->el)) {
        paint_checkbox(r, n, nox, noy, eff);
    } else if (n->el &&
               (tid == WUI_TAG_PROGRESS || tid == WUI_TAG_METER)) {
        paint_progress(r, n, nox, noy, eff);
    } else if (n->el && is_editable_node(n)) {
        paint_editable(r, n, nox, noy, eff);
    }
    /* <select> control: value + arrow painted here; the expanded list is
     * painted LAST in render_frame so nothing occludes it */
    if (is_select_node(n)) {
        paint_select_value(r, n, nox, noy, eff);
    }
}

/* --- editing interaction helpers --- */

/* font params of a layout node (matches the paint path) */
void node_font(whaleui_layout_node_t* n, int* fs, std::string* family, bool* bold)
{
    *fs = 16;
    std::string fsv = sget(n->style, "font-size");
    if (!fsv.empty()) {
        *fs = std::atoi(fsv.c_str());
    }
    *family = sget(n->style, "font-family");
    std::string fw = sget(n->style, "font-weight");
    *bold = fw == "bold" || fw == "bolder" ||
            (!fw.empty() && std::atoi(fw.c_str()) >= 600);
}

/* geometry node of an editable box: the layout text run (if present), so
 * the caret/highlight line up with the text the run paints; otherwise the
 * box itself (empty control) */
whaleui_layout_node_t* editable_geo(whaleui_layout_node_t* n)
{
    if (n->first_child && n->first_child->is_text) {
        return n->first_child;
    }
    return n;
}

/* text top-left for hit-testing/highlighting (matches paint_text).
 * Text runs: x from the run's laid-out position (parent content origin for
 * block runs, accumulated inline x on inline lines), y from the run's own
 * box, vertically centered in its height. Containers (editable overlays):
 * content box, centered. */
void text_origin(whaleui_render_t* r, whaleui_layout_node_t* n,
                 const std::string& text, int fs, const std::string& family,
                 bool bold, int* tx, int* ty)
{
    int tw = 0, th = 0;
    text_size(r, text, fs, family, bold, &tw, &th);
    if (n->is_text) {
        *tx = n->border.x;
        *ty = n->border.y + (n->border.h - th) / 2;
    } else {
        *tx = n->content.x;
        *ty = n->content.y + (n->content.h - th) / 2;
    }
}

/* byte offset in a text-run node at (x,y) */
size_t byte_at_node(whaleui_render_t* r, whaleui_layout_node_t* hit, int x, int y)
{
    int fs;
    std::string family;
    bool bold;
    node_font(hit, &fs, &family, &bold);
    int tx = 0, ty = 0;
    text_origin(r, hit, hit->text, fs, family, bold, &tx, &ty);
    return byte_at_text(r, hit->text, fs, family, bold, x - tx, y - ty);
}

/* caret byte offset in an editable element at (x,y); hit is the layout node
 * under the mouse (the text run or the editable box itself). The geometry
 * follows the layout text run so the caret matches where the glyphs paint. */
size_t caret_from_point(whaleui_render_t* r, lxb_dom_element* el,
                        whaleui_layout_node_t* hit, int x, int y)
{
    std::string val = edit_value(el);
    if (val.empty()) {
        return 0;
    }
    whaleui_layout_node_t* box =
        (hit && hit->is_text && hit->parent) ? hit->parent : hit;
    whaleui_layout_node_t* geo =
        (hit && hit->is_text) ? hit : editable_geo(box);
    int fs;
    std::string family;
    bool bold;
    node_font(box, &fs, &family, &bold);
    int tx = 0, ty = 0;
    text_origin(r, geo, val, fs, family, bold, &tx, &ty);
    return byte_at_text(r, val, fs, family, bold, x - tx, y - ty);
}

/* drag: move the selection focus end to the element under the mouse */
void update_selection_focus(whaleui_render_t* r, whaleui_layout_node_t* hit,
                            int x, int y)
{
    if (!hit) {
        return;
    }
    if (hit->is_text) {
        r->sel_focus_el = hit->el;
        r->sel_focus = static_cast<int>(byte_at_node(r, hit, x, y));
        /* repaint only: the highlight reads r->sel_*, the layout tree is
         * unchanged, so dragging must not relayout every mouse move */
        r->scroll_dirty = 1;
    } else if (is_editable(hit->el)) {
        r->sel_focus_el = hit->el;
        r->sel_focus = static_cast<int>(caret_from_point(r, hit->el, hit, x, y));
        r->scroll_dirty = 1;
    }
}

/* replace [a,b) of the editable value with `insertion`, move the caret to
 * the end of the insertion and write the result back into the DOM */
void edit_replace(whaleui_render_t* r, lxb_dom_element* el, size_t a, size_t b,
                  const std::string& insertion)
{
    std::string val = edit_value(el);
    if (a > val.size()) {
        a = val.size();
    }
    if (b > val.size()) {
        b = val.size();
    }
    if (a > b) {
        std::swap(a, b);
    }
    std::string nv = val.substr(0, a) + insertion + val.substr(b);
    edit_set_value(el, nv);
    size_t caret = a + insertion.size();
    r->sel_anchor_el = r->sel_focus_el = el;
    r->sel_anchor = r->sel_focus = static_cast<int>(caret);
    r->compose.clear();
    r->has_dirty = 1;
}

bool is_contenteditable(lxb_dom_element* el)
{
    if (!el || tag_eq(el, "input") || tag_eq(el, "textarea")) {
        return false;
    }
    return is_editable(el);
}

size_t line_start(const std::string& s, size_t b)
{
    size_t p = s.rfind('\n', b == 0 ? std::string::npos : b - 1);
    return p == std::string::npos ? 0 : p + 1;
}
size_t line_end(const std::string& s, size_t b)
{
    size_t p = s.find('\n', b);
    return p == std::string::npos ? s.size() : p;
}

void edit_key(whaleui_render_t* r, int keycode, int mods)
{
    lxb_dom_element* el = r->edit_el;
    if (!el) {
        return;
    }
    std::string val = edit_value(el);
    int a = r->sel_anchor, b = r->sel_focus;
    if (a > b) {
        std::swap(a, b);
    }
    bool ctrl = (mods & SDL_KMOD_CTRL) != 0;
    if (ctrl) {
        if (keycode == 'a') { /* select all */
            r->sel_anchor = 0;
            r->sel_focus = static_cast<int>(val.size());
            r->has_dirty = 1;
        }
        /* other ctrl shortcuts (clipboard etc.) not handled yet */
        return;
    }
    switch (keycode) {
    case WHALEUI_KEY_LEFT:
        r->sel_anchor = r->sel_focus =
            static_cast<int>(utf8_prev(val, static_cast<size_t>(
                r->sel_anchor != r->sel_focus ? a : r->sel_focus)));
        r->has_dirty = 1;
        break;
    case WHALEUI_KEY_RIGHT:
        r->sel_anchor = r->sel_focus =
            static_cast<int>(utf8_next(val, static_cast<size_t>(
                r->sel_anchor != r->sel_focus ? b : r->sel_focus)));
        r->has_dirty = 1;
        break;
    case WHALEUI_KEY_HOME:
        r->sel_anchor = r->sel_focus =
            static_cast<int>(line_start(val, static_cast<size_t>(r->sel_focus)));
        r->has_dirty = 1;
        break;
    case WHALEUI_KEY_END:
        r->sel_anchor = r->sel_focus =
            static_cast<int>(line_end(val, static_cast<size_t>(r->sel_focus)));
        r->has_dirty = 1;
        break;
    case WHALEUI_KEY_UP: { /* simplified: jump to the previous line start */
        size_t ls = line_start(val, static_cast<size_t>(r->sel_focus));
        if (ls > 0) {
            r->sel_anchor = r->sel_focus =
                static_cast<int>(line_start(val, ls - 1));
            r->has_dirty = 1;
        }
        break;
    }
    case WHALEUI_KEY_DOWN: { /* simplified: jump to the next line start */
        size_t le = line_end(val, static_cast<size_t>(r->sel_focus));
        if (le < val.size()) {
            r->sel_anchor = r->sel_focus = static_cast<int>(le + 1);
            r->has_dirty = 1;
        }
        break;
    }
    case WHALEUI_KEY_BACKSPACE:
        if (a != b) {
            edit_replace(r, el, static_cast<size_t>(a), static_cast<size_t>(b), "");
        } else if (a > 0) {
            edit_replace(r, el, utf8_prev(val, static_cast<size_t>(a)),
                         static_cast<size_t>(a), "");
        }
        break;
    case WHALEUI_KEY_DELETE:
        if (a != b) {
            edit_replace(r, el, static_cast<size_t>(a), static_cast<size_t>(b), "");
        } else if (a < static_cast<int>(val.size())) {
            edit_replace(r, el, static_cast<size_t>(a),
                         utf8_next(val, static_cast<size_t>(a)), "");
        }
        break;
    case WHALEUI_KEY_ENTER:
        if (tag_eq(el, "textarea") || is_contenteditable(el)) {
            edit_replace(r, el, static_cast<size_t>(a), static_cast<size_t>(b), "\n");
        }
        break;
    default:
        break;
    }
}

} // namespace

/* scrollbar drag helpers (defined with the wheel/click handling below) */
whaleui_layout_node_t* scrollbar_under(whaleui_render_t* r,
                                       whaleui_layout_node_t* hit, int x,
                                       int y);
void update_drag_scroll(whaleui_render_t* r, int y);
SDL_Cursor* render_cursor(whaleui_render_t* r, SDL_SystemCursor id);

/* --- FSR 1.0 (GPU compute) resources --- */

/* build compute pipelines + textures for the current window size. Returns 1
 * when everything is usable (FSR can run), 0 if any resource failed. */
int render_fsr_create(whaleui_render_t* r)
{
    SDL_GPUDevice* d = r->device;
    /* pick the shader variant for the actual backend: the official AMD FSR
     * SPIR-V shaders (fsr-demo-gpu) on Vulkan, DXIL (compiled from HLSL
     * with dxc) on D3D12 */
    const char* drv = SDL_GetGPUDeviceDriver(d);
    const bool vulkan = drv && std::strcmp(drv, "vulkan") == 0;
    SDL_GPUComputePipelineCreateInfo ci;
    std::memset(&ci, 0, sizeof(ci));
    ci.entrypoint = "main";
    ci.num_readonly_storage_textures = 1;
    ci.num_readwrite_storage_textures = 1;
    ci.num_uniform_buffers = 1;
    ci.threadcount_z = 1;
    if (vulkan) {
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.threadcount_x = 16; /* official EASU workgroup 16x16 */
        ci.threadcount_y = 16;
        ci.code_size = g_fsr_easu_spv_size * 4;
        ci.code = reinterpret_cast<const Uint8*>(g_fsr_easu_spv);
    } else {
        ci.format = SDL_GPU_SHADERFORMAT_DXIL;
        ci.threadcount_x = 8;
        ci.threadcount_y = 8;
        ci.code_size = g_easu_dxil_size * 4;
        ci.code = reinterpret_cast<const Uint8*>(g_easu_dxil);
    }
    r->fsr_easu_pipe = SDL_CreateGPUComputePipeline(d, &ci);
    if (!r->fsr_easu_pipe) {
        return 0;
    }
    if (vulkan) {
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.threadcount_x = 8;
        ci.threadcount_y = 8;
        ci.code_size = g_fsr_rcas_custom_spv_size * 4;
        ci.code = reinterpret_cast<const Uint8*>(g_fsr_rcas_custom_spv);
    } else {
        ci.format = SDL_GPU_SHADERFORMAT_DXIL;
        ci.threadcount_x = 8;
        ci.threadcount_y = 8;
        ci.code_size = g_rcas_dxil_size * 4;
        ci.code = reinterpret_cast<const Uint8*>(g_rcas_dxil);
    }
    r->fsr_rcas_pipe = SDL_CreateGPUComputePipeline(d, &ci);
    if (!r->fsr_rcas_pipe) {
        return 0;
    }
    /* EASU reads the GPU render target (needs no upload buffer anymore);
     * RCAS writes straight into gpu->target2 which is the blit source */
    auto mkTex = [&](int w, int h, SDL_GPUTextureUsageFlags u,
                     SDL_GPUTexture** out) {
        SDL_GPUTextureCreateInfo t;
        std::memset(&t, 0, sizeof(t));
        t.type = SDL_GPU_TEXTURETYPE_2D;
        t.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; /* matches rgba8 shader */
        t.usage = u;
        t.width = static_cast<Uint32>(w);
        t.height = static_cast<Uint32>(h);
        t.layer_count_or_depth = 1;
        t.num_levels = 1;
        *out = SDL_CreateGPUTexture(d, &t);
        return *out != nullptr;
    };
    const SDL_GPUTextureUsageFlags scratch_usage =
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    const SDL_GPUTextureUsageFlags out_usage =
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE |
        SDL_GPU_TEXTUREUSAGE_SAMPLER; /* blit source after RCAS */
    if (!mkTex(r->width, r->height, scratch_usage, &r->fsr_up) ||
        !mkTex(r->width, r->height, out_usage, &r->fsr_out)) {
        return 0;
    }
    return 1;
}

void render_fsr_destroy(whaleui_render_t* r)
{
    if (!r) {
        return;
    }
    SDL_GPUDevice* d = r->device;
    SDL_ReleaseGPUComputePipeline(d, r->fsr_easu_pipe);
    SDL_ReleaseGPUComputePipeline(d, r->fsr_rcas_pipe);
    SDL_ReleaseGPUTexture(d, r->fsr_up);
    SDL_ReleaseGPUTexture(d, r->fsr_out);
    r->fsr_easu_pipe = nullptr;
    r->fsr_rcas_pipe = nullptr;
    r->fsr_up = nullptr;
    r->fsr_out = nullptr;
}

/* should the current frame use the FSR path? mode 0 = auto. */
int fsr_want_active(whaleui_render_t* r)
{
    if (!r->fsr_easu_pipe || !r->fsr_rcas_pipe || !r->gpu) {
        return 0; /* resources failed to create */
    }
    if (r->fsr_mode == 2) {
        return 0;
    }
    if (r->fsr_mode == 1) {
        return 1;
    }
    /* auto: 4K display, or running on battery, and the render surface is big
     * enough for the extra EASU/RCAS passes to pay off. High-DPI windows
     * are judged by their physical (pixel) size, not the logical size. */
    int pw = r->width, ph = r->height;
    SDL_GetWindowSizeInPixels(r->window, &pw, &ph);
    if (pw < 2560 && ph < 1440) {
        return 0; /* render surface too small to bother */
    }
    SDL_DisplayID did = SDL_GetDisplayForWindow(r->window);
    const SDL_DisplayMode* dm = did ? SDL_GetDesktopDisplayMode(did) : nullptr;
    bool big = dm && (dm->w >= 3840 || dm->h >= 2160);
    SDL_PowerState ps = SDL_GetPowerInfo(nullptr, nullptr);
    bool battery = ps == SDL_POWERSTATE_ON_BATTERY;
    return (big || battery) ? 1 : 0;
}

/* default clamped scroll behavior (defined with the wheel handling) */
static int scroll_default(whaleui_render_t* r, lxb_dom_element* el,
                          int delta, void* userdata);

extern "C" whaleui_render_t* whaleui_render_create(SDL_GPUDevice* device, SDL_Window* window,
                                                   int width, int height)
{
    if (!device || !window || width <= 0 || height <= 0) {
        return nullptr;
    }
    /* value-initialize: STL containers (pixels/fonts/select_index) are
     * default-constructed, scalars zeroed - never memset a struct with
     * std::map/vector members (breaks the map's sentinel node) */
    whaleui_render_t* r = new whaleui_render_t();
    r->device = device;
    r->window = window;
    r->width = width;
    r->height = height;
    r->fb_w = width;
    r->fb_h = height;
    r->has_dirty = 1;
    r->bg_color = 0xFF202020;
    r->fsr_mode = 0;   /* auto */
    r->fsr_scale = 0.5f;
    r->fsr_sharpness = 0.4f;
    r->scroll_fn = scroll_default;
    r->scroll_ud = nullptr;
    r->cursor_arrow = nullptr;
    r->cursor_text = nullptr;
    r->cursor_pointer = nullptr;
    r->anim = whaleui_anim_create();
    r->text_scale = 1.0f;
    r->partial = 0;
    r->pixels.resize(static_cast<size_t>(r->fb_w) * r->fb_h, 0xFF202020);
    r->text_layer.resize(static_cast<size_t>(r->fb_w) * r->fb_h, 0);

    /* GPU renderer: batched draw commands into an offscreen target.
     * FSR is disabled for now (its path still expects the CPU framebuffer;
     * it will be rewired to read the GPU target). */
    r->gpu = whaleui_gpu_create(device, width, height);
    if (!r->gpu) {
        delete r;
        return nullptr;
    }
    r->fsr_active = 0;
    /* FSR compute resources (created up front; auto mode may enable it) */
    render_fsr_create(r);

    /* default font: register the platform UI fonts (CJK/emoji fallback
     * chain), then open the first as default and chain fallbacks */
#ifdef WHALEUI_BUILD_FULL
    whaleui_font_register_system_defaults();
    whaleui_font_registry* reg = whaleui_font_registry_get();
    if (reg->count > 0) {
        SDL_IOStream* io = SDL_IOFromMem(const_cast<unsigned char*>(reg->fonts[0].data),
                                         reg->fonts[0].len);
        if (io) {
            r->font_default = TTF_OpenFontIO(io, true, 16.0f);
            if (r->font_default) {
                render_build_fallback(r, r->font_default, 16, 0);
            }
        }
    }
#endif
    return r;
}

extern "C" void whaleui_render_destroy(whaleui_render_t* r)
{
    if (!r) {
        return;
    }
    whaleui_layout_destroy(r->tree);
    if (g_last_tree == r->tree) {
        g_last_tree = nullptr; /* the tree this pointer referenced is gone */
    }
    whaleui_anim_destroy(r->anim);
    if (r->rules) {
        whaleui_css_rules_destroy(r->rules, r->rule_count);
    }
    whaleui_css_keyframes_destroy(&r->keyframes);
#ifdef WHALEUI_BUILD_FULL
    for (auto& f : r->fonts) {
        TTF_CloseFont(f.second);
    }
    TTF_CloseFont(r->font_default);
    for (auto& e : r->text_cache) {
        TTF_DestroyText(e.second.t);
        SDL_DestroySurface(e.second.surf);
    }
    for (auto& im : r->images) {
        SDL_DestroySurface(im.second);
    }
    if (r->cursor_arrow) {
        SDL_DestroyCursor(r->cursor_arrow);
    }
    if (r->cursor_text) {
        SDL_DestroyCursor(r->cursor_text);
    }
    if (r->cursor_pointer) {
        SDL_DestroyCursor(r->cursor_pointer);
    }
    if (r->text_engine) {
        TTF_DestroySurfaceTextEngine(r->text_engine);
    }
#endif
    whaleui_gpu_destroy(r->gpu);
    render_fsr_destroy(r);
    delete r;
}

extern "C" void whaleui_render_set_css(whaleui_render_t* r,
                                       const whaleui_css_rule_t* rules, size_t count,
                                       const whaleui_css_keyframes_t* keyframes,
                                       const std::map<std::string, std::string>* theme_vars)
{
    if (!r) {
        return;
    }
    if (r->rules) {
        whaleui_css_rules_destroy(r->rules, r->rule_count);
        r->rules = nullptr;
        r->rule_count = 0;
    }
    whaleui_css_keyframes_destroy(&r->keyframes);
    if (rules && count) {
        /* deep-copy rules so the render context owns its stylesheet */
        whaleui_css_rule_t* copy = static_cast<whaleui_css_rule_t*>(
            std::malloc(count * sizeof(*copy)));
        if (copy) {
            for (size_t i = 0; i < count; ++i) {
                std::memset(&copy[i], 0, sizeof(copy[i]));
            }
            r->rules = copy;
            r->rule_count = count;
            for (size_t i = 0; i < count; ++i) {
                copy[i].selector = strdup(rules[i].selector ? rules[i].selector : "");
                copy[i].media = rules[i].media ? strdup(rules[i].media) : nullptr;
                copy[i].important = rules[i].important;
                if (rules[i].decl_count) {
                    copy[i].decls = static_cast<char**>(std::malloc(rules[i].decl_count * sizeof(char*)));
                    for (size_t d = 0; d < rules[i].decl_count; ++d) {
                        copy[i].decls[d] = strdup(rules[i].decls[d]);
                    }
                    copy[i].decl_count = rules[i].decl_count;
                }
            }
        }
    }
    if (keyframes) {
        r->keyframes.items = static_cast<whaleui_keyframes_t*>(std::malloc(
            keyframes->count * sizeof(*keyframes->items)));
        r->keyframes.count = keyframes->count;
        for (size_t i = 0; i < keyframes->count; ++i) {
            whaleui_keyframes_t* d = &r->keyframes.items[i];
            const whaleui_keyframes_t* s = &keyframes->items[i];
            std::memset(d, 0, sizeof(*d));
            d->name = strdup(s->name);
            if (s->frame_count) {
                d->frames = static_cast<char**>(std::malloc(s->frame_count * sizeof(char*)));
                for (size_t j = 0; j < s->frame_count; ++j) {
                    d->frames[j] = strdup(s->frames[j]);
                }
                d->frame_count = s->frame_count;
            }
        }
    }
    if (theme_vars) {
        r->theme_vars = *theme_vars;
    }
    /* new stylesheet: drop stale animation state, point the engine at the
     * fresh keyframes copy */
    whaleui_anim_reset(r->anim);
    whaleui_anim_set_keyframes(r->anim, &r->keyframes);
    r->has_dirty = 1;
}

extern "C" whaleui_layout_tree_t* whaleui_render_last_tree(void)
{
    return g_last_tree;
}

extern "C" whaleui_dom_element_t* whaleui_render_hit_element(whaleui_render_t* r,
                                                             int x, int y)
{
    if (!r || !r->tree) {
        return nullptr;
    }
    fb_coords(r, x, y);
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    return hit ? reinterpret_cast<whaleui_dom_element_t*>(hit->el) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_render_focus_element(whaleui_render_t* r)
{
    return r ? reinterpret_cast<whaleui_dom_element_t*>(r->focus_el) : nullptr;
}

extern "C" void whaleui_render_reset_dom(whaleui_render_t* r)
{
    if (!r) {
        return;
    }
#ifdef WHALEUI_BUILD_FULL
    for (auto& e : r->text_cache) {
        TTF_DestroyText(e.second.t);
        SDL_DestroySurface(e.second.surf);
    }
    r->text_cache.clear();
    for (auto& im : r->images) {
        SDL_DestroySurface(im.second);
    }
    r->images.clear();
#endif
    r->open_select = nullptr;
    r->open_select_hover = 0;
    r->select_index.clear();
    r->scrolls.clear();
    r->last_scrolls.clear();
    r->hover_el = nullptr;
    r->focus_el = nullptr;
    r->pressed_el = nullptr;
    r->sel_anchor_el = nullptr;
    r->sel_focus_el = nullptr;
    r->sel_anchor = r->sel_focus = 0;
    r->selecting = 0;
    r->edit_el = nullptr;
    r->compose.clear();
    r->drag_scroll_el = nullptr;
    r->drag_scroll_node = nullptr;
    r->scroll_max_el = nullptr;
    r->wheel_node = nullptr;
    r->has_dirty = 1;
}

extern "C" void whaleui_render_invalidate(whaleui_render_t* r)
{
    if (r) {
        r->has_dirty = 1;
    }
}

extern "C" void whaleui_render_invalidate_rect(whaleui_render_t* r, int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
    if (r) {
        r->has_dirty = 1;
    }
}

extern "C" int whaleui_render_resize(whaleui_render_t* r, int width, int height)
{
    if (!r || width <= 0 || height <= 0) {
        return -1;
    }
    r->width = width;
    r->height = height;
    r->fb_w = width;
    r->fb_h = height;
    r->pixels.assign(static_cast<size_t>(width) * height, r->bg_color);
    /* the CPU text layer and the GPU render targets are size-bound too:
     * leaving them at the old size made text painting (and the layer
     * upload) index out of bounds after growing the window */
    r->text_layer.assign(static_cast<size_t>(width) * height, 0);
    SDL_ReleaseGPUTexture(r->device, r->offscreen);
    SDL_ReleaseGPUTransferBuffer(r->device, r->transfer);
    SDL_GPUTextureCreateInfo tci;
    std::memset(&tci, 0, sizeof(tci));
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    tci.width = static_cast<Uint32>(width);
    tci.height = static_cast<Uint32>(height);
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    r->offscreen = SDL_CreateGPUTexture(r->device, &tci);
    SDL_GPUTransferBufferCreateInfo tbi;
    std::memset(&tbi, 0, sizeof(tbi));
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = static_cast<Uint32>(static_cast<size_t>(width) * height * 4);
    r->transfer = SDL_CreateGPUTransferBuffer(r->device, &tbi);
    /* GPU render targets (geometry ping-pong + composite) must follow the
     * new size; recreate the renderer like the FSR toggle does */
    whaleui_gpu_destroy(r->gpu);
    r->gpu = whaleui_gpu_create(r->device, width, height);
    if (!r->gpu) {
        return -2;
    }
    render_fsr_destroy(r);
    render_fsr_create(r);
    r->fsr_active = 0;
    r->has_dirty = 1;
    /* stale scroll deltas from the old size would make the first scroll
     * after a resize shift by the accumulated difference */
    r->last_scrolls.clear();
    return 0;
}

/* find the layout node for a DOM element in the current tree */
whaleui_layout_node_t* find_node_by_el(whaleui_layout_node_t* n, lxb_dom_element* el)
{
    if (!n || !el) {
        return nullptr;
    }
    if (n->el == el) {
        return n;
    }
    for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
        whaleui_layout_node_t* hit = find_node_by_el(c, el);
        if (hit) {
            return hit;
        }
    }
    return nullptr;
}

/* window -> framebuffer coordinates (identity when FSR is off) */
static void fb_coords(whaleui_render_t* r, int& x, int& y)
{
    if (r->width > 1 && r->fb_w != r->width) {
        x = x * r->fb_w / r->width;
    }
    if (r->height > 1 && r->fb_h != r->height) {
        y = y * r->fb_h / r->height;
    }
}

extern "C" int whaleui_render_handle_click(whaleui_render_t* r, int x, int y,
                                           const char** out_value)
{
    if (out_value) {
        *out_value = nullptr;
    }
    if (!r || !r->tree) {
        return 0;
    }
    fb_coords(r, x, y);
    /* 1. clicking inside the expanded list chooses an option */
    if (r->open_select) {
        whaleui_layout_node_t* s = find_node_by_el(r->tree->root, r->open_select);
        if (!s) {
            r->open_select = nullptr;
            return 0;
        }
        std::vector<std::string> texts, values;
        select_options(s->el, texts, values);
        int soff = 0;
        for (whaleui_layout_node_t* p = s->parent; p; p = p->parent) {
            soff += scroll_delta(r, p);
        }
        int list_x = s->border.x;
        int list_y = s->border.y + soff + s->border.h;
        int list_w = s->border.w;
        if (x >= list_x && x < list_x + list_w &&
            y >= list_y && y < list_y + kSelectItemH * static_cast<int>(values.size())) {
            int idx = (y - list_y) / kSelectItemH;
            if (idx >= 0 && idx < static_cast<int>(values.size())) {
                r->select_index[r->open_select] = idx;
                r->open_select = nullptr;
                r->has_dirty = 1;
                if (out_value) {
                    static std::string chosen;
                    chosen = values[idx];
                    *out_value = chosen.c_str();
                }
                return 1;
            }
        }
        /* click outside the list closes it */
        r->open_select = nullptr;
        r->has_dirty = 1;
    }
    /* 2. clicking a <select> toggles it open */
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    if (hit && is_select_node(hit)) {
        r->open_select = hit->el;
        r->open_select_hover = 0;
        r->has_dirty = 1;
    }
    /* 3. clicking a <summary> toggles its parent <details> (collapse/
     * expand is a C++ behavior, not pure CSS). The hit may be the summary
     * itself or a run inside it: walk up to the nearest summary. */
    {
        lxb_dom_element* el = hit ? hit->el : nullptr;
        while (el && !tag_eq(el, "summary")) {
            lxb_dom_node* p = el->node.parent;
            if (!p || p->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                break;
            }
            el = lxb_dom_interface_element(p);
        }
        if (el && tag_eq(el, "summary")) {
            lxb_dom_node* par = el->node.parent;
            if (par && par->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element* det = lxb_dom_interface_element(par);
                size_t dlen = 0;
                const lxb_char_t* dname =
                    lxb_dom_element_local_name(det, &dlen);
                if (dname && dlen == 7 &&
                    std::memcmp(dname, "details", 7) == 0) {
                    if (lxb_dom_element_has_attribute(
                            det, (const lxb_char_t*)"open", 4)) {
                        lxb_dom_element_remove_attribute(
                            det, (const lxb_char_t*)"open", 4);
                    } else {
                        lxb_dom_element_set_attribute(
                            det, (const lxb_char_t*)"open", 4,
                            (const lxb_char_t*)"", 0);
                    }
                    r->has_dirty = 1; /* relayout: show/hide the body */
                }
            }
        }
    }
    /* 4. checkbox/radio toggle the checked attribute (radio is exclusive
     * within its name group, matching browser behavior) */
    if (hit && hit->el && is_check_radio(hit->el)) {
        if (lxb_dom_element_has_attribute(hit->el, (const lxb_char_t*)"checked", 7)) {
            lxb_dom_element_remove_attribute(hit->el, (const lxb_char_t*)"checked", 7);
        } else {
            lxb_dom_element_set_attribute(hit->el, (const lxb_char_t*)"checked", 7,
                                          (const lxb_char_t*)"", 0);
            size_t nlen = 0;
            const lxb_char_t* nm = lxb_dom_element_get_attribute(
                hit->el, (const lxb_char_t*)"name", 4, &nlen);
            size_t tlen = 0;
            const lxb_char_t* tname = lxb_dom_element_get_attribute(
                hit->el, (const lxb_char_t*)"type", 4, &tlen);
            bool radio = tname && tlen == 5 &&
                         std::memcmp(tname, "radio", 5) == 0;
            if (radio && nm && nlen > 0) {
                /* clear checked on every other radio with the same name */
                std::function<void(lxb_dom_node*)> uncheck =
                    [&](lxb_dom_node* nd) {
                        while (nd) {
                            if (nd->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                                lxb_dom_element* e = lxb_dom_interface_element(nd);
                                if (e != hit->el &&
                                    lxb_dom_element_has_attribute(
                                        e, (const lxb_char_t*)"checked", 7)) {
                                    size_t nn = 0;
                                    const lxb_char_t* en = lxb_dom_element_get_attribute(
                                        e, (const lxb_char_t*)"name", 4, &nn);
                                    if (en && nn == nlen &&
                                        std::memcmp(en, nm, nlen) == 0) {
                                        lxb_dom_element_remove_attribute(
                                            e, (const lxb_char_t*)"checked", 7);
                                    }
                                }
                                if (nd->first_child) {
                                    uncheck(nd->first_child);
                                }
                            }
                            nd = nd->next;
                        }
                    };
                lxb_dom_document* doc2 = hit->el->node.owner_document;
                lxb_dom_element* docroot =
                    doc2 ? lxb_dom_document_element(doc2) : nullptr;
                if (docroot) {
                    uncheck(&docroot->node);
                }
            }
        }
        r->has_dirty = 1;
    }
    return 0;
}

extern "C" void whaleui_render_set_hover(whaleui_render_t* r, int x, int y)
{
    if (!r || !r->tree) {
        return;
    }
    fb_coords(r, x, y);
    /* dragging a scrollbar: the thumb follows the mouse y. Skipping the
     * hover update keeps the layout tree stable while dragging (hover
     * changes would otherwise rebuild it every frame as content scrolls
     * under the cursor - the main scrollbar-drag stall). */
    if (r->drag_scroll_el) {
        update_drag_scroll(r, y);
        return;
    }
    /* hover inside the expanded list highlights the option under the mouse */
    if (r->open_select) {
        whaleui_layout_node_t* s = find_node_by_el(r->tree->root, r->open_select);
        if (s) {
            std::vector<std::string> texts, values;
            select_options(s->el, texts, values);
            int soff = 0;
            for (whaleui_layout_node_t* p = s->parent; p; p = p->parent) {
                soff += scroll_delta(r, p);
            }
            int list_x = s->border.x;
            int list_y = s->border.y + soff + s->border.h;
            int list_w = s->border.w;
            int hov = -1;
            if (x >= list_x && x < list_x + list_w &&
                y >= list_y && y < list_y + kSelectItemH * static_cast<int>(values.size())) {
                hov = (y - list_y) / kSelectItemH;
                if (hov >= static_cast<int>(values.size())) {
                    hov = -1;
                }
            }
            if (hov != r->open_select_hover) {
                r->open_select_hover = hov;
                r->has_dirty = 1;
            }
        }
    }
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    lxb_dom_element* el = hit ? hit->el : nullptr;
    if (el != r->hover_el) {
        r->hover_el = el;
        /* switch the system cursor: I-beam over editable text, pointer
         * over links/clickable controls, arrow otherwise */
        SDL_Cursor* want = render_cursor(r, SDL_SYSTEM_CURSOR_DEFAULT);
        if (el) {
            if (is_editable(el)) {
                want = render_cursor(r, SDL_SYSTEM_CURSOR_TEXT);
            } else {
                std::string c = sget(hit->style, "cursor");
                bool pointer = c == "pointer";
                if (!pointer) {
                    size_t tlen = 0;
                    const lxb_char_t* tname = lxb_dom_element_local_name(el, &tlen);
                    pointer = tname &&
                              ((tlen == 1 && tname[0] == 'a') ||
                               (tlen == 6 &&
                                (std::memcmp(tname, "select", 6) == 0 ||
                                 std::memcmp(tname, "button", 6) == 0)) ||
                               (tlen == 7 &&
                                std::memcmp(tname, "summary", 7) == 0));
                }
                if (pointer) {
                    want = render_cursor(r, SDL_SYSTEM_CURSOR_POINTER);
                }
            }
        }
        SDL_SetCursor(want);
        r->has_dirty = 1;
    }
    /* drag to extend a selection: gated by a 6px threshold so a plain
     * click - including the incidental hand micro-motion of pressing a
     * mouse button - never selects */
    if (r->pressed_el && r->sel_anchor_el && hit) {
        if (!r->selecting) {
            int dx = x - r->press_x;
            int dy2 = y - r->press_y;
            if (dx * dx + dy2 * dy2 < 36) {
                return;
            }
            r->selecting = 1;
        }
        update_selection_focus(r, hit, x, y);
    }
}

/* --- scrollbar dragging --- */

/* cached system cursor (created on first use) */
SDL_Cursor* render_cursor(whaleui_render_t* r, SDL_SystemCursor id)
{
    SDL_Cursor** slot = &r->cursor_arrow;
    if (id == SDL_SYSTEM_CURSOR_TEXT) {
        slot = &r->cursor_text;
    } else if (id == SDL_SYSTEM_CURSOR_POINTER) {
        slot = &r->cursor_pointer;
    }
    if (!*slot) {
        *slot = SDL_CreateSystemCursor(id);
    }
    return *slot;
}

/* nearest scrollable box whose scrollbar track contains (x, y); walks up
 * from the hit node. NULL when the click is not on a scrollbar. */
whaleui_layout_node_t* scrollbar_under(whaleui_render_t* r,
                                       whaleui_layout_node_t* hit, int x,
                                       int y)
{
    for (whaleui_layout_node_t* n = hit; n; n = n->parent) {
        if (!n->el || n->is_text || n->scroll_max <= 0) {
            continue;
        }
        int off = 0;
        for (whaleui_layout_node_t* p = n->parent; p; p = p->parent) {
            off += scroll_delta(r, p);
        }
        const int bw = 8;
        if (x >= n->border.x + n->border.w - bw &&
            x < n->border.x + n->border.w &&
            y >= n->border.y + off &&
            y < n->border.y + off + n->border.h) {
            return n;
        }
    }
    return nullptr;
}

/* move the dragged scrollbar so the thumb center follows the mouse y */
void update_drag_scroll(whaleui_render_t* r, int y)
{
    if (!r->drag_scroll_el || !r->tree) {
        return;
    }
    /* layout tree is stable while dragging (hover updates are skipped), so
     * the node is cached and only re-resolved after a rebuild */
    whaleui_layout_node_t* sc = r->drag_scroll_node;
    if (!sc || sc->el != r->drag_scroll_el) {
        sc = find_node_by_el(r->tree->root, r->drag_scroll_el);
        r->drag_scroll_node = sc;
    }
    if (!sc || sc->scroll_max <= 0) {
        r->drag_scroll_el = nullptr;
        r->drag_scroll_node = nullptr;
        return;
    }
    int off = 0;
    for (whaleui_layout_node_t* p = sc->parent; p; p = p->parent) {
        off += scroll_delta(r, p);
    }
    const int bw = 8;
    int track_x = sc->border.x + sc->border.w - bw;
    int track_y = sc->border.y + off;
    int track_h = sc->border.h;
    int content_h = sc->scroll_max + sc->content.h;
    if (content_h <= 0 || track_h <= 0) {
        return;
    }
    int thumb_h = track_h * sc->content.h / content_h;
    if (thumb_h < 10) {
        thumb_h = 10;
    }
    if (thumb_h > track_h) {
        thumb_h = track_h;
    }
    int range = track_h - thumb_h;
    if (range <= 0) {
        return;
    }
    int nv = (y - track_y - thumb_h / 2) * sc->scroll_max / range;
    if (nv > sc->scroll_max) {
        nv = sc->scroll_max;
    }
    if (nv < 0) {
        nv = 0;
    }
    if (nv != r->scrolls[sc->el]) {
        r->scrolls[sc->el] = nv;
        r->scroll_dirty = 1;
    }
}

extern "C" void whaleui_render_set_pressed(whaleui_render_t* r, int x, int y,
                                           int down)
{
    if (!r || !r->tree) {
        return;
    }
    fb_coords(r, x, y);
    if (down) {
        whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
        lxb_dom_element* el = hit ? hit->el : nullptr;
        r->press_x = x;
        r->press_y = y;
        r->selecting = 0;
        /* scrollbar drag takes priority over selection/caret */
        whaleui_layout_node_t* sc = scrollbar_under(r, hit, x, y);
        if (sc) {
            r->drag_scroll_el = sc->el;
            r->drag_scroll_node = sc; /* valid until the next rebuild */
            update_drag_scroll(r, y);
            r->pressed_el = el;
            r->focus_el = el;
            r->has_dirty = 1;
            return;
        }
        if (el && is_editable(el)) {
            /* focus the editable control and place the caret */
            r->edit_el = el;
            r->sel_anchor_el = r->sel_focus_el = el;
            r->sel_anchor = r->sel_focus =
                static_cast<int>(caret_from_point(r, el, hit, x, y));
            SDL_StartTextInput(r->window);
        } else if (hit && hit->is_text) {
            /* anchor a potential selection (only drags extend it) */
            if (r->edit_el) {
                SDL_StopTextInput(r->window);
                r->edit_el = nullptr;
            }
            r->compose.clear();
            r->sel_anchor_el = r->sel_focus_el = hit->el;
            r->sel_anchor = r->sel_focus =
                static_cast<int>(byte_at_node(r, hit, x, y));
        } else {
            /* click elsewhere: drop the selection + editing focus */
            if (r->edit_el) {
                SDL_StopTextInput(r->window);
                r->edit_el = nullptr;
            }
            r->compose.clear();
            r->sel_anchor_el = r->sel_focus_el = nullptr;
            r->sel_anchor = r->sel_focus = 0;
        }
        r->pressed_el = el;
        r->focus_el = el;
    } else {
        /* mouse up: a selection survives only when it was actually dragged.
         * A plain click (press+release without crossing the threshold, even
         * with incidental micro-motion) leaves nothing selected. The caret
         * of a focused editable control is kept as-is. */
        r->pressed_el = nullptr;
        r->drag_scroll_el = nullptr;
        r->drag_scroll_node = nullptr;
        if (!r->selecting && !(r->edit_el && r->sel_anchor_el == r->edit_el)) {
            r->sel_anchor_el = r->sel_focus_el = nullptr;
            r->sel_anchor = r->sel_focus = 0;
        }
        r->selecting = 0;
    }
    r->has_dirty = 1;
}

extern "C" void whaleui_render_handle_wheel(whaleui_render_t* r, int x, int y,
                                            float dy)
{
    if (!r || !r->tree) {
        return;
    }
    fb_coords(r, x, y);
    /* wheel units: discrete notches come as small integers (1-3), touchpad
     * precision scrolling as larger pixel deltas. Scale only the notches so
     * both input types move a similar distance per event. */
    const float notch = 40.0f;
    float dpx = (dy >= -4.0f && dy <= 4.0f) ? dy * notch : dy;
    /* wheel-down (dy<0) must INCREASE scroll_y: delta = -dpx */
    const int delta = -static_cast<int>(dpx);

    auto do_scroll = [r, delta](whaleui_layout_node_t* sc) {
        if (!sc->el) {
            return;
        }
        /* the scroll behavior hook owns position updates (default clamps
         * to [0, scroll_max]); a custom hook may animate instead */
        if (r->scroll_fn(r, sc->el, delta, r->scroll_ud)) {
            r->scroll_dirty = 1;
        }
    };

    /* hit-test once per pointer position: wheel bursts (rapid scrolling
     * without moving the mouse) keep hitting the same scroll container, so
     * the full-tree walk is skipped on repeated coordinates */
    whaleui_layout_node_t* hit = nullptr;
    if (r->wheel_node && r->wheel_x == x && r->wheel_y == y) {
        hit = r->wheel_node;
    } else {
        hit = hit_test(r, r->tree->root, x, y, 0);
        r->wheel_node = hit;
        r->wheel_x = x;
        r->wheel_y = y;
    }
    /* nearest scrollable ancestor (the hit element itself included).
     * Text runs inherit their parent's overflow but are not scroll
     * containers: skip them so the walk reaches the actual box. */
    for (whaleui_layout_node_t* n = hit; n; n = n->parent) {
        if (!n->el || n->is_text) {
            continue;
        }
        std::string ov = sget(n->style, "overflow");
        if (ov == "auto" || ov == "scroll") {
            do_scroll(n);
            return;
        }
    }
    /* nothing explicitly scrollable: scroll the page (html root) */
    do_scroll(r->tree->root);
}

/* default scroll behavior: add delta to the element's scroll_y, clamped to
 * the content range [0, scroll_max]. (Smooth/eased scrolling was tried via
 * scroll targets + per-frame easing and backed out - it made wheel input
 * unreliable; revisit with a proper velocity model later.) */
static int scroll_default(whaleui_render_t* r, lxb_dom_element* el,
                          int delta, void* userdata)
{
    (void)userdata;
    if (!el || !r->tree) {
        return 0;
    }
    int max = 0;
    if (r->scroll_max_el == el) {
        max = r->scroll_max_cache; /* wheel bursts hit the same element */
    } else {
        whaleui_layout_node_t* n = find_node_by_el(r->tree->root, el);
        max = n ? n->scroll_max : 0;
        r->scroll_max_el = el;
        r->scroll_max_cache = max;
    }
    int& cur = r->scrolls[el];
    int nv = cur + delta;
    if (nv > max) {
        nv = max;
    }
    if (nv < 0) {
        nv = 0;
    }
    if (nv != cur) {
        cur = nv;
        return 1;
    }
    return 0;
}

extern "C" int whaleui_render_set_scroll_behavior(whaleui_render_t* r,
                                                  whaleui_scroll_behavior_fn fn,
                                                  void* userdata)
{
    if (!r) {
        return -1;
    }
    r->scroll_fn = fn ? fn : scroll_default;
    r->scroll_ud = userdata;
    return 0;
}

extern "C" void whaleui_render_handle_key(whaleui_render_t* r, int keycode,
                                          int pressed, int mods)
{
    if (!r || !pressed) {
        return;
    }
    edit_key(r, keycode, mods);
}

extern "C" void whaleui_render_handle_text(whaleui_render_t* r, const char* utf8)
{
    if (!r || !utf8 || !*utf8 || !r->edit_el) {
        return;
    }
    int a = r->sel_anchor, b = r->sel_focus;
    if (a > b) {
        std::swap(a, b);
    }
    edit_replace(r, r->edit_el, static_cast<size_t>(a), static_cast<size_t>(b),
                 std::string(utf8));
}

extern "C" void whaleui_render_handle_editing(whaleui_render_t* r,
                                              const char* utf8)
{
    if (!r) {
        return;
    }
    r->compose = utf8 ? utf8 : "";
    r->has_dirty = 1;
}

extern "C" void whaleui_render_set_fsr(whaleui_render_t* r, int mode,
                                       float scale, float sharpness)
{
    if (!r) {
        return;
    }
    r->fsr_mode = (mode < 0 || mode > 2) ? 0 : mode;
    r->fsr_scale = scale > 0.0f ? scale : 0.5f;
    r->fsr_sharpness = sharpness < 0.0f ? 0.0f : (sharpness > 1.0f ? 1.0f : sharpness);
    r->has_dirty = 1;
}

extern "C" int whaleui_render_frame(whaleui_render_t* r, whaleui_dom_document_t* doc)
{
    if (!r || !doc) {
        return -1;
    }
    /* advance all running animations and collect the current values (the
     * paint-only fast path below applies them without relaying out) */
    const uint64_t now = SDL_GetTicks();
    const bool animating = r->anim && whaleui_anim_tick(r->anim, now) != 0;
    /* skip the whole frame when nothing changed: idle frames cost ~0.
     * Repaint when the layout/state is dirty, a wheel scroll happened, an
     * animation/transition is running, or an editable caret is blinking. */
    if (!r->has_dirty && r->tree && !r->scroll_dirty &&
        !animating && !r->edit_el) {
        return 0;
    }
    r->scroll_dirty = 0;
    /* FSR decision: auto mode watches display size + power state, so it can
     * flip at runtime; switching resolution rebuilds the framebuffer */
    {
        int want = fsr_want_active(r);
        if (want != r->fsr_active) {
            r->fsr_active = want;
            const float scale = r->fsr_scale > 0.0f ? r->fsr_scale : 0.5f;
            r->fb_w = want ? static_cast<int>(r->width * scale) : r->width;
            r->fb_h = want ? static_cast<int>(r->height * scale) : r->height;
            if (r->fb_w < 1) { r->fb_w = 1; }
            if (r->fb_h < 1) { r->fb_h = 1; }
            r->pixels.assign(static_cast<size_t>(r->fb_w) * r->fb_h, r->bg_color);
            r->text_layer.assign(static_cast<size_t>(r->fb_w) * r->fb_h, 0);
            /* the GPU targets follow the render resolution */
            whaleui_gpu_destroy(r->gpu);
            r->gpu = whaleui_gpu_create(r->device, r->fb_w, r->fb_h);
            render_fsr_destroy(r);
            render_fsr_create(r);
            r->has_dirty = 1;
        }
    }
    /* layout rebuild needed when the document is dirty OR an animation
     * touches a layout-affecting property (width/margin/font-size/...).
     * Paint-only animations (opacity/transform/colors) skip the rebuild:
     * the tick's values are applied straight onto the tree and only the
     * opacity chain is recomputed - the bulk of the per-frame cost
     * (style cascade + box layout) is gone. */
    const bool need_layout = r->has_dirty || !r->tree ||
                             (animating && whaleui_anim_needs_layout(r->anim));
    if (need_layout) {
        whaleui_layout_destroy(r->tree);
        whaleui_style_state st;
        st.hover = r->hover_el;
        st.focus = r->focus_el;
        st.pressed = r->pressed_el;
        r->tree = whaleui_layout_compute(doc, r->rules, r->rule_count,
                                         &r->theme_vars, r->fb_w, r->fb_h,
                                         &st, &r->scrolls, r->anim,
                                         r->text_scale);
        g_last_tree = r->tree; /* DOM geometry queries see this frame */
        r->has_dirty = 0;
        r->bounds_valid = 0; /* subtree paint bounds are stale */
        r->drag_scroll_node = nullptr; /* layout nodes were recreated */
        r->scroll_max_el = nullptr;    /* scroll_max may have changed */
        r->wheel_node = nullptr;       /* hit cache is stale */
        /* the viewport may have changed size (resize/fullscreen): clamp
         * every live scroll position to its new range so scrollbars and
         * content don't sit past the end */
        std::function<void(whaleui_layout_node_t*)> clamp_sc =
            [&](whaleui_layout_node_t* nd) {
                if (nd->el) {
                    auto it = r->scrolls.find(nd->el);
                    if (it != r->scrolls.end()) {
                        if (it->second > nd->scroll_max) {
                            it->second = nd->scroll_max;
                        }
                        if (it->second < 0) {
                            it->second = 0;
                        }
                    }
                }
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    clamp_sc(c);
                }
            };
        clamp_sc(r->tree->root);
    } else if (animating) {
        /* paint-only animation: apply the tick's values to the tree styles
         * and refresh the cascaded opacity (children inherit it) */
        std::function<void(whaleui_layout_node_t*)> apply_ov =
            [&](whaleui_layout_node_t* nd) {
                if (nd->el && whaleui_anim_has_el(r->anim, nd->el)) {
                    whaleui_anim_apply_ov(r->anim, nd->el, nd->style);
                }
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    apply_ov(c);
                }
            };
        apply_ov(r->tree->root);
        std::function<void(whaleui_layout_node_t*, float)> re_op =
            [&](whaleui_layout_node_t* nd, float po) {
                if (nd->is_text) {
                    nd->opacity = po;
                    return;
                }
                float o = 1.0f;
                std::string ov2 = sget(nd->style, "opacity");
                if (!ov2.empty()) {
                    o = std::strtof(ov2.c_str(), nullptr);
                    if (o < 0.0f) {
                        o = 0.0f;
                    }
                    if (o > 1.0f) {
                        o = 1.0f;
                    }
                }
                nd->opacity = o * po;
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    re_op(c, nd->opacity);
                }
            };
        re_op(r->tree->root, 1.0f);
    }
    if (!r->tree) {
        return -2;
    }

    /* pure scroll: shift the previous image (ping-pong blit on the GPU
     * side + memmove of the text layer) and only repaint the exposed strip.
     * Any other dirty/animations fall back to a full repaint (dy=0). */
    int scroll_dy = 0;
    if (!r->has_dirty && !r->scroll_dirty && !animating && !r->edit_el &&
        r->tree) {
        std::map<lxb_dom_element*, int> cur;
        std::function<void(whaleui_layout_node_t*)> collect =
            [&](whaleui_layout_node_t* nd) {
                if (nd->scroll_max > 0 && nd->el) {
                    auto it = r->scrolls.find(nd->el);
                    if (it != r->scrolls.end()) {
                        cur[nd->el] = it->second;
                    }
                }
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    collect(c);
                }
            };
        collect(r->tree->root);
        for (auto& kv : cur) {
            auto it = r->last_scrolls.find(kv.first);
            if (it != r->last_scrolls.end()) {
                scroll_dy += kv.second - it->second;
            }
        }
        r->last_scrolls = std::move(cur);
    }

    /* paint: collect batched GPU draw commands. Text goes to the CPU layer
     * (moved on scroll), uploaded + composited by a compute pass. */
    Clip full = {0, 0, r->fb_w, r->fb_h};
    Clip strip = full;
    if (scroll_dy > 0) { /* content moved up: bottom strip is new */
        strip.y = r->fb_h - scroll_dy;
        strip.h = scroll_dy < r->fb_h ? scroll_dy : r->fb_h;
        std::fill(r->text_layer.begin() + static_cast<size_t>(strip.y) * r->fb_w,
                  r->text_layer.end(), 0);
    } else if (scroll_dy < 0) { /* content moved down: top strip is new */
        strip.y = 0;
        strip.h = -scroll_dy;
        std::fill(r->text_layer.begin(),
                  r->text_layer.begin() + static_cast<size_t>(strip.h) * r->fb_w,
                  0);
    }
    if (scroll_dy != 0) {
        /* shift the existing text layer rows (opposite of the scroll) */
        int rows = r->fb_h - (scroll_dy < 0 ? -scroll_dy : scroll_dy);
        if (scroll_dy > 0) { /* content up: row y takes row y+dy */
            for (int y = 0; y < rows; ++y) {
                std::memcpy(&r->text_layer[static_cast<size_t>(y) * r->fb_w],
                            &r->text_layer[static_cast<size_t>(y + scroll_dy) * r->fb_w],
                            static_cast<size_t>(r->fb_w) * 4);
            }
        } else { /* content down: row y-dy takes row y */
            for (int y = rows - 1; y >= 0; --y) {
                std::memcpy(&r->text_layer[static_cast<size_t>(y - scroll_dy) * r->fb_w],
                            &r->text_layer[static_cast<size_t>(y) * r->fb_w],
                            static_cast<size_t>(r->fb_w) * 4);
            }
        }
    }
    bool partial = false; /* load-only repaint of a dirty region */
    if (scroll_dy != 0) {
        /* scroll strip handled above */
    } else if (animating && !need_layout && !r->has_dirty && !r->edit_el) {
        /* paint-only animation: repaint only the animating elements'
         * bounding boxes (dirty-rect, keeps the rest of the frame) */
        std::function<void(whaleui_layout_node_t*)> acc =
            [&](whaleui_layout_node_t* nd) {
                if (nd->el && whaleui_anim_has_el(r->anim, nd->el)) {
                    int x0 = nd->border.x, y0 = nd->border.y;
                    int x1 = x0 + nd->border.w, y1 = y0 + nd->border.h;
                    /* widen by the transform translation */
                    std::string tv = sget(nd->style, "transform");
                    if (!tv.empty() && tv != "none") {
                        whaleui_transform_t tf;
                        if (whaleui_transform_eval(
                                tv.c_str(), static_cast<float>(nd->border.w),
                                static_cast<float>(nd->border.h), &tf) == 0) {
                            x0 -= static_cast<int>(tf.tx > 0 ? tf.tx : -tf.tx);
                            y0 -= static_cast<int>(tf.ty > 0 ? tf.ty : -tf.ty);
                            x1 += static_cast<int>(tf.tx > 0 ? tf.tx : -tf.tx);
                            y1 += static_cast<int>(tf.ty > 0 ? tf.ty : -tf.ty);
                        }
                    }
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    if (x1 > r->fb_w) x1 = r->fb_w;
                    if (y1 > r->fb_h) y1 = r->fb_h;
                    if (x1 > x0 && y1 > y0) {
                        dirty_rect(x0, y0, x1, y1, &strip);
                    }
                }
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    acc(c);
                }
            };
        acc(r->tree->root);
        if (strip.w > 0 && strip.h > 0) {
            partial = true;
        }
    }
    if (partial) {
        /* clear only the dirty text-layer region */
        for (int yy = strip.y; yy < strip.y + strip.h; ++yy) {
            std::fill(r->text_layer.begin() +
                          static_cast<size_t>(yy) * r->fb_w + strip.x,
                      r->text_layer.begin() +
                          static_cast<size_t>(yy) * r->fb_w + strip.x + strip.w,
                      0);
        }
    } else if (scroll_dy == 0) {
        std::fill(r->text_layer.begin(), r->text_layer.end(), 0);
    }
    g_gpu = r->gpu;
    g_backdrop_active = (scroll_dy == 0 && !partial) ? 1 : 0;
    int sel_lo = 0, sel_hi = 0;
    if (!r->bounds_valid) {
        compute_paint_bounds(r->tree->root);
        r->bounds_valid = 1;
    }
    const Clip* paint_clip = (partial || scroll_dy != 0) ? &strip : &full;
    sel_seq(r, &sel_lo, &sel_hi, paint_clip);
    int seq = 0;
    paint_node(r, r->tree->root, 0, 0, seq, sel_lo, sel_hi, paint_clip,
               false);

    /* expanded select list is drawn last (highest z) so later siblings and
     * other content cannot cover it; its position follows the select's
     * scroll-offset ancestors */
    if (r->open_select) {
        whaleui_layout_node_t* s = find_node_by_el(r->tree->root, r->open_select);
        if (s) {
            int soff = 0;
            for (whaleui_layout_node_t* p = s->parent; p; p = p->parent) {
                soff += scroll_delta(r, p);
            }
            paint_select_list(r, s, soff, &full);
        }
    }
    g_gpu = nullptr;

    /* upload the text layer (cleared to zero before painting; the compute
     * pass composites it over the geometry). Dirty-rect repaints upload
     * only the painted region - the rest of the layer is unchanged. */
    if (partial) {
        whaleui_gpu_text_layer(r->gpu, r->text_layer.data(), r->fb_w, r->fb_h,
                               strip.x, strip.y, strip.w, strip.h);
    } else {
        whaleui_gpu_text_layer(r->gpu, r->text_layer.data(), r->fb_w, r->fb_h,
                               0, 0, r->fb_w, r->fb_h);
    }

    /* present: one batched render pass into the offscreen target, then a
     * single blit to the swapchain - no per-element GPU round-trips */
    SDL_GPUCommandBuffer* cmd =
        whaleui_gpu_flush(r->gpu, r->fb_w, r->fb_h, r->bg_color, scroll_dy,
                          partial ? 1 : 0);
    if (!cmd) {
        return -3;
    }
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 sw = 0, sh = 0;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, r->window, &swapchain, &sw, &sh) || !swapchain) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return 0;
    }
    /* FSR 1.0 upscale: EASU (target2 low-res -> fsr_up) + RCAS sharpen
     * (fsr_up -> fsr_out), then blit fsr_out. Both passes read their input
     * as compute storage (same as the pre-GPU path, but the input is now
     * the GPU render target instead of an uploaded CPU framebuffer). */
    SDL_GPUTexture* blit_src = r->gpu->target2;
    Uint32 blit_w = static_cast<Uint32>(r->fb_w);
    Uint32 blit_h = static_cast<Uint32>(r->fb_h);
    if (r->fsr_active && r->fsr_easu_pipe && r->fsr_rcas_pipe && r->gpu) {
        auto runPass = [&](SDL_GPUComputePipeline* p, SDL_GPUTexture* rw,
                           SDL_GPUTexture* ro, const void* pc, Uint32 pcSz) {
            SDL_GPUStorageTextureReadWriteBinding rwBind;
            std::memset(&rwBind, 0, sizeof(rwBind));
            rwBind.texture = rw;
            rwBind.mip_level = 0;
            rwBind.layer = 0;
            rwBind.cycle = false;
            SDL_GPUComputePass* cps =
                SDL_BeginGPUComputePass(cmd, &rwBind, 1, nullptr, 0);
            if (cps) {
                SDL_BindGPUComputePipeline(cps, p);
                SDL_BindGPUComputeStorageTextures(cps, 0, &ro, 1);
                SDL_PushGPUComputeUniformData(cmd, 0, pc, pcSz);
                SDL_DispatchGPUCompute(cps,
                                       (static_cast<Uint32>(r->width) + 7) / 8,
                                       (static_cast<Uint32>(r->height) + 7) / 8,
                                       1);
                SDL_EndGPUComputePass(cps);
            }
        };
        float pf[4] = {static_cast<float>(r->fb_w), static_cast<float>(r->fb_h),
                       static_cast<float>(r->fb_w) / static_cast<float>(r->width),
                       static_cast<float>(r->fb_h) / static_cast<float>(r->height)};
        runPass(r->fsr_easu_pipe, r->fsr_up, r->gpu->target2, pf, sizeof(pf));
        float rp[4] = {r->fsr_sharpness, 0, 0, 0};
        runPass(r->fsr_rcas_pipe, r->fsr_out, r->fsr_up, rp, sizeof(rp));
        blit_src = r->fsr_out;
        blit_w = static_cast<Uint32>(r->width);
        blit_h = static_cast<Uint32>(r->height);
    }
    SDL_GPUBlitInfo blit;
    std::memset(&blit, 0, sizeof(blit));
    blit.source.texture = blit_src;
    blit.source.w = blit_w;
    blit.source.h = blit_h;
    blit.destination.texture = swapchain;
    blit.destination.w = sw;
    blit.destination.h = sh;
    blit.load_op = SDL_GPU_LOADOP_CLEAR;
    blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    blit.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &blit);
    SDL_SubmitGPUCommandBuffer(cmd);
    return 0;
}

