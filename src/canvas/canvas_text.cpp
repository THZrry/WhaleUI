/* Canvas 2D text backend: measure + rasterize UTF-8 text for
 * fillText/strokeText/measureText.
 *
 * Full builds rasterize through SDL3_ttf (the same TTF_Init'd library and
 * font registry the page text uses, so canvas text matches page text);
 * lite/minimal have no TTF backend, and the canvas text APIs report
 * failure there rather than drawing nothing or wrong glyphs.
 *
 * The `font` CSS string ("bold italic 16px Arial, sans-serif") is parsed
 * here: weight/style prefix, px size, family list. Units other than px
 * (pt/em/%) are not resolved - canvas has no cascade to resolve them
 * against, so they fall back to the default size. */

#include "canvas/canvas.h"
#include "font/font.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef WHALEUI_BUILD_FULL
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#endif

namespace {

struct FontSpec
{
    int size;       /* px */
    int style;      /* 0 normal, 1 bold, 2 italic (TTF_STYLE_*) */
    std::string family;
};

/* parse the canvas `font` shorthand; defaults match the spec's "10px
 * sans-serif" initial value */
FontSpec parse_font(const char* css)
{
    FontSpec f;
    f.size = 10;
    f.style = 0;
    f.family = "sans-serif";
    if (!css || !*css) {
        return f;
    }
    std::string s(css);
    size_t pos = 0;
    bool saw_size = false;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
            ++pos;
        }
        size_t start = pos;
        while (pos < s.size() && s[pos] != ' ' && s[pos] != '\t') {
            ++pos;
        }
        std::string tok = s.substr(start, pos - start);
        if (tok.empty()) {
            continue;
        }
        if (!saw_size) {
            if (tok == "bold" || tok == "bolder") {
                f.style |= 1;
                continue;
            }
            if (tok == "italic" || tok == "oblique") {
                f.style |= 2;
                continue;
            }
            if (tok == "normal" || tok == "small-caps") {
                continue;
            }
            /* a size token: digits (possibly fractional) + optional unit */
            if (tok[0] >= '0' && tok[0] <= '9') {
                f.size = static_cast<int>(std::atof(tok.c_str()) + 0.5);
                if (f.size <= 0) {
                    f.size = 10;
                }
                saw_size = true;
                continue;
            }
        }
        /* first non-keyword after the size: the family list (rest of the
         * string, commas and all) */
        f.family = s.substr(start);
        break;
    }
    return f;
}

} // namespace

#ifdef WHALEUI_BUILD_FULL

/* open the registry font whose family matches (or the first loadable one
 * when the family is generic/unknown). The TTF_Font is cached per
 * (family, size, style) so repeated fillText calls reuse the glyph cache
 * the way the page text path does. */
namespace {

struct TtfCache
{
    std::string key;
    TTF_Font* font;
};
std::vector<TtfCache>& ttf_cache()
{
    static std::vector<TtfCache> c;
    return c;
}

bool family_matches(const char* real, const std::string& want)
{
    if (!real || want.empty()) {
        return false;
    }
    /* case-insensitive compare */
    size_t i = 0;
    for (; real[i] && want[i]; ++i) {
        char a = real[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) {
            return false;
        }
    }
    return real[i] == '\0' && want[i] == '\0';
}

TTF_Font* open_for(const FontSpec& spec)
{
    char keybuf[256];
    std::snprintf(keybuf, sizeof(keybuf), "%d|%d|%s", spec.size, spec.style,
                  spec.family.c_str());
    std::string key = keybuf;
    for (size_t i = 0; i < ttf_cache().size(); ++i) {
        if (ttf_cache()[i].key == key) {
            return ttf_cache()[i].font;
        }
    }
    whaleui_font_registry* reg = whaleui_font_registry_get();
    TTF_Font* font = nullptr;
    bool generic = spec.family == "sans-serif" || spec.family == "serif" ||
                   spec.family == "monospace" ||
                   spec.family.find("sans-serif") != std::string::npos;
    if (reg) {
        /* 1. exact family match on the registry entry's family name */
        for (size_t i = 0; i < reg->count && !font; ++i) {
            if (!generic && reg->fonts[i].family &&
                family_matches(reg->fonts[i].family, spec.family)) {
                SDL_IOStream* io = nullptr;
                if (reg->fonts[i].path) {
                    io = SDL_IOFromFile(reg->fonts[i].path, "rb");
                } else if (reg->fonts[i].data && reg->fonts[i].len) {
                    io = SDL_IOFromMem(
                        const_cast<unsigned char*>(reg->fonts[i].data),
                        reg->fonts[i].len);
                }
                if (io) {
                    font = TTF_OpenFontIO(io, true,
                                          static_cast<float>(spec.size));
                }
            }
        }
        /* 2. the real family name of an opened font (registry family is the
         * file base name, e.g. "segoeui" vs the real "Segoe UI") */
        if (!font && !generic) {
            for (size_t i = 0; i < reg->count && !font; ++i) {
                SDL_IOStream* io = nullptr;
                if (reg->fonts[i].path) {
                    io = SDL_IOFromFile(reg->fonts[i].path, "rb");
                } else if (reg->fonts[i].data && reg->fonts[i].len) {
                    io = SDL_IOFromMem(
                        const_cast<unsigned char*>(reg->fonts[i].data),
                        reg->fonts[i].len);
                }
                if (!io) {
                    continue;
                }
                TTF_Font* probe = TTF_OpenFontIO(io, true,
                                                 static_cast<float>(spec.size));
                if (!probe) {
                    continue;
                }
                const char* real = TTF_GetFontFamilyName(probe);
                if (real && family_matches(real, spec.family)) {
                    font = probe;
                } else {
                    TTF_CloseFont(probe);
                }
            }
        }
        /* 3. generic/unknown: first loadable registry font */
        if (!font) {
            for (size_t i = 0; i < reg->count && !font; ++i) {
                SDL_IOStream* io = nullptr;
                if (reg->fonts[i].path) {
                    io = SDL_IOFromFile(reg->fonts[i].path, "rb");
                } else if (reg->fonts[i].data && reg->fonts[i].len) {
                    io = SDL_IOFromMem(
                        const_cast<unsigned char*>(reg->fonts[i].data),
                        reg->fonts[i].len);
                }
                if (io) {
                    font = TTF_OpenFontIO(io, true,
                                          static_cast<float>(spec.size));
                }
            }
        }
    }
    if (font) {
        TTF_SetFontStyle(font, spec.style);
        TtfCache e;
        e.key = key;
        e.font = font;
        ttf_cache().push_back(e);
    }
    return font;
}

} // namespace

