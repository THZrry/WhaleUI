/* Renderer: CPU paint into an RGBA framebuffer, uploaded to an offscreen GPU
 * texture and blitted to the swapchain (SDL built-in blit pipeline; custom
 * shaders are a later step). Text comes from SDL3_ttf using fonts registered
 * through the font module. */

#include "render/render.h"
#include "font/font.h"
#include "style/style.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#ifdef WHALEUI_BUILD_FULL
#include <SDL3_ttf/SDL_ttf.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>

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
            unsigned int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            if (a == 255) {
                d = 0xFF000000 | (sr << 16) | (sg << 8) | sb;
            } else {
                unsigned int nr = (sr * a + dr * (255 - a)) / 255;
                unsigned int ng = (sg * a + dg * (255 - a)) / 255;
                unsigned int nb = (sb * a + db * (255 - a)) / 255;
                d = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
            }
        }
    }
}

/* style helpers */
std::string sget(const WhaleUIComputedStyle& s, const char* k)
{
    auto it = s.find(k);
    return it == s.end() ? std::string() : it->second;
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
        int rr = 0, gg = 0, bb = 0, aa = 255;
        if (std::sscanf(s, "rgba(%d,%d,%d,%d)", &rr, &gg, &bb, &aa) >= 3 ||
            std::sscanf(s, "rgb(%d,%d,%d)", &rr, &gg, &bb) >= 3) {
            unsigned int r = rr < 0 ? 0 : static_cast<unsigned>(rr);
            unsigned int g = gg < 0 ? 0 : static_cast<unsigned>(gg);
            unsigned int b = bb < 0 ? 0 : static_cast<unsigned>(bb);
            unsigned int a = aa < 0 ? 0 : static_cast<unsigned>(aa);
            if (r > 255) { r = 255; }
            if (g > 255) { g = 255; }
            if (b > 255) { b = 255; }
            if (a > 255) { a = 255; }
            *out = (a << 24) | (r << 16) | (g << 8) | b;
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

#ifdef WHALEUI_BUILD_FULL

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

void paint_text(whaleui_render_t* r, whaleui_layout_node_t* n, const Clip* clip)
{
#ifdef WHALEUI_BUILD_FULL
    unsigned int color = color_of(n->style, "color", 0xFF000000);
    float alpha = (color >> 24) & 0xFF;
    unsigned int a8 = static_cast<unsigned>(alpha * n->opacity);
    color = (a8 << 24) | (color & 0x00FFFFFF);

    int fs = 16;
    std::string fsv = sget(n->style, "font-size");
    if (!fsv.empty()) {
        fs = std::atoi(fsv.c_str());
    }
    if (fs <= 0) {
        fs = 16;
    }
    std::string family = sget(n->style, "font-family");
    /* font-weight: bold/bolder/600-900 render bold */
    std::string fw = sget(n->style, "font-weight");
    bool bold = fw == "bold" || fw == "bolder" ||
                (!fw.empty() && std::atoi(fw.c_str()) >= 600);
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
    TTF_Text* t = TTF_CreateText(engine, font, n->text.c_str(), n->text.size());
    if (!t) {
        return;
    }
    /* NOTE: TTF_SetTextColor (and Float) on the 3.2.2 prebuilt break
     * TTF_DrawSurfaceText (draws nothing, no error). Instead we render with
     * the default color and tint during blending. */
    int tw = 0, th = 0;
    TTF_GetTextSize(t, &tw, &th);
    if (tw > 0 && th > 0) {
        SDL_Surface* surf = SDL_CreateSurface(tw, th, SDL_PIXELFORMAT_RGBA8888);
        if (surf) {
            SDL_FillSurfaceRect(surf, nullptr, 0);
            TTF_DrawSurfaceText(t, 0, 0, surf);
            /* text-align within the box */
            std::string ta = sget(n->style, "text-align");
            int tx = n->border.x;
            if (ta == "center") {
                tx = n->border.x + (n->border.w - tw) / 2;
            } else if (ta == "right") {
                tx = n->border.x + n->border.w - tw;
            }
            int ty = n->border.y + (n->border.h - th) / 2;
            blend_surface(r->pixels, r->width, r->height, surf, tx, ty, clip, &color);
            SDL_DestroySurface(surf);
        }
    }
    TTF_DestroyText(t);
#endif /* WHALEUI_BUILD_FULL */
}

void paint_node(whaleui_render_t* r, whaleui_layout_node_t* n, const Clip* clip)
{
    if (!n->visible) {
        return;
    }
    if (n->is_text) {
        paint_text(r, n, clip);
        return;
    }
    /* overflow: hidden clips descendants to the border box */
    Clip self;
    const Clip* eff = clip;
    std::string ov = sget(n->style, "overflow");
    if (ov == "hidden") {
        self.x = n->border.x;
        self.y = n->border.y;
        self.w = n->border.w;
        self.h = n->border.h;
        eff = &self;
    }
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
        int radius = 0;
        std::string br = sget(n->style, "border-radius");
        if (!br.empty()) {
            radius = std::atoi(br.c_str());
        }
        if (radius > 0) {
            fill_round_rect(r->pixels, r->width, r->height, n->border.x, n->border.y,
                            n->border.w, n->border.h, radius, c, eff);
        } else {
            fill_rect(r->pixels, r->width, r->height, n->border.x, n->border.y,
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
            int radius = 0;
            std::string br2 = sget(n->style, "border-radius");
            if (!br2.empty()) {
                radius = std::atoi(br2.c_str());
            }
            /* uniform border width for the ring */
            int brw = bw[1] > 0 ? bw[1] : (bw[3] > 0 ? bw[3] : (bw[0] > 0 ? bw[0] : bw[2]));
            if (radius > 0 && brw > 0) {
                fill_round_border(r->pixels, r->width, r->height, n->border.x, n->border.y,
                                  n->border.w, n->border.h, radius, brw, c, eff);
            } else {
                if (bw[0]) { /* top */
                    fill_rect(r->pixels, r->width, r->height, n->border.x, n->border.y, n->border.w, bw[0], c, eff);
                }
                if (bw[2]) { /* bottom */
                    fill_rect(r->pixels, r->width, r->height, n->border.x,
                              n->border.y + n->border.h - bw[2], n->border.w, bw[2], c, eff);
                }
                if (bw[1]) { /* right */
                    fill_rect(r->pixels, r->width, r->height,
                              n->border.x + n->border.w - bw[1], n->border.y, bw[1], n->border.h, c, eff);
                }
                if (bw[3]) { /* left */
                    fill_rect(r->pixels, r->width, r->height, n->border.x, n->border.y, bw[3], n->border.h, c, eff);
                }
            }
        }
    }
    for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
        paint_node(r, c, eff);
    }
}

} // namespace

