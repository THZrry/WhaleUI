/* Encoding of documents/stylesheet buffers fed to the parsers.
 *
 * Windows editors write UTF-8 with a BOM and UTF-16 (Notepad's "Unicode")
 * with FF FE / FE FF. Feeding UTF-16 to a UTF-8 parser collapses the whole
 * buffer to its first ASCII character: 0xFF/0xFE are illegal lead bytes and
 * every 0x00 suffix reads as a string terminator (a 45KB UTF-16 log rendered
 * as a single "C"). Both entry points (DOM and CSS) take their bytes through
 * wui_utf8_view so the encoding is handled once, at the edge.
 *
 * The returned pointer is either `in` itself or the converted bytes inside
 * `scratch`; `scratch` must outlive the parse. */
#ifndef WHALEUI_CORE_UTF_H
#define WHALEUI_CORE_UTF_H

#include <cstddef>
#include <string>

inline void wui_put_utf8(std::string& out, unsigned int cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/* UTF-16 (either endianness) -> UTF-8. Surrogate pairs are combined; a lone
 * NUL is dropped (a real document never contains U+0000, and it would only
 * truncate every downstream C string). */
inline void wui_utf16_to_utf8(const unsigned char* p, size_t n, bool be,
                              std::string& out)
{
    size_t i = 0;
    while (i + 1 < n) {
        unsigned int u = be ? ((static_cast<unsigned int>(p[i]) << 8) | p[i + 1])
                            : ((static_cast<unsigned int>(p[i + 1]) << 8) | p[i]);
        i += 2;
        if (u == 0) {
            continue;
        }
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            unsigned int lo = be
                                  ? ((static_cast<unsigned int>(p[i]) << 8) | p[i + 1])
                                  : ((static_cast<unsigned int>(p[i + 1]) << 8) | p[i]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                i += 2;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
        }
        wui_put_utf8(out, u);
    }
}

inline const char* wui_utf8_view(const char* in, size_t len,
                                 std::string& scratch, size_t* out_len)
{
    *out_len = len;
    if (!in || len < 2) {
        return in;
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in);
    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        *out_len = len - 3;
        return in + 3;
    }
    if (p[0] == 0xFF && p[1] == 0xFE) {
        scratch.clear();
        scratch.reserve(len);
        wui_utf16_to_utf8(p + 2, len - 2, false, scratch);
        *out_len = scratch.size();
        return scratch.data();
    }
    if (p[0] == 0xFE && p[1] == 0xFF) {
        scratch.clear();
        scratch.reserve(len);
        wui_utf16_to_utf8(p + 2, len - 2, true, scratch);
        *out_len = scratch.size();
        return scratch.data();
    }
    return in;
}

#endif /* WHALEUI_CORE_UTF_H */
