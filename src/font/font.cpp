/* Font: public C API implementation.
 * Step 2: contract implementation - registry + family naming (parsed from
 * the font file's name table is step 3; for now family = file base name or
 * "memory-N"). Loading goes through the VFS as required. */

#include "font/font.h"
#include "fs/fs.h"

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
    char* data = nullptr;
    size_t len = 0;
    if (whaleui_fs_load(uri, &data, &len) != 0) {
        return nullptr;
    }
    /* data is malloc'd by fs_load; font registry borrows it */
    whaleui_font_registry* reg = registry();
    whaleui_font_t* nf = static_cast<whaleui_font_t*>(std::realloc(reg->fonts, (reg->count + 1) * sizeof(*reg->fonts)));
    if (!nf) {
        std::free(data);
        return nullptr;
    }
    reg->fonts = nf;
    whaleui_font_t* f = &reg->fonts[reg->count];
    f->family = family_from_path(uri);
    f->data = reinterpret_cast<const unsigned char*>(data);
    f->len = len;
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
    char name[32];
    std::sprintf(name, "memory-%zu", reg->count);
    f->family = dup_str(name);
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