extern "C" whaleui_render_t* whaleui_render_create(SDL_GPUDevice* device, SDL_Window* window,
                                                   int width, int height)
{
    if (!device || !window || width <= 0 || height <= 0) {
        return nullptr;
    }
    whaleui_render_t* r = new whaleui_render_t;
    std::memset(r, 0, sizeof(*r));
    r->device = device;
    r->window = window;
    r->width = width;
    r->height = height;
    r->has_dirty = 1;
    r->bg_color = 0xFF202020;
    r->pixels.resize(static_cast<size_t>(width) * height, 0xFF202020);

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
    if (r->text_engine) {
        TTF_DestroySurfaceTextEngine(r->text_engine);
    }
#endif
    SDL_ReleaseGPUTexture(r->device, r->offscreen);
    SDL_ReleaseGPUTransferBuffer(r->device, r->transfer);
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
    r->has_dirty = 1;
    return 0;
}

extern "C" int whaleui_render_frame(whaleui_render_t* r, whaleui_dom_document_t* doc)
{
    if (!r || !doc) {
        return -1;
    }
    if (r->has_dirty || !r->tree) {
        whaleui_layout_destroy(r->tree);
        r->tree = whaleui_layout_compute(doc, r->rules, r->rule_count,
                                         &r->theme_vars, r->width, r->height);
        r->has_dirty = 0;
    }
    if (!r->tree) {
        return -2;
    }

    /* paint */
    std::fill(r->pixels.begin(), r->pixels.end(), r->bg_color);
    Clip full = {0, 0, r->width, r->height};
    paint_node(r, r->tree->root, &full);

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
    region.w = static_cast<Uint32>(r->width);
    region.h = static_cast<Uint32>(r->height);
    region.d = 1;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_UploadToGPUTexture(cp, &upload, &region, false);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUBlitInfo blit;
    std::memset(&blit, 0, sizeof(blit));
    blit.source.texture = r->offscreen;
    blit.source.w = static_cast<Uint32>(r->width);
    blit.source.h = static_cast<Uint32>(r->height);
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
