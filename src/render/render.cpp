/* Renderer: CPU paint into an RGBA framebuffer, uploaded to an offscreen GPU
 * texture and blitted to the swapchain (SDL built-in blit pipeline; custom
 * shaders are a later step). Text comes from SDL3_ttf using fonts registered
 * through the font module. */

#include "render/render.h"
#include "render/fsr_shaders.h"
#include "render/fsr_dxil.h"
#include "font/font.h"
#include "style/style.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#ifdef WHALEUI_BUILD_FULL
#include <SDL3_ttf/SDL_ttf.h>
#else
/* stb_truetype text backend for lite/minimal builds (header-only) */
#define STB_TRUETYPE_IMPLEMENTATION
#endif
#include <stb/stb_truetype.h>

#include <lexbor/dom/dom.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace {

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
                       unsigned int color, const Clip* clip)
{
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
 * the default font */
TTF_Font* render_open_font(whaleui_render_t* r, const std::string& family, int size,
                           bool bold, bool use_cache)
{
    if (size <= 0) {
        size = 16;
    }
    std::string key = family + "|" + std::to_string(size) + "|" + (bold ? "b" : "n");
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
    if (font && bold) {
        TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    }
    if (use_cache && font) {
        r->fonts.emplace_back(key, font);
    }
    return font;
}

/* attach every other registered font as a fallback (same size) so glyphs
 * missing from `font` (CJK, emoji, ...) resolve through the library */
void render_build_fallback(whaleui_render_t* r, TTF_Font* font, int size, bool bold)
{
    if (!font) {
        return;
    }
    if (size <= 0) {
        size = 16;
    }
    whaleui_font_registry* reg = whaleui_font_registry_get();
    for (size_t i = 0; i < reg->count; ++i) {
        TTF_Font* fb = render_open_font(r, reg->fonts[i].family, size, bold, true);
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
                          bool bold)
{
    if (size <= 0) {
        size = 16;
    }
    std::string key = family + "|" + std::to_string(size) + "|" + (bold ? "b" : "n");
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
        font = render_open_font(r, fam, size, bold, false);
        if (font) {
            break;
        }
    }
    if (!font) {
        /* nothing matched: use the default font */
        if (bold && r->font_default) {
            TTF_SetFontStyle(r->font_default, TTF_STYLE_BOLD);
        }
        font = r->font_default;
    }
    if (font) {
        render_build_fallback(r, font, size, bold);
        r->fonts.emplace_back(key, font);
    }
    return font;
}
#else /* !WHALEUI_BUILD_FULL: text rendering needs SDL3_ttf (full only).
         stb_font text lands with the lite/minimal font path. */
TTF_Font* render_get_font(whaleui_render_t*, const std::string&, int, bool) { return nullptr; }
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
    TTF_Font* font = render_get_font(r, family, fs, bold);
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
    TTF_Font* font = render_get_font(r, family, fs, bold);
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
    if (px >= tw) {
        px = tw > 0 ? tw - 1 : 0;
    }
    if (py >= th) {
        py = th > 0 ? th - 1 : 0;
    }
    TTF_SubString sub;
    size_t off = text.size();
    if (TTF_GetTextSubStringForPoint(t, static_cast<float>(px),
                                     static_cast<float>(py), &sub)) {
        off = static_cast<size_t>(sub.offset);
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

/* render one text string inside the box (bx,by,bw,bh). align: 0=left,
 * 1=center, 2=right. Shared by text runs and <select> controls.
 * ckey: element to cache the TTF_Text against (NULL = no caching). */
void draw_text_at(whaleui_render_t* r, const std::string& text,
                  int bx, int by, int bw, int bh,
                  int fs, const std::string& family, unsigned int color,
                  bool bold, int align, lxb_dom_element* ckey,
                  const Clip* clip)
{
#ifdef WHALEUI_BUILD_FULL
    if (fs <= 0) {
        fs = 16;
    }
    TTF_Font* font = render_get_font(r, family, fs, bold);
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
    TTF_Text* t = nullptr;
    bool owned = true;
    std::string cache_key;
    if (ckey) {
        /* reuse the cached text object; recreate when the style changed
         * (key) or the content changed (TTF_Text is immutable) */
        char key[96];
        std::snprintf(key, sizeof(key), "%p|%d|%c|%s",
                      static_cast<void*>(ckey), fs, bold ? 'b' : 'n',
                      family.c_str());
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
         * function has no global side effects on other text */
        int ty = by + (bh - th) / 2;
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
            }
        } else {
            surf = SDL_CreateSurface(tw, th, SDL_PIXELFORMAT_RGBA8888);
            if (surf) {
                SDL_FillSurfaceRect(surf, nullptr, 0);
                TTF_DrawSurfaceText(t, 0, 0, surf);
            }
        }
        if (surf) {
            blend_surface(r->pixels, r->fb_w, r->fb_h, surf, tx, ty, clip,
                          &color);
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
                for (int pass = 0; pass < (bold ? 2 : 1); ++pass) {
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

/* pre-order sequence numbers of the anchor/focus layout nodes, computed by
 * walking the tree EXACTLY like paint_node does (same visibility skip, same
 * clipped-subtree skip, same scroll offsets), so the sequence assigned
 * during painting matches. -1 when same-element or collapsed. */
void sel_seq(whaleui_render_t* r, int* lo, int* hi)
{
    *lo = *hi = -1;
    lxb_dom_element* sa = r->sel_anchor_el;
    lxb_dom_element* sf = r->sel_focus_el;
    if (!sa || !sf || sa == sf || !r->tree) {
        return;
    }
    int idx = 0;
    std::function<void(whaleui_layout_node_t*, int)> walk =
        [&](whaleui_layout_node_t* n, int off_y) {
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
                walk(c, child_off);
            }
        };
    walk(r->tree->root, 0);
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

/* paint-time scroll offset of a scrollable box: current scrolls minus the
 * value baked into the (possibly stale) layout tree. Zero when the tree is
 * fresh, so scrolling never needs a relayout. */
int scroll_delta(whaleui_render_t* r, whaleui_layout_node_t* n)
{
    if (n->scroll_max > 0 && n->el) {
        auto it = r->scrolls.find(n->el);
        if (it != r->scrolls.end()) {
            return it->second - n->scroll_y;
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
void paint_text_selection(whaleui_render_t* r, whaleui_layout_node_t* n,
                          int fs, const std::string& family, bool bold,
                          int off_y, int seq, int sel_lo, int sel_hi,
                          const Clip* clip)
{
        size_t a = 0, b = 0;
    int tw = 0, th = 0;
    text_size(r, n->text, fs, family, bold, &tw, &th);
    if (th <= 0) {
        return;
    }
    int tx = 0, ty = 0;
    text_origin(r, n, n->text, fs, family, bold, &tx, &ty);
    ty += off_y;
    std::vector<TRect> rects = sel_rects(r, n->text, fs, family, bold, a, b);
    expand_hl_rects(rects, text_line_h(r, fs, family, bold));
    unsigned int hl = accent_hl(r, 0x3C);
    for (size_t i = 0; i < rects.size(); ++i) {
        fill_rect(r->pixels, r->fb_w, r->fb_h,
                  tx + rects[i].x, ty + rects[i].y,
                  rects[i].w, rects[i].h, hl, clip);
    }
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

/* editable controls: input paints its value text (always); when focused, a
 * selection highlight + caret + IME composition overlay are drawn on top.
 * textarea/contenteditable glyphs come from the layout text run - this only
 * adds the interaction layer. */
void paint_editable(whaleui_render_t* r, whaleui_layout_node_t* n,
                    int off_y, const Clip* clip)
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
    std::string val = edit_value(el);
    bool focused = (el == r->edit_el);
    /* caret/highlight geometry: input centers in its content box; textarea/
     * contenteditable follow the layout text run so they line up with the
     * painted glyphs (which sit at the run's top, not the box center) */
    whaleui_layout_node_t* geo = editable_geo(n);
    int tx = 0, ty = 0;
    text_origin(r, geo, val, fs, family, bold, &tx, &ty);
    ty += off_y;
    int th = text_line_h(r, fs, family, bold);

    if (tag_eq(el, "input")) {
        /* input has no text children: paint the value here, always */
        unsigned int fg = color_of(n->style, "color", 0xFF1a1a1a);
        draw_text_at(r, val, n->content.x, n->content.y,
                     n->content.w, n->content.h,
                     fs, family, fg, bold, 0, el, clip);
    }
    if (focused) {
        size_t a = 0, b = 0;
        if (sel_range_for(r, el, val.size(), &a, &b, -1, -1, -1)) {
            std::vector<TRect> rects = sel_rects(r, val, fs, family, bold, a, b);
            expand_hl_rects(rects, text_line_h(r, fs, family, bold));
            unsigned int hl = accent_hl(r, 0x3C);
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
        paint_caret(r, tx, ty, val, fs, family, bold, caret, clip);
        if (!r->compose.empty()) {
            unsigned int fg = color_of(n->style, "color", 0xFF1a1a1a);
            unsigned int comp = (0xC8 << 24) | (fg & 0x00FFFFFF);
            int cxx = 0, cyy = 0, chh = 16;
            caret_pos(r, val, fs, family, bold, caret, &cxx, &cyy, &chh);
            draw_text_at(r, r->compose, tx + cxx, ty + cyy, 300, chh,
                         fs, family, comp, bold, 0, el, clip);
        }
    }
}

/* vertical scrollbar for scrollable containers and the page (html root).
 * Painted over the content on the container's right edge; thumb height
 * tracks the visible fraction, position tracks scroll_y/scroll_max. */
void paint_scrollbar(whaleui_render_t* r, whaleui_layout_node_t* n,
                     int off_y, const Clip* clip)
{
    if (n->scroll_max <= 0 || n->border.h < 24) {
        return;
    }
    const int bw = 8;
    int track_x = n->border.x + n->border.w - bw;
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
    int sy = scroll_delta(r, n) + n->scroll_y;
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

void paint_text(whaleui_render_t* r, whaleui_layout_node_t* n, int off_y,
                int seq, int sel_lo, int sel_hi, const Clip* clip)
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
    /* font-weight: bold/bolder/600-900 render bold */
    std::string fw = sget(n->style, "font-weight");
    bool bold = fw == "bold" || fw == "bolder" ||
                (!fw.empty() && std::atoi(fw.c_str()) >= 600);
    /* text-align aligns within the parent element's content box */
    std::string ta = sget(n->style, "text-align");
    int align = 0;
    if (ta == "center") {
        align = 1;
    } else if (ta == "right") {
        align = 2;
    }
    whaleui_layout_node_t* box = n;
    if (n->parent && !n->parent->is_text) {
        box = n->parent;
    }
    paint_text_selection(r, n, fs, family, bold, off_y, seq, sel_lo, sel_hi,
                         clip);
    /* the run's own box carries the scroll shift; draw_text_at centers the
     * glyphs in it, matching text_origin's hit/highlight geometry */
    draw_text_at(r, n->text, box->content.x, n->border.y + off_y,
                 box->content.w, n->border.h,
                 fs, family, color, bold, align, n->el, clip);
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
    /* value text left, arrow pinned to the right edge. Both are nudged up
     * 2px: segoe's glyphs sit ~2px below the line-box center, so metric
     * centering alone leaves the text looking low inside the control. */
    int arrow_x = n->content.x + n->content.w - 16;
    int text_w = arrow_x - n->content.x - 8;
    if (text_w < 10) {
        text_w = 10;
    }
    int vy = n->content.y + off_y - 2;
    draw_text_at(r, texts[sel], n->content.x + 2, vy,
                 text_w, n->content.h, fs, "", fg, false, 0, n->el, clip);
    /* large solid triangle ¨‹: the small ? is only a few px tall and looks
     * like it floats above the text baseline */
    draw_text_at(r, "\xe2\x96\xbc", arrow_x, vy,
                 16, n->content.h, fs, "", fg, false, 0, n->el, clip);
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
                         kSelectItemH, fs, "", fg, false, 0, n->el, clip);
        }
    }
    /* border around the list (follows the corner arcs when rounded) */
    fill_round_border(r->pixels, r->fb_w, r->fb_h, list_x, list_y,
                      list_w, list_h, radius, 1, border_c, clip);
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

/* soft shadow under a box: concentric rounded rects expanding up to `blur`
 * px, alpha fading outwards. Painted BEFORE the background so the element
 * body covers the inner layers. */
void paint_shadow(whaleui_render_t* r, whaleui_layout_node_t* n,
                  const Clip* clip)
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
    int sx0 = n->border.x + ox - blur;
    int sy0 = n->border.y + oy - blur;
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
    /* concentric layers, alpha fading outwards; skip the near-invisible
     * outermost ring and step by 2px (half the fill cost, gradient still
     * smooth) */
    for (int k = (blur > 0 ? blur - 1 : 0); k >= 1; k -= 2) {
        unsigned int ka = static_cast<unsigned>(a * (blur - k + 1) / (blur + 1));
        if (ka < 4) {
            continue;
        }
        unsigned int c = (ka << 24) | (col & 0x00FFFFFF);
        int sx = n->border.x + ox - k;
        int sy = n->border.y + oy - k;
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

/* parse "background-color 100ms" / "all 0.1s" -> duration in ms (0 = none) */
uint32_t transition_ms(const WhaleUIComputedStyle& s)
{
    std::string v = sget(s, "transition");
    if (v.empty() || v == "none") {
        return 0;
    }
    const char* p = v.c_str();
    while (*p) {
        if ((*p >= '0' && *p <= '9') || *p == '.') {
            float num = static_cast<float>(std::atof(p));
            const char* q = p;
            while ((*q >= '0' && *q <= '9') || *q == '.') {
                ++q;
            }
            while (*q == ' ' || *q == '\t') {
                ++q;
            }
            if (*q == 'm' && q[1] == 's') {
                return static_cast<uint32_t>(num);
            }
            if (*q == 's') {
                return static_cast<uint32_t>(num * 1000.0f);
            }
            p = q;
        } else {
            ++p;
        }
    }
    return 0;
}

unsigned int lerp_color(unsigned int from, unsigned int to, float t)
{
    auto ch = [t](unsigned int f, unsigned int g) -> unsigned int {
        return static_cast<unsigned int>(f + (static_cast<float>(g) - f) * t + 0.5f);
    };
    unsigned int fr = (from >> 16) & 0xFF, fg = (from >> 8) & 0xFF,
                 fb = from & 0xFF, fa = (from >> 24) & 0xFF;
    unsigned int tr = (to >> 16) & 0xFF, tg = (to >> 8) & 0xFF,
                 tb = to & 0xFF, ta = (to >> 24) & 0xFF;
    return (ch(fa, ta) << 24) | (ch(fr, tr) << 16) | (ch(fg, tg) << 8) |
           ch(fb, tb);
}

/* interpolated value at `now`; updates anim_last so the next frame's change
 * detection keeps working */
unsigned int current_anim_color(whaleui_render_t* r,
                                whaleui_render_t::ColorAnim& a,
                                const std::string& key);

/* resolve the drawn color for (el, prop): snaps unless a transition is
 * configured, in which case it starts/advances a ColorAnim and reports the
 * interpolated value. `running` is set while an animation is active. */
unsigned int anim_color(whaleui_render_t* r, whaleui_layout_node_t* n,
                        const char* prop, unsigned int target, bool* running)
{
    *running = false;
    if (!n->el) {
        return target;
    }
    std::string key = std::string(prop) + "@" +
                      std::to_string(reinterpret_cast<size_t>(n->el));
    const uint64_t now = SDL_GetTicks();
    auto it = r->anim_last.find(key);
    if (it == r->anim_last.end()) {
        r->anim_last[key] = target;
        return target;
    }
    if (it->second == target) {
        return target;
    }
    /* an animation is only continued, never restarted mid-flight */
    whaleui_render_t::ColorAnim* active = nullptr;
    for (auto& a : r->anims) {
        if (a.el == n->el && a.prop == prop) {
            active = &a;
            break;
        }
    }
    const uint32_t dur = transition_ms(n->style);
    if (!active && dur > 0) {
        whaleui_render_t::ColorAnim a;
        a.el = n->el;
        a.prop = prop;
        a.from = it->second;
        a.to = target;
        a.start = now;
        a.dur = dur;
        r->anims.push_back(a);
        active = &r->anims.back();
    } else if (!active) {
        it->second = target; /* no transition: snap */
        return target;
    }
    if (active->to != target) {
        /* target changed mid-flight: retarget from the current value */
        float p = static_cast<float>(now - active->start) /
                  static_cast<float>(active->dur);
        if (p > 1.0f) {
            p = 1.0f;
        }
        active->from = lerp_color(active->from, active->to, p);
        active->to = target;
        active->start = now;
    }
    *running = true;
    return current_anim_color(r, *active, key);
}

/* interpolated value at `now`; updates anim_last so the next frame's change
 * detection keeps working */
unsigned int current_anim_color(whaleui_render_t* r,
                                whaleui_render_t::ColorAnim& a,
                                const std::string& key)
{
    const uint64_t now = SDL_GetTicks();
    float p = static_cast<float>(now - a.start) / static_cast<float>(a.dur);
    if (p >= 1.0f) {
        p = 1.0f;
    }
    unsigned int v = lerp_color(a.from, a.to, p);
    r->anim_last[key] = v;
    return v;
}

void paint_node(whaleui_render_t* r, whaleui_layout_node_t* n, int off_y,
                int& seq, int sel_lo, int sel_hi, const Clip* clip)
{
    if (!n->visible) {
        return;
    }
    const int my_seq = seq++;
    if (n->is_text) {
        paint_text(r, n, off_y, my_seq, sel_lo, sel_hi, clip);
        return;
    }
    /* overflow: hidden/auto/scroll clips descendants to the border box
     * (auto/scroll containers also shift children by scroll_y at layout) */
    Clip self;
    const Clip* eff = clip;
    std::string ov = sget(n->style, "overflow");
    if (ov == "hidden" || ov == "auto" || ov == "scroll") {
        self.x = n->border.x;
        self.y = n->border.y + off_y;
        self.w = n->border.w;
        self.h = n->border.h;
        eff = &self;
        /* cull clipped containers fully outside the framebuffer: on a
         * scrolled page most of the document is off-screen, so skip their
         * whole subtree (the biggest scroll-paint win) */
        if (self.y + self.h <= 0 || self.y >= r->fb_h) {
            return;
        }
    }
    /* soft shadow first, so the element body covers the inner layers */
    paint_shadow(r, n, eff);
    /* background (border-radius supported) */
    unsigned int bg = color_of(n->style, "background-color", 0);
    unsigned int bg2 = color_of(n->style, "background", 0);
    if (bg == 0) {
        bg = bg2;
    }
    if (bg != 0) {
        unsigned int a = (bg >> 24) & 0xFF;
        unsigned int a8 = static_cast<unsigned>(a * n->opacity);
        unsigned int c = (a8 << 24) | (bg & 0x00FFFFFF);
        bool running = false;
        c = anim_color(r, n, "background-color", c, &running);
        int radius = 0;
        std::string br = sget(n->style, "border-radius");
        if (!br.empty()) {
            radius = std::atoi(br.c_str());
        }
        if (radius > 0) {
            fill_round_rect(r->pixels, r->fb_w, r->fb_h, n->border.x,
                            n->border.y + off_y,
                            n->border.w, n->border.h, radius, c, eff);
        } else {
            fill_rect(r->pixels, r->fb_w, r->fb_h, n->border.x,
                      n->border.y + off_y,
                      n->border.w, n->border.h, c, eff);
        }
    }
    /* border (follows the corner arcs when border-radius is set) */
    int bw[4] = {n->border_w[0], n->border_w[1], n->border_w[2], n->border_w[3]};
    bool any = bw[0] || bw[1] || bw[2] || bw[3];
    if (any) {
        unsigned int bc = color_of(n->style, "border-color",
                                   border_color_of(n->style, 0xFF000000));
        if (bc != 0) {
            unsigned int a = (bc >> 24) & 0xFF;
            unsigned int a8 = static_cast<unsigned>(a * n->opacity);
            unsigned int c = (a8 << 24) | (bc & 0x00FFFFFF);
            bool running = false;
            c = anim_color(r, n, "border-color", c, &running);
            int radius = 0;
            std::string br2 = sget(n->style, "border-radius");
            if (!br2.empty()) {
                radius = std::atoi(br2.c_str());
            }
            /* uniform border width for the ring */
            int brw = bw[1] > 0 ? bw[1] : (bw[3] > 0 ? bw[3] : (bw[0] > 0 ? bw[0] : bw[2]));
            if (radius > 0 && brw > 0) {
                fill_round_border(r->pixels, r->fb_w, r->fb_h, n->border.x,
                                  n->border.y + off_y,
                                  n->border.w, n->border.h, radius, brw, c, eff);
            } else {
                if (bw[0]) { /* top */
                    fill_rect(r->pixels, r->fb_w, r->fb_h, n->border.x,
                              n->border.y + off_y, n->border.w, bw[0], c, eff);
                }
                if (bw[2]) { /* bottom */
                    fill_rect(r->pixels, r->fb_w, r->fb_h, n->border.x,
                              n->border.y + off_y + n->border.h - bw[2], n->border.w, bw[2], c, eff);
                }
                if (bw[1]) { /* right */
                    fill_rect(r->pixels, r->fb_w, r->fb_h,
                              n->border.x + n->border.w - bw[1],
                              n->border.y + off_y, bw[1], n->border.h, c, eff);
                }
                if (bw[3]) { /* left */
                    fill_rect(r->pixels, r->fb_w, r->fb_h, n->border.x,
                              n->border.y + off_y, bw[3], n->border.h, c, eff);
                }
            }
        }
    }
    int child_off = off_y + scroll_delta(r, n);
    for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
        paint_node(r, c, child_off, seq, sel_lo, sel_hi, eff);
    }
    /* scrollbar sits on top of the content */
    if (n->scroll_max > 0) {
        paint_scrollbar(r, n, off_y, eff);
    }
    /* editable control: value text (input) + selection/caret overlay */
    if (n->el && is_editable(n->el)) {
        paint_editable(r, n, off_y, eff);
    }
    /* <select> control: value + arrow painted here; the expanded list is
     * painted LAST in render_frame so nothing occludes it */
    if (is_select_node(n)) {
        paint_select_value(r, n, off_y, eff);
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
 * Text runs: x from the parent content box, y from the run's own box
 * (already scroll-shifted at layout), vertically centered in the run's
 * estimated height. Containers (editable overlays): content box, centered. */
void text_origin(whaleui_render_t* r, whaleui_layout_node_t* n,
                 const std::string& text, int fs, const std::string& family,
                 bool bold, int* tx, int* ty)
{
    int tw = 0, th = 0;
    text_size(r, text, fs, family, bold, &tw, &th);
    if (n->is_text) {
        whaleui_layout_node_t* box =
            (n->parent && !n->parent->is_text) ? n->parent : n;
        *tx = box->content.x;
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

/* --- FSR 1.0 (GPU compute) resources --- */

/* build compute pipelines + textures for the current window size. Returns 1
 * when everything is usable (FSR can run), 0 if any resource failed. */
int render_fsr_create(whaleui_render_t* r)
{
    SDL_GPUDevice* d = r->device;
    /* pick the shader variant for the actual backend: SPIR-V on Vulkan,
     * DXIL (compiled from HLSL with dxc) on D3D12 */
    const char* drv = SDL_GetGPUDeviceDriver(d);
    const bool vulkan = drv && std::strcmp(drv, "vulkan") == 0;
    const uint32_t* easu_code = vulkan ? g_easu_spv : g_easu_dxil;
    const uint32_t easu_words = vulkan ? g_easu_spv_size : g_easu_dxil_size;
    const uint32_t* rcas_code = vulkan ? g_rcas_spv : g_rcas_dxil;
    const uint32_t rcas_words = vulkan ? g_rcas_spv_size : g_rcas_dxil_size;
    const SDL_GPUShaderFormat fmt =
        vulkan ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_DXIL;
    SDL_GPUComputePipelineCreateInfo ci;
    std::memset(&ci, 0, sizeof(ci));
    ci.entrypoint = "main";
    ci.format = fmt;
    ci.num_readonly_storage_textures = 1;
    ci.num_readwrite_storage_textures = 1;
    ci.num_uniform_buffers = 1;
    ci.threadcount_x = 8;
    ci.threadcount_y = 8;
    ci.threadcount_z = 1;
    ci.code_size = easu_words * 4;
    ci.code = reinterpret_cast<const Uint8*>(easu_code);
    r->fsr_easu_pipe = SDL_CreateGPUComputePipeline(d, &ci);
    if (!r->fsr_easu_pipe) {
        return 0;
    }
    ci.code_size = rcas_words * 4;
    ci.code = reinterpret_cast<const Uint8*>(rcas_code);
    r->fsr_rcas_pipe = SDL_CreateGPUComputePipeline(d, &ci);
    if (!r->fsr_rcas_pipe) {
        return 0;
    }
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
    const SDL_GPUTextureUsageFlags low_usage =
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ;
    const SDL_GPUTextureUsageFlags scratch_usage =
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    if (!mkTex(r->fb_w, r->fb_h, low_usage, &r->offscreen_low) ||
        !mkTex(r->width, r->height, scratch_usage, &r->fsr_up) ||
        !mkTex(r->width, r->height, scratch_usage, &r->fsr_out)) {
        return 0;
    }
    SDL_GPUTransferBufferCreateInfo tbi;
    std::memset(&tbi, 0, sizeof(tbi));
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = static_cast<Uint32>(static_cast<size_t>(r->fb_w) * r->fb_h * 4);
    r->fsr_transfer = SDL_CreateGPUTransferBuffer(d, &tbi);
    return r->fsr_transfer != nullptr;
}

void render_fsr_destroy(whaleui_render_t* r)
{
    if (!r) {
        return;
    }
    SDL_GPUDevice* d = r->device;
    SDL_ReleaseGPUComputePipeline(d, r->fsr_easu_pipe);
    SDL_ReleaseGPUComputePipeline(d, r->fsr_rcas_pipe);
    SDL_ReleaseGPUTexture(d, r->offscreen_low);
    SDL_ReleaseGPUTexture(d, r->fsr_up);
    SDL_ReleaseGPUTexture(d, r->fsr_out);
    SDL_ReleaseGPUTransferBuffer(d, r->fsr_transfer);
    r->fsr_easu_pipe = nullptr;
    r->fsr_rcas_pipe = nullptr;
    r->offscreen_low = nullptr;
    r->fsr_up = nullptr;
    r->fsr_out = nullptr;
    r->fsr_transfer = nullptr;
}

/* should the current frame use the FSR path? mode 0 = auto. */
int fsr_want_active(whaleui_render_t* r)
{
    if (!r->fsr_easu_pipe || !r->fsr_rcas_pipe || !r->offscreen_low) {
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
    r->pixels.resize(static_cast<size_t>(r->fb_w) * r->fb_h, 0xFF202020);

    /* GPU path: offscreen target + transfer buffer */
    SDL_GPUTextureCreateInfo tci;
    std::memset(&tci, 0, sizeof(tci));
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    tci.width = static_cast<Uint32>(width);
    tci.height = static_cast<Uint32>(height);
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    r->offscreen = SDL_CreateGPUTexture(device, &tci);

    SDL_GPUTransferBufferCreateInfo tbi;
    std::memset(&tbi, 0, sizeof(tbi));
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = static_cast<Uint32>(static_cast<size_t>(width) * height * 4);
    r->transfer = SDL_CreateGPUTransferBuffer(device, &tbi);

    if (!r->offscreen || !r->transfer) {
        SDL_ReleaseGPUTexture(device, r->offscreen);
        SDL_ReleaseGPUTransferBuffer(device, r->transfer);
        delete r;
        return nullptr;
    }
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
                render_build_fallback(r, r->font_default, 16, false);
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
    if (r->text_engine) {
        TTF_DestroySurfaceTextEngine(r->text_engine);
    }
#endif
    SDL_ReleaseGPUTexture(r->device, r->offscreen);
    SDL_ReleaseGPUTransferBuffer(r->device, r->transfer);
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
    render_fsr_destroy(r);
    render_fsr_create(r);
    r->fsr_active = 0;
    r->has_dirty = 1;
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
    return 0;
}

extern "C" void whaleui_render_set_hover(whaleui_render_t* r, int x, int y)
{
    if (!r || !r->tree) {
        return;
    }
    fb_coords(r, x, y);
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

    auto do_scroll = [r, dpx](whaleui_layout_node_t* sc) {
        if (!sc->el || sc->scroll_max <= 0) {
            return;
        }
        int& cur = r->scrolls[sc->el];
        /* Content moves opposite the wheel: rolling down (dy<0) reveals
         * content further down, so scroll_y increases with -dy. */
        int nv = cur - static_cast<int>(dpx);
        if (nv > sc->scroll_max) {
            nv = sc->scroll_max;
        }
        if (nv < 0) {
            nv = 0;
        }
        if (nv != cur) {
            cur = nv;
            /* no relayout: the paint path applies the scroll offset (see
             * scroll_delta), so wheel scrolling stays cheap on big pages */
            r->scroll_dirty = 1;
        }
    };

    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    /* nearest scrollable ancestor (the hit element itself included).
     * Text runs inherit their parent's overflow but are not scroll
     * containers (scroll_max == 0): skip them so the walk reaches the
     * actual box */
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
    /* skip the whole frame when nothing changed: idle frames cost ~0.
     * Repaint when the layout/state is dirty, a wheel scroll happened, a
     * transition is running, or an editable caret is blinking. */
    if (!r->has_dirty && r->tree && !r->scroll_dirty &&
        r->anims.empty() && !r->edit_el) {
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
            render_fsr_destroy(r);
            render_fsr_create(r);
            r->has_dirty = 1;
        }
    }
    if (r->has_dirty || !r->tree) {
        whaleui_layout_destroy(r->tree);
        whaleui_style_state st;
        st.hover = r->hover_el;
        st.focus = r->focus_el;
        st.pressed = r->pressed_el;
        r->tree = whaleui_layout_compute(doc, r->rules, r->rule_count,
                                         &r->theme_vars, r->fb_w, r->fb_h,
                                         &st, &r->scrolls);
        r->has_dirty = 0;
    }
    if (!r->tree) {
        return -2;
    }

    /* paint */
    std::fill(r->pixels.begin(), r->pixels.end(), r->bg_color);
    Clip full = {0, 0, r->fb_w, r->fb_h};
    int sel_lo = 0, sel_hi = 0;
    sel_seq(r, &sel_lo, &sel_hi);
    int seq = 0;
    paint_node(r, r->tree->root, 0, seq, sel_lo, sel_hi, &full);

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

    /* advance color transitions: keep repainting while any is running, then
     * drop finished ones */
    {
        const uint64_t now = SDL_GetTicks();
        bool any = false;
        for (auto& a : r->anims) {
            if (now < a.start + a.dur) {
                any = true;
            }
        }
        if (any) {
            r->has_dirty = 1;
        }
        auto& an = r->anims;
        an.erase(std::remove_if(an.begin(), an.end(),
                                [now](const whaleui_render_t::ColorAnim& a) {
                                    return now >= a.start + a.dur;
                                }),
                 an.end());
    }

    /* present: upload offscreen + blit to swapchain */
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(r->device);
    if (!cmd) {
        return -3;
    }
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 sw = 0, sh = 0;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, r->window, &swapchain, &sw, &sh) || !swapchain) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return 0;
    }
    if (r->fsr_active && r->offscreen_low) {
        /* FSR path: upload the low-res framebuffer (RGB swapped: pixels are
         * 0xAARRGGBB = B,G,R,A bytes, the rgba8 shader wants R,G,B,A), then
         * EASU upscale -> RCAS sharpen -> blit the full-res result. */
        void* mapped = SDL_MapGPUTransferBuffer(r->device, r->fsr_transfer, false);
        if (mapped) {
            const unsigned char* src =
                reinterpret_cast<const unsigned char*>(r->pixels.data());
            unsigned char* dst = static_cast<unsigned char*>(mapped);
            const size_t n = r->pixels.size();
            for (size_t i = 0; i < n; ++i) {
                dst[i * 4 + 0] = src[i * 4 + 2]; /* R */
                dst[i * 4 + 1] = src[i * 4 + 1]; /* G */
                dst[i * 4 + 2] = src[i * 4 + 0]; /* B */
                dst[i * 4 + 3] = src[i * 4 + 3]; /* A */
            }
            SDL_UnmapGPUTransferBuffer(r->device, r->fsr_transfer);
        }
        SDL_GPUTextureTransferInfo upload;
        std::memset(&upload, 0, sizeof(upload));
        upload.transfer_buffer = r->fsr_transfer;
        SDL_GPUTextureRegion region;
        std::memset(&region, 0, sizeof(region));
        region.texture = r->offscreen_low;
        region.w = static_cast<Uint32>(r->fb_w);
        region.h = static_cast<Uint32>(r->fb_h);
        region.d = 1;
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_UploadToGPUTexture(cp, &upload, &region, false);
        SDL_EndGPUCopyPass(cp);

        auto runPass = [&](SDL_GPUComputePipeline* p, SDL_GPUTexture* rw,
                           SDL_GPUTexture* ro, const void* pc, Uint32 pcSz,
                           int outW, int outH) {
            SDL_GPUStorageTextureReadWriteBinding rwBind;
            std::memset(&rwBind, 0, sizeof(rwBind));
            rwBind.texture = rw;
            rwBind.mip_level = 0;
            rwBind.layer = 0;
            rwBind.cycle = false;
            SDL_GPUComputePass* cps = SDL_BeginGPUComputePass(cmd, &rwBind, 1, nullptr, 0);
            if (cps) {
                SDL_BindGPUComputePipeline(cps, p);
                SDL_BindGPUComputeStorageTextures(cps, 0, &ro, 1);
                SDL_PushGPUComputeUniformData(cmd, 0, pc, pcSz);
                SDL_DispatchGPUCompute(cps, (static_cast<Uint32>(outW) + 7) / 8,
                                       (static_cast<Uint32>(outH) + 7) / 8, 1);
                SDL_EndGPUComputePass(cps);
            }
        };
        float pf[4] = {static_cast<float>(r->fb_w), static_cast<float>(r->fb_h),
                       static_cast<float>(r->fb_w) / static_cast<float>(r->width),
                       static_cast<float>(r->fb_h) / static_cast<float>(r->height)};
        runPass(r->fsr_easu_pipe, r->fsr_up, r->offscreen_low, pf, sizeof(pf),
                r->width, r->height);
        float rp[4] = {r->fsr_sharpness, 0, 0, 0};
        runPass(r->fsr_rcas_pipe, r->fsr_out, r->fsr_up, rp, sizeof(rp),
                r->width, r->height);

        SDL_GPUBlitInfo blit;
        std::memset(&blit, 0, sizeof(blit));
        blit.source.texture = r->fsr_out;
        blit.source.w = static_cast<Uint32>(r->width);
        blit.source.h = static_cast<Uint32>(r->height);
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
    void* mapped = SDL_MapGPUTransferBuffer(r->device, r->transfer, false);
    if (mapped) {
        std::memcpy(mapped, r->pixels.data(), r->pixels.size() * 4);
        SDL_UnmapGPUTransferBuffer(r->device, r->transfer);
    }
    SDL_GPUTextureTransferInfo upload;
    std::memset(&upload, 0, sizeof(upload));
    upload.transfer_buffer = r->transfer;
    SDL_GPUTextureRegion region;
    std::memset(&region, 0, sizeof(region));
    region.texture = r->offscreen;
    region.w = static_cast<Uint32>(r->fb_w);
    region.h = static_cast<Uint32>(r->fb_h);
    region.d = 1;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_UploadToGPUTexture(cp, &upload, &region, false);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUBlitInfo blit;
    std::memset(&blit, 0, sizeof(blit));
    blit.source.texture = r->offscreen;
    blit.source.w = static_cast<Uint32>(r->fb_w);
    blit.source.h = static_cast<Uint32>(r->fb_h);
    blit.destination.texture = swapchain;
    blit.destination.w = sw;
    blit.destination.h = sh;
    blit.load_op = SDL_GPU_LOADOP_CLEAR;
    blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cmd, &blit);
    SDL_SubmitGPUCommandBuffer(cmd);
    return 0;
}