extern "C" int whaleui_canvas_text_measure(const char* utf8,
                                           const char* font_css, int* out_w,
                                           int* out_h)
{
    if (!utf8 || !out_w || !out_h) {
        return -1;
    }
    FontSpec spec = parse_font(font_css);
    TTF_Font* font = open_for(spec);
    if (!font) {
        return -2; /* no loadable font */
    }
    int w = 0, h = 0;
    if (!TTF_GetStringSize(font, utf8, 0, &w, &h)) {
        return -3;
    }
    *out_w = w;
    *out_h = h;
    return 0;
}

extern "C" int whaleui_canvas_text_raster(const char* utf8,
                                          const char* font_css,
                                          unsigned int color,
                                          std::vector<unsigned int>& out_px,
                                          int* out_w, int* out_h, int* out_xoff,
                                          int* out_yoff)
{
    if (!utf8 || !out_w || !out_h || !out_xoff || !out_yoff) {
        return -1;
    }
    FontSpec spec = parse_font(font_css);
    TTF_Font* font = open_for(spec);
    if (!font) {
        return -2;
    }
    SDL_Color fg;
    fg.r = static_cast<Uint8>((color >> 16) & 0xFF);
    fg.g = static_cast<Uint8>((color >> 8) & 0xFF);
    fg.b = static_cast<Uint8>(color & 0xFF);
    fg.a = 255;
    SDL_Surface* surf = TTF_RenderText_Blended(font, utf8, 0, fg);
    if (!surf) {
        return -3;
    }
    SDL_Surface* rgba = surf;
    if (surf->format != SDL_PIXELFORMAT_RGBA32) {
        rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surf);
        if (!rgba) {
            return -4;
        }
    }
    int w = rgba->w, h = rgba->h;
    out_px.assign(static_cast<size_t>(w) * h, 0u);
    const unsigned char* src = static_cast<const unsigned char*>(rgba->pixels);
    int pitch = rgba->pitch;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const unsigned char* s =
                src + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
            unsigned int a = s[3];
            if (a == 0) {
                continue;
            }
            /* straight alpha: keep the color channels, carry the coverage */
            out_px[static_cast<size_t>(y) * w + x] =
                (a << 24) | (static_cast<unsigned int>(s[0]) << 16) |
                (static_cast<unsigned int>(s[1]) << 8) |
                static_cast<unsigned int>(s[2]);
        }
    }
    SDL_DestroySurface(rgba);
    *out_w = w;
    *out_h = h;
    /* TTF_RenderText_Blended returns a surface cropped to the ink box; the
     * baseline origin maps to the ascent, so the caller positions at
     * (x, y - ascent) for textBaseline=alphabetic. Report the ascent as the
     * y offset from the baseline to the surface top. */
    *out_xoff = 0;
    int asc = TTF_GetFontAscent(font);
    *out_yoff = -asc;
    return 0;
}

#else /* lite/minimal: no TTF backend for canvas text yet */

extern "C" int whaleui_canvas_text_measure(const char*, const char*, int*,
                                           int*)
{
    return -2;
}

extern "C" int whaleui_canvas_text_raster(const char*, const char*,
                                          unsigned int,
                                          std::vector<unsigned int>&, int*,
                                          int*, int*, int*)
{
    return -2;
}

#endif
