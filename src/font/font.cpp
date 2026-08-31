/* Font: public C API implementation.
 * Step 2: contract implementation - registry + family naming (parsed from
 * the font file's name table is step 3; for now family = file base name or
 * "memory-N"). Loading goes through the VFS as required. */

#include "font/font.h"
#include "fs/fs.h"
#include "platform/platform.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace {

whaleui_font_registry* registry()
{
    static whaleui_font_registry r;
    return &r;
}

char* dup_str(const char* s)
{
    size_t n = std::strlen(s) + 1;
    char* d = static_cast<char*>(std::malloc(n));
    if (d) {
        std::memcpy(d, s, n);
    }
    return d;
}

/* "path/to/FontName.ttf" -> "FontName" */
char* family_from_path(const char* path)
{
    const char* base = std::strrchr(path, '/');
    if (!base) {
        base = std::strrchr(path, '\\');
    }
    base = base ? base + 1 : path;
    char* name = dup_str(base);
    char* dot = name ? std::strrchr(name, '.') : nullptr;
    if (dot) {
        *dot = '\0';
    }
    return name;
}

} // namespace

extern "C" whaleui_font_registry* whaleui_font_registry_get(void)
{
    return registry();
}

extern "C" const char* whaleui_font_register(const char* uri)
{
    if (!uri) {
        return nullptr;
    }
    /* dedupe: same family (base file name) already registered -> reuse it
     * instead of re-reading the font file */
    whaleui_font_registry* reg = registry();
    char* want = family_from_path(uri);
    for (size_t j = 0; j < reg->count; ++j) {
        if (reg->fonts[j].family && want &&
            std::strcmp(reg->fonts[j].family, want) == 0) {
            std::free(want);
            return reg->fonts[j].family;
        }
    }
    std::free(want);
    whaleui_font_t* nf = static_cast<whaleui_font_t*>(std::realloc(reg->fonts, (reg->count + 1) * sizeof(*reg->fonts)));
    if (!nf) {
        return nullptr;
    }
    reg->fonts = nf;
    whaleui_font_t* f = &reg->fonts[reg->count];
    std::memset(f, 0, sizeof(*f));
    f->family = family_from_path(uri);
    /* the file is read lazily when a TTF_Font is opened (render_text), so
     * registering a font pins no file data - a 20MB CJK font costs a few
     * bytes until a page actually renders its glyphs. */
    f->path = dup_str(uri);
    f->data = nullptr;
    f->len = 0;
    reg->count++;
    return f->family;
}

extern "C" const char* whaleui_font_register_memory(const unsigned char* data, size_t len)
{
    if (!data || len == 0) {
        return nullptr;
    }
    whaleui_font_registry* reg = registry();
    whaleui_font_t* nf = static_cast<whaleui_font_t*>(std::realloc(reg->fonts, (reg->count + 1) * sizeof(*reg->fonts)));
    if (!nf) {
        return nullptr;
    }
    reg->fonts = nf;
    whaleui_font_t* f = &reg->fonts[reg->count];
    std::memset(f, 0, sizeof(*f));
    char name[32];
    std::sprintf(name, "memory-%zu", reg->count);
    f->family = dup_str(name);
    f->path = nullptr;
    f->data = data; /* borrowed */
    f->len = len;
    reg->count++;
    return f->family;
}

extern "C" int whaleui_font_set_default(const char* family)
{
    if (!family) {
        return -1;
    }
    whaleui_font_registry* reg = registry();
    char* d = dup_str(family);
    if (!d) {
        return -1;
    }
    std::free(reg->default_family);
    reg->default_family = d;
    return 0;
}

extern "C" const char* whaleui_font_get_default(void)
{
    whaleui_font_registry* reg = registry();
    return reg->default_family ? reg->default_family : "sans-serif";
}

extern "C" const char* whaleui_font_list(void)
{
    whaleui_font_registry* reg = registry();
    if (reg->count == 0) {
        return "";
    }
    /* rebuild each call; simple contract impl */
    static char buf[512];
    buf[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < reg->count && off < sizeof(buf); ++i) {
        int n = std::snprintf(buf + off, sizeof(buf) - off, "%s%s",
                              i ? "," : "", reg->fonts[i].family);
        if (n > 0) {
            off += static_cast<size_t>(n);
        }
    }
    return buf;
}

extern "C" int whaleui_font_register_system_defaults(void)
{
    /* One-shot helper: register the platform's common UI fonts so text can
     * fall back across Latin / CJK / emoji. Registration only records the
     * file PATH - no font data is read here (render_text opens a font only
     * when a page actually needs its glyphs), so registering a long list
     * costs a few dozen bytes per font and never auto-loads anything.
     * Candidates are tried in order; missing files are skipped. The lazy
     * fallback chain (render_text.cpp ensure_fallback) walks this same
     * order, so the first candidate that can satisfy a missing glyph wins. */
#if defined(_WIN32)
    /* Windows: Segoe UI (Vista+) + Arial (universal) cover Latin; YaHei /
     * SimSun cover CJK; Segoe UI Emoji (8.1+) covers emoji. Arial/SimSun
     * stay as the last-resort chain so even a stripped Win8 without
     * Segoe/YaHei still has a default font. */
    static const char* kFiles[] = {
        "segoeui.ttf",  /* Segoe UI */
        "arial.ttf",    /* universal Latin fallback */
        "msyh.ttc",     /* Microsoft YaHei */
        "simsun.ttc",   /* SimSun */
        "seguiemj.ttf", /* Segoe UI Emoji */
    };
#elif defined(__APPLE__)
    static const char* kFiles[] = {
        "Helvetica.ttc",            /* universal */
        "PingFang.ttc",             /* CJK */
        "Apple Color Emoji.ttc",    /* emoji */
    };
#else
    /* Linux/BSD: DejaVu (universal), Noto CJK, Noto emoji, FreeSans */
    static const char* kFiles[] = {
        "DejaVuSans.ttf",
        "FreeSans.ttf",
        "NotoSansCJK-Regular.ttc",
        "NotoColorEmoji.ttf",
    };
#endif
    const char* dirs[4];
    int ndirs = whaleui_platform_system_font_dirs(dirs, 4);
    int added = 0;
    const int kN = static_cast<int>(sizeof(kFiles) / sizeof(kFiles[0]));
    for (int d = 0; d < ndirs && added < kN; ++d) {
        for (size_t i = 0; i < static_cast<size_t>(kN); ++i) {
            if (added >= kN) {
                break;
            }
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/%s", dirs[d], kFiles[i]);
            /* skip if this family is already registered */
            whaleui_font_registry* reg = registry();
            bool known = false;
            for (size_t j = 0; j < reg->count; ++j) {
                const char* fam = reg->fonts[j].family;
                size_t blen = std::strlen(kFiles[i]);
                const char* dot = std::strrchr(fam, '.');
                size_t flen = dot ? static_cast<size_t>(dot - fam) : std::strlen(fam);
                if (flen >= blen - 4 && std::strncmp(fam, kFiles[i], flen) == 0) {
                    known = true;
                    break;
                }
            }
            if (known) {
                continue;
            }
            /* register (path only - the file is read lazily on first use,
             * and a missing file simply never opens) */
            if (whaleui_font_register(path) != nullptr) {
                added++;
            }
        }
    }
    return added;
}
