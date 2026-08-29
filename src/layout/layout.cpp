/* Layout: small box/flex engine.
 *
 * Lexbor parses HTML/CSS but computes no layout, so this module implements
 * the layout pass itself: box model (margin/border/padding/content),
 * block flow, a basic flex layout, position (static/relative/absolute/
 * fixed), z-index and opacity. The result is a flat tree the renderer
 * walks. Layout-related CSS properties are the ones that matter here -
 * see README-css.md. */

#include "layout/layout.h"

#include "animate/animate.h"

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <cstdlib>
#include <chrono>
#include <cstring>
#include <functional>
#include <vector>

namespace {

/* --- tiny value helpers (kept local so minimal target builds without
 *      style.cpp) --- */

std::string get(const WhaleUIComputedStyle& s, const char* k)
{
    auto it = s.find(k);
    return it == s.end() ? std::string() : it->second;
}

/* split "clamp(a,b,c)" / "min(a,b)" inner args on top-level commas
 * (nested function calls keep their own commas) */
static void split_len_args(const std::string& v, size_t open,
                           std::vector<std::string>& out)
{
    size_t depth = 0;
    size_t start = open;
    for (size_t i = open; i < v.size(); ++i) {
        char c = v[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth == 0) {
                break;
            }
            --depth;
        } else if (c == ',' && depth == 0) {
            std::string arg = v.substr(start, i - start);
            size_t b = arg.find_first_not_of(" \t");
            size_t e = arg.find_last_not_of(" \t");
            if (b != std::string::npos) {
                out.push_back(arg.substr(b, e - b + 1));
            }
            start = i + 1;
        }
    }
    std::string arg = v.substr(start);
    size_t b = arg.find_first_not_of(" \t");
    size_t e = arg.find_last_not_of(" \t\r\n");
    if (b != std::string::npos) {
        out.push_back(arg.substr(b, e - b + 1));
    }
}

/* "12px"/"1.5em"/"50%"/"auto" -> px value relative to parent/base.
 * Supports math functions clamp(MIN,VAL,MAX)/min()/max() and the vw/vh
 * units (relative to viewport_w/viewport_h). unit 0=px 1=% 2=em 4=unitless.
 * Returns value in px. */
static float len_px_impl(const std::string& v, float base_px, float em_base,
                         float vp_w, float vp_h)
{
    if (v.empty() || v == "auto" || v == "none") {
        return 0;
    }
    if (v.compare(0, 6, "clamp(") == 0) {
        std::vector<std::string> args;
        split_len_args(v, 6, args);
        if (args.size() != 3) {
            return 0;
        }
        float mn = len_px_impl(args[0], base_px, em_base, vp_w, vp_h);
        float val = len_px_impl(args[1], base_px, em_base, vp_w, vp_h);
        float mx = len_px_impl(args[2], base_px, em_base, vp_w, vp_h);
        if (val < mn) {
            return mn;
        }
        if (val > mx) {
            return mx;
        }
        return val;
    }
    if (v.compare(0, 4, "min(") == 0) {
        std::vector<std::string> args;
        split_len_args(v, 4, args);
        if (args.size() != 2) {
            return 0;
        }
        float a = len_px_impl(args[0], base_px, em_base, vp_w, vp_h);
        float b = len_px_impl(args[1], base_px, em_base, vp_w, vp_h);
        return a < b ? a : b;
    }
    if (v.compare(0, 4, "max(") == 0) {
        std::vector<std::string> args;
        split_len_args(v, 4, args);
        if (args.size() != 2) {
            return 0;
        }
        float a = len_px_impl(args[0], base_px, em_base, vp_w, vp_h);
        float b = len_px_impl(args[1], base_px, em_base, vp_w, vp_h);
        return a > b ? a : b;
    }
    char* end = nullptr;
    float n = std::strtof(v.c_str(), &end);
    if (end == v.c_str()) {
        return 0;
    }
    if (*end == '%') {
        return n * base_px / 100.0f;
    }
    if (end[0] == 'e' && end[1] == 'm') {
        return n * em_base;
    }
    if (end[0] == 'v' && end[1] == 'w') {
        return n * vp_w / 100.0f;
    }
    if (end[0] == 'v' && end[1] == 'h') {
        return n * vp_h / 100.0f;
    }
    return n; /* px or unitless */
}

float len_px(const std::string& v, float base_px, float em_base)
{
    return len_px_impl(v, base_px, em_base, 0, 0);
}

/* like len_px but resolves vw/vh against the viewport (used by the layout
 * pass, where the viewport size is known) */
float len_px_vp(const std::string& v, float base_px, float em_base,
                float vp_w, float vp_h)
{
    return len_px_impl(v, base_px, em_base, vp_w, vp_h);
}

/* parse a length that may be "auto" (out_auto set). "max-content" and
 * friends size to the content, same as auto for our layout. */
float len_or_auto(const std::string& v, float base_px, float em_base, bool* is_auto)
{
    if (v == "auto" || v == "max-content" || v == "min-content" ||
        v == "fit-content" || v.empty()) {
        *is_auto = true;
        return 0;
    }
    *is_auto = false;
    return len_px(v, base_px, em_base);
}

/* len_or_auto with viewport resolution for vw/vh */
float len_or_auto_vp(const std::string& v, float base_px, float em_base,
                     float vp_w, float vp_h, bool* is_auto)
{
    if (v == "auto" || v == "max-content" || v == "min-content" ||
        v == "fit-content" || v.empty()) {
        *is_auto = true;
        return 0;
    }
    *is_auto = false;
    return len_px_vp(v, base_px, em_base, vp_w, vp_h);
}

/* margin/padding/border-width shorthand: 1-4 values */
void sides(const std::string& v, int out[4], float base_px, float em_base, int def)
{
    for (int i = 0; i < 4; ++i) {
        out[i] = def;
    }
    if (v.empty()) {
        return;
    }
    std::vector<std::string> toks;
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
        toks.push_back(std::string(s, static_cast<size_t>(p - s)));
    }
    float vals[4] = {0, 0, 0, 0};
    size_t n = toks.size();
    if (n == 1) {
        vals[0] = vals[1] = vals[2] = vals[3] = len_px(toks[0], base_px, em_base);
    } else if (n == 2) {
        vals[0] = vals[2] = len_px(toks[0], base_px, em_base);
        vals[1] = vals[3] = len_px(toks[1], base_px, em_base);
    } else if (n == 3) {
        vals[0] = len_px(toks[0], base_px, em_base);
        vals[1] = vals[3] = len_px(toks[1], base_px, em_base);
        vals[2] = len_px(toks[2], base_px, em_base);
    } else if (n >= 4) {
        for (int i = 0; i < 4; ++i) {
            vals[i] = len_px(toks[i], base_px, em_base);
        }
    }
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<int>(vals[i]);
    }
}

/* are the left/right margins set to "auto"? (margin: 0 auto -> centered) */
void margin_auto_halves(const std::string& v, bool& left, bool& right)
{
    left = right = false;
    if (v.empty()) {
        return;
    }
    std::vector<std::string> toks;
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
        toks.push_back(std::string(s, static_cast<size_t>(p - s)));
    }
    size_t n = toks.size();
    bool a0 = n > 0 && toks[0] == "auto";
    bool a1 = n > 1 && toks[1] == "auto";
    bool a2 = n > 2 && toks[2] == "auto";
    bool a3 = n > 3 && toks[3] == "auto";
    if (n == 1) {
        left = right = a0;
    } else if (n == 2) {
        left = right = a1;
    } else if (n == 3) {
        left = right = a2;
    } else if (n >= 4) {
        left = a3;
        right = a1;
    }
}

int border_width(const std::string& v, float em_base)
{
    if (v.empty() || v == "none" || v == "0") {
        return 0;
    }
    return static_cast<int>(len_px(v, 0, em_base));
}

/* --- display / position kinds --- */

int display_kind(const std::string& d)
{
    if (d == "none") {
        return 0;
    }
    if (d == "flex" || d == "inline-flex") {
        return 1;
    }
    if (d == "inline" || d == "inline-block") {
        return 2;
    }
    if (d == "grid" || d == "inline-grid") {
        return 4;
    }
    return 3; /* block (default) */
}

/* flex-grow, honoring the "flex: <grow> ..." shorthand */
float flex_grow(const WhaleUIComputedStyle& s)
{
    std::string g = get(s, "flex-grow");
    if (!g.empty()) {
        return std::strtof(g.c_str(), nullptr);
    }
    std::string f = get(s, "flex");
    if (!f.empty()) {
        return std::strtof(f.c_str(), nullptr);
    }
    return 0;
}

/* estimated pixel width of a UTF-8 string: ASCII ~0.5em, CJK/fullwidth
 * glyphs ~1em (half-width estimate under-counts CJK, causing wrapped text
 * to overflow its box) */
float text_est_width(const std::string& s, float fs)
{
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            w += fs * 0.5f;
            ++i;
        } else {
            size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            w += fs * 0.95f;
            i += n;
        }
    }
    return w;
}

/* renderer-installed real text metric (NULL in pure layout tests) */
static whaleui_text_metric_fn g_text_metric = nullptr;
/* renderer-installed real line height (NULL uses fs*1.2) */
static whaleui_line_height_fn g_line_height = nullptr;

/* UTF-8 character count */
size_t utf8_count(const std::string& s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

/* real text width when the renderer installed a metric hook (TTF), else
 * the estimate + letter-spacing gaps */
float text_measure(const std::string& s, float fs, const std::string& family,
                   bool bold, float lsp_px)
{
    float w = 0;
    if (g_text_metric && !s.empty()) {
        w = g_text_metric(s.c_str(), s.size(), fs, bold, family.c_str(),
                          lsp_px);
    }
    if (w <= 0) {
        w = text_est_width(s, fs);
        if (lsp_px > 0 && s.size() > 1) {
            w += lsp_px * static_cast<float>(utf8_count(s) - 1);
        }
    }
    return w;
}

/* estimate-only width (no real-metric hook): block-level runs use this -
 * their laid-out width is a layout hint, the painted glyphs come from the
 * renderer. Keeps the hot layout path free of per-run TTF measures. */
float text_measure_est(const std::string& s, float fs, float lsp_px)
{
    float w = text_est_width(s, fs);
    if (lsp_px > 0 && s.size() > 1) {
        w += lsp_px * static_cast<float>(utf8_count(s) - 1);
    }
    return w;
}
/* font-weight: bold/bolder/600-900 render bold */
bool font_weight_bold(const WhaleUIComputedStyle& s)
{
    std::string fw = get(s, "font-weight");
    return fw == "bold" || fw == "bolder" ||
           (!fw.empty() && std::atoi(fw.c_str()) >= 600);
}

/* letter-spacing in px (em/% resolved against fs) */
float letter_spacing_px(const WhaleUIComputedStyle& s, float fs)
{
    std::string v = get(s, "letter-spacing");
    if (v.empty() || v == "normal") {
        return 0;
    }
    float n = static_cast<float>(std::atof(v.c_str()));
    return v.find("em") != std::string::npos ? n * fs : n;
}

/* estimated line count when `text` wraps at `avail` px. Short text that
 * clearly fits one line uses the fast estimate; anything that may wrap is
 * measured with the real glyph width so the laid-out line count matches
 * the painted wrap (an under-estimate left "推理能力强" splitting its last
 * character). */
/* estimated wrapped line count of one segment (no '\n'): greedy per-char
 * accumulation using the cheap per-char estimate, so the structure matches
 * the renderer's per-glyph wrap (a total-width/avail division under-counts:
 * 7 chars * 8px in a 30px box is 2 lines by division but 3 by greedy
 * wrap). Runs in O(chars) with no font calls. */
static size_t est_seg_lines(const std::string& seg, float fs, int avail,
                            float lsp_px)
{
    if (seg.empty() || avail <= 0) {
        return 1;
    }
    size_t lines = 1;
    float wacc = 0;
    size_t last_sp = std::string::npos;
    float wacc_at_sp = 0;
    size_t i = 0;
    while (i < seg.size()) {
        unsigned char c = static_cast<unsigned char>(seg[i]);
        size_t len = 1;
        float cw;
        if (c < 0x80) {
            cw = fs * 0.5f;
        } else {
            len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            cw = fs * 0.95f;
        }
        if (wacc > 0 && wacc + cw > static_cast<float>(avail)) {
            if (last_sp != std::string::npos) {
                /* break after the space; its width was already counted */
                ++lines;
                i = last_sp + 1;
                wacc = 0;
                last_sp = std::string::npos;
                continue;
            }
            ++lines;
            wacc = 0;
            continue;
        }
        wacc += cw;
        if (c == ' ') {
            last_sp = i;
            wacc_at_sp = wacc;
        }
        i += len;
    }
    (void)wacc_at_sp;
    (void)lsp_px;
    return lines;
}

size_t est_wrap_lines(const std::string& s, float fs, int avail,
                      const std::string& family, bool bold, float lsp_px)
{
    if (s.empty() || avail <= 0) {
        return 1;
    }
    size_t lines = 0;
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find('\n', p);
        if (q == std::string::npos) {
            q = s.size();
        }
        lines += est_seg_lines(s.substr(p, q - p), fs, avail, lsp_px);
        if (q == s.size()) {
            break;
        }
        p = q + 1;
    }
    if (s.back() == '\n') {
        ++lines; /* trailing newline keeps the empty last line */
    }
    return lines;
}

/* line height in px from the style: number (× fs), px, em (× fs) or % */
float line_height_px(const WhaleUIComputedStyle& s, float fs)
{
    /* absolute line-height (px/em/%) wins. A unitless value (1.2/1.5/2)
     * is relative to the font, but the renderer's text_line_h measures the
     * real font height - using the unitless factor here made laid-out
     * boxes (textarea height, scroll_max) disagree with the painted lines.
     * So unitless uses the renderer hook (falling back to fs*n in pure
     * layout tests that have no hook). */
    std::string v = get(s, "line-height");
    if (!v.empty() &&
        (v.find("px") != std::string::npos ||
         v.find("em") != std::string::npos ||
         v.find('%') != std::string::npos)) {
        if (v.find("px") != std::string::npos) {
            return static_cast<float>(std::atof(v.c_str()));
        }
        if (v.find("em") != std::string::npos) {
            return static_cast<float>(std::atof(v.c_str())) * fs;
        }
        return fs * static_cast<float>(std::atof(v.c_str())) / 100.0f;
    }
    if (g_line_height) {
        std::string fam = get(s, "font-family");
        float lh = g_line_height(fs, font_weight_bold(s), fam.c_str());
        if (lh > 0) {
            return lh;
        }
    }
    if (!v.empty()) {
        float n = static_cast<float>(std::atof(v.c_str()));
        if (n > 0) {
            return fs * n;
        }
    }
    return fs * 1.2f;
}

/* rough content width of a node: summed direct text runs + padding,
 * recursively including element children (nested flex rows, grid tracks)
 * so auto-width containers don't collapse to 1px. */
float estimate_content_width(whaleui_layout_node_t* k, float em)
{
    float fs = len_px(get(k->style, "font-size"), 0, em);
    if (fs <= 0) {
        fs = em;
    }
    /* inline boxes (b/i/span/em...) size by the REAL glyph width: the
     * painted wrap width follows the box, so an under-sized estimate
     * splits short text mid-word ("01", "V3", "推理能力强") */
    bool inline_box = display_kind(get(k->style, "display")) == 2;
    std::string fam = get(k->style, "font-family");
    bool bold = font_weight_bold(k->style);
    float lsp = letter_spacing_px(k->style, fs);
    float w = 0;
    for (whaleui_layout_node_t* c = k->first_child; c; c = c->next) {
        if (c->is_text) {
            /* width = the LONGEST line, not the whole run: measuring the
             * whole text (with \n) sums every line's width, so a pile of
             * short lines blew the box up to "the width of all lines". */
            size_t p0 = 0;
            float best = 0;
            while (p0 < c->text.size()) {
                size_t q0 = c->text.find('\n', p0);
                if (q0 == std::string::npos) {
                    q0 = c->text.size();
                }
                std::string seg = c->text.substr(p0, q0 - p0);
                float lw = inline_box
                               ? text_measure(seg, fs, fam, bold, lsp)
                               : text_measure_est(seg, fs, lsp);
                if (lw > best) {
                    best = lw;
                }
                if (q0 == c->text.size()) {
                    break;
                }
                p0 = q0 + 1;
            }
            w += best;
        } else {
            w += estimate_content_width(c, em);
        }
    }
    /* padding left/right */
    std::string p = get(k->style, "padding");
    if (!p.empty()) {
        float base = 0;
        float l = len_px(p, base, em);
        w += l * 2;
    }
    if (inline_box) {
        /* 2px safety margin: the painted wrap uses the box width, and a
         * 1px sub-pixel difference would split the last glyph */
        w += 2;
    }
    return w;
}

int position_kind(const std::string& p)
{
    if (p == "absolute") {
        return 1;
    }
    if (p == "fixed") {
        return 2;
    }
    if (p == "relative") {
        return 3;
    }
    if (p == "sticky") {
        return 4;
    }
    return 0; /* static */
}

/* A text node that is pure whitespace (source indentation/newlines) is
 * skipped - it would otherwise become a tall blank text run that inflates
 * block heights. (build() then interleaves the non-blank runs with element
 * children in DOM order so inline elements flow between their text.) */
static bool text_is_blank(const lxb_char_t* d, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        char c = static_cast<char>(d[i]);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') {
            return false;
        }
    }
    return true;
}

/* input type classification: 0 = text-like, 1 = button/other, 2 = checkbox
 * or radio (native control look, no field chrome) */
int input_kind(lxb_dom_element* el)
{
    size_t alen = 0;
    const lxb_char_t* t = lxb_dom_element_get_attribute(
        el, (const lxb_char_t*)"type", 4, &alen);
    if (t && alen > 0) {
        if ((alen == 8 && std::memcmp(t, "checkbox", 8) == 0) ||
            (alen == 5 && std::memcmp(t, "radio", 5) == 0)) {
            return 2;
        }
    }
    return 0;
}

/* tag name -> WUI_TAG_* id (ECS component). The name comes from lexbor
 * (lowercase); a length-classified comparison covers the common tags. */
int tag_id_of(lxb_dom_element* el)
{
    if (!el) {
        return WUI_TAG_UNKNOWN;
    }
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(el, &len);
    if (!name) {
        return WUI_TAG_UNKNOWN;
    }
    if (len == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
        return WUI_TAG_H1 + (name[1] - '1'); /* h1-h6 */
    }
    switch (len) {
    case 1:
        if (name[0] == 'p') return WUI_TAG_P;
        if (name[0] == 'a') return WUI_TAG_A;
        if (name[0] == 'b') return WUI_TAG_B;
        if (name[0] == 'i') return WUI_TAG_I;
        if (name[0] == 'u') return WUI_TAG_UL;
        return WUI_TAG_UNKNOWN;
    case 2:
        if (std::memcmp(name, "ul", 2) == 0) return WUI_TAG_UL;
        if (std::memcmp(name, "ol", 2) == 0) return WUI_TAG_OL;
        if (std::memcmp(name, "li", 2) == 0) return WUI_TAG_LI;
        if (std::memcmp(name, "dl", 2) == 0) return WUI_TAG_DL;
        if (std::memcmp(name, "dt", 2) == 0) return WUI_TAG_DT;
        if (std::memcmp(name, "dd", 2) == 0) return WUI_TAG_DD;
        if (std::memcmp(name, "em", 2) == 0) return WUI_TAG_EM;
        if (std::memcmp(name, "br", 2) == 0) return WUI_TAG_BR;
        if (std::memcmp(name, "hr", 2) == 0) return WUI_TAG_HR;
        if (std::memcmp(name, "tr", 2) == 0) return WUI_TAG_TR;
        if (std::memcmp(name, "td", 2) == 0) return WUI_TAG_TD;
        if (std::memcmp(name, "th", 2) == 0) return WUI_TAG_TH;
        return WUI_TAG_UNKNOWN;
    case 3:
        if (std::memcmp(name, "div", 3) == 0) return WUI_TAG_DIV;
        if (std::memcmp(name, "img", 3) == 0) return WUI_TAG_IMG;
        if (std::memcmp(name, "nav", 3) == 0) return WUI_TAG_NAV;
        if (std::memcmp(name, "pre", 3) == 0) return WUI_TAG_PRE;
        return WUI_TAG_UNKNOWN;
    case 4:
        if (std::memcmp(name, "span", 4) == 0) return WUI_TAG_SPAN;
        if (std::memcmp(name, "head", 4) == 0) return WUI_TAG_HEAD;
        if (std::memcmp(name, "body", 4) == 0) return WUI_TAG_BODY;
        if (std::memcmp(name, "html", 4) == 0) return WUI_TAG_HTML;
        if (std::memcmp(name, "main", 4) == 0) return WUI_TAG_MAIN;
        if (std::memcmp(name, "code", 4) == 0) return WUI_TAG_CODE;
        return WUI_TAG_UNKNOWN;
    case 5:
        if (std::memcmp(name, "input", 5) == 0) return WUI_TAG_INPUT;
        if (std::memcmp(name, "label", 5) == 0) return WUI_TAG_LABEL;
        if (std::memcmp(name, "meter", 5) == 0) return WUI_TAG_METER;
        if (std::memcmp(name, "table", 5) == 0) return WUI_TAG_TABLE;
        if (std::memcmp(name, "aside", 5) == 0) return WUI_TAG_ASIDE;
        return WUI_TAG_UNKNOWN;
    case 6:
        if (std::memcmp(name, "select", 6) == 0) return WUI_TAG_SELECT;
        if (std::memcmp(name, "option", 6) == 0) return WUI_TAG_OPTION;
        if (std::memcmp(name, "button", 6) == 0) return WUI_TAG_BUTTON;
        if (std::memcmp(name, "strong", 6) == 0) return WUI_TAG_STRONG;
        if (std::memcmp(name, "header", 6) == 0) return WUI_TAG_HEADER;
        if (std::memcmp(name, "footer", 6) == 0) return WUI_TAG_FOOTER;
        return WUI_TAG_UNKNOWN;
    case 7:
        if (std::memcmp(name, "details", 7) == 0) return WUI_TAG_DETAILS;
        if (std::memcmp(name, "summary", 7) == 0) return WUI_TAG_SUMMARY;
        if (std::memcmp(name, "article", 7) == 0) return WUI_TAG_ARTICLE;
        if (std::memcmp(name, "section", 7) == 0) return WUI_TAG_SECTION;
        return WUI_TAG_UNKNOWN;
    case 8:
        if (std::memcmp(name, "textarea", 8) == 0) return WUI_TAG_TEXTAREA;
        if (std::memcmp(name, "progress", 8) == 0) return WUI_TAG_PROGRESS;
        return WUI_TAG_UNKNOWN;
    case 10:
        if (std::memcmp(name, "blockquote", 10) == 0) return WUI_TAG_BLOCKQUOTE;
        return WUI_TAG_UNKNOWN;
    default:
        return WUI_TAG_UNKNOWN;
    }
}

struct Builder
{
    whaleui_layout_tree_t* tree;
    const whaleui_css_rule_t* rules;
    size_t rule_count;
    std::map<std::string, std::string> vars;
    whaleui_style_state st;
    const std::map<lxb_dom_element*, int>* scrolls;
    whaleui_anim_t* anim;
    float text_scale;

    whaleui_layout_node_t* new_node()
    {
        tree->arena.emplace_back();
        return &tree->arena.back();
    }

    /* content of a ::before(1)/::after(2) pseudo-element for el: first
     * matching rule wins, surrounding quotes stripped */
    void pseudo_content(lxb_dom_element* el, int which, std::string& out)
    {
        out.clear();
        if (!el || !rules) {
            return;
        }
        for (size_t i = 0; i < rule_count; ++i) {
            const char* sel = rules[i].selector;
            /* fast filter: only pseudo rules can produce content */
            if (!sel || (std::strstr(sel, "::before") == nullptr &&
                         std::strstr(sel, "::after") == nullptr)) {
                continue;
            }
            int pseudo = 0;
            if (!whaleui_style_match_pseudo(sel, el, &st, &pseudo)) {
                continue;
            }
            if (pseudo != which) {
                continue;
            }
            const char* c = whaleui_css_get_property(&rules[i], "content");
            if (!c || !*c || std::strcmp(c, "none") == 0) {
                continue;
            }
            out = c;
            if (out.size() >= 2 &&
                ((out[0] == '\'' && out[out.size() - 1] == '\'') ||
                 (out[0] == '"' && out[out.size() - 1] == '"'))) {
                out = out.substr(1, out.size() - 2);
            }
            return;
        }
    }

    /* merge the matching ::before/::after rule's paint-relevant properties
     * (color/font/decoration) into a copy of the element's computed style */
    WhaleUIComputedStyle pseudo_style(const WhaleUIComputedStyle& base,
                                      lxb_dom_element* el, int which)
    {
        WhaleUIComputedStyle s = base;
        if (!el || !rules) {
            return s;
        }
        for (size_t i = 0; i < rule_count; ++i) {
            const char* sel = rules[i].selector;
            /* fast filter: only pseudo rules carry paint for this merge */
            if (!sel || (std::strstr(sel, "::before") == nullptr &&
                         std::strstr(sel, "::after") == nullptr)) {
                continue;
            }
            int pseudo = 0;
            if (!whaleui_style_match_pseudo(sel, el, &st, &pseudo) ||
                pseudo != which) {
                continue;
            }
            for (size_t d = 0; d < rules[i].decl_count; ++d) {
                char* kv = rules[i].decls[d];
                char* eq = std::strchr(kv, '=');
                if (!eq) {
                    continue;
                }
                std::string name(kv, static_cast<size_t>(eq - kv));
                static const char* kPaint[] = {
                    "color", "font-size", "font-family", "font-weight",
                    "font-style", "letter-spacing", "text-transform",
                    "text-decoration", "background", "background-color",
                    "white-space",
                };
                for (size_t p = 0; p < sizeof(kPaint) / sizeof(kPaint[0]);
                     ++p) {
                    if (name == kPaint[p]) {
                        s[name] = resolve_var_in(eq + 1);
                        break;
                    }
                }
            }
        }
        return s;
    }

    /* resolve var(--x[, fallback]) inside a single value (mirrors the
     * cascade pass; pseudo rules are merged after whaleui_style_compute) */
    std::string resolve_var_in(const std::string& input)
    {
        std::string val = input;
        for (;;) {
            size_t start = val.find("var(");
            if (start == std::string::npos) {
                return val;
            }
            size_t end = val.find(')', start);
            if (end == std::string::npos) {
                return val;
            }
            std::string inner = val.substr(start + 4, end - start - 4);
            size_t b = inner.find_first_not_of(" \t");
            size_t e = inner.find_last_not_of(" \t");
            if (b == std::string::npos) {
                return val;
            }
            inner = inner.substr(b, e - b + 1);
            size_t comma = inner.find(',');
            std::string name = comma == std::string::npos
                                   ? inner
                                   : inner.substr(0, comma);
            size_t nb = name.find_first_not_of(" \t");
            size_t ne = name.find_last_not_of(" \t");
            name = name.substr(nb, ne - nb + 1);
            std::string repl;
            auto it = vars.find(name);
            if (it != vars.end()) {
                repl = it->second;
            } else if (comma != std::string::npos) {
                repl = inner.substr(comma + 1);
                size_t rb = repl.find_first_not_of(" \t");
                size_t re = repl.find_last_not_of(" \t");
                repl = repl.substr(rb, re - rb + 1);
            }
            val.replace(start, end - start + 1, repl);
            if (repl.empty()) {
                return val;
            }
        }
    }

    /* append a text-run node under n */
    whaleui_layout_node_t* add_run(whaleui_layout_node_t* n,
                                   const std::string& text,
                                   const WhaleUIComputedStyle& style)
    {
        tree->text_arena.push_back(text);
        whaleui_layout_node_t* t = new_node();
        t->el = n->el;
        t->parent = n;
        t->is_text = 1;
        t->visible = 1;
        t->in_inline = 0;
        t->opacity = n->opacity;
        t->text = tree->text_arena.back();
        t->style = style;
        if (!n->first_child) {
            n->first_child = t;
        } else {
            whaleui_layout_node_t* last = n->first_child;
            while (last->next) {
                last = last->next;
            }
            last->next = t;
        }
        return t;
    }

    /* build node + children; returns the node (display:none still built) */
    whaleui_layout_node_t* build(lxb_dom_element* el, whaleui_layout_node_t* parent)
    {
        whaleui_layout_node_t* n = new_node();
        tree->by_el.emplace(el, n); /* first entry wins: text runs share el */
        /* scalars reset here; style/text are STL containers (default-
         * constructed by new_node) and must NOT be memset */
        n->el = el;
        n->tag_id = tag_id_of(el);
        n->parent = parent;
        n->z = 0;
        n->opacity = 1.0f;
        n->visible = 1;
        n->is_text = 0;
        n->in_inline = 0;
        n->scroll_y = 0;
        n->scroll_max = 0;
        if (scrolls && el) {
            auto it = scrolls->find(el);
            if (it != scrolls->end() && it->second > 0) {
                n->scroll_y = it->second;
            }
        }
        std::memset(&n->border, 0, sizeof(n->border));
        std::memset(&n->content, 0, sizeof(n->content));
        std::memset(n->margin, 0, sizeof(n->margin));
        std::memset(n->padding, 0, sizeof(n->padding));
        std::memset(n->border_w, 0, sizeof(n->border_w));

        n->style = whaleui_style_compute(el, rules, rule_count, vars, &st);

        /* <img> has intrinsic size (300x150, browser default) and is
         * inline-level when the page sets nothing; explicit CSS wins */
        size_t tlen0 = 0;
        const lxb_char_t* tname0 = lxb_dom_element_local_name(el, &tlen0);
        bool is_img = tname0 && tlen0 == 3 &&
                      std::memcmp(tname0, "img", 3) == 0;
        if (is_img) {
            if (n->style.find("width") == n->style.end()) {
                n->style["width"] = "300px";
            }
            if (n->style.find("height") == n->style.end()) {
                n->style["height"] = "150px";
            }
            if (n->style.find("display") == n->style.end()) {
                n->style["display"] = "inline-block";
            }
        }
        /* form controls get the browser-default inline size (~20ch): as
         * inline-blocks their content estimate is 0 (option/value text is
         * not a text run), which would collapse them to zero width.
         * checkbox/radio are sized by the renderer (native control look)
         * and drop the field border/padding. */
        std::string cw0 = get(n->style, "width");
        bool cw_missing = cw0.empty() || cw0 == "auto";
        if (tname0 && cw_missing) {
            bool ctrl = (tlen0 == 6 && std::memcmp(tname0, "select", 6) == 0) ||
                        (tlen0 == 5 && std::memcmp(tname0, "input", 5) == 0) ||
                        (tlen0 == 8 && std::memcmp(tname0, "textarea", 8) == 0) ||
                        (tlen0 == 8 && std::memcmp(tname0, "progress", 8) == 0) ||
                        (tlen0 == 5 && std::memcmp(tname0, "meter", 5) == 0);
            if (ctrl) {
                if (tlen0 == 5 && std::memcmp(tname0, "input", 5) == 0 &&
                    input_kind(n->el) == 2) {
                    /* checkbox/radio: fixed control box, no field chrome */
                    n->style["width"] = "16px";
                    n->style["height"] = "16px";
                    n->style["padding"] = "0";
                    n->style["border"] = "none";
                    n->style["background"] = "none";
                } else if (tlen0 == 8 && std::memcmp(tname0, "progress", 8) == 0) {
                    /* progress/meter: wide track, fixed height */
                    n->style["width"] = "16em";
                    n->style["height"] = "6px";
                    n->style["padding"] = "0";
                    n->style["border"] = "none";
                } else if (tlen0 == 5 && std::memcmp(tname0, "meter", 5) == 0) {
                    n->style["width"] = "16em";
                    n->style["height"] = "6px";
                    n->style["padding"] = "0";
                    n->style["border"] = "none";
                } else if (tlen0 == 8 && std::memcmp(tname0, "textarea", 8) == 0 &&
                           n->style.find("height") == n->style.end()) {
                    /* textarea: fixed default height (browser-like); the
                     * content scrolls inside (overflow:auto) instead of
                     * growing the control as lines are added. Set even when
                     * a width is present (the width block below is gated on
                     * an absent width, so this must be its own check). */
                    n->style["width"] = "12em";
                    n->style["height"] = "60px";
                } else {
                    n->style["width"] = "12em";
                }
            }
        }
        /* textarea default height must apply regardless of the width check
         * above (an explicit width skips that block entirely) */
        if (tname0 && tlen0 == 8 && std::memcmp(tname0, "textarea", 8) == 0 &&
            input_kind(n->el) == 0) {
            if (n->style.find("height") == n->style.end()) {
                n->style["height"] = "60px";
            }
            /* textarea scrolls its content by default (browser UA style),
             * so typing more lines never stretches the control - the fixed
             * height stays and the content scrolls inside */
            if (n->style.find("overflow") == n->style.end()) {
                n->style["overflow"] = "auto";
            }
        }
        /* contenteditable elements default to inline, whose width follows
         * the text: the box would stretch with every typed character (the
         * demo's editable span grew sideways while typing). Give them the
         * form-control treatment - inline-block + a fixed default width -
         * so typing wraps instead. */
        if (tname0 && cw_missing) {
            size_t alen = 0;
            const lxb_char_t* ce = lxb_dom_element_get_attribute(
                n->el, (const lxb_char_t*)"contenteditable", 15, &alen);
            if (ce && alen > 0 && !(alen == 5 &&
                                    std::memcmp(ce, "false", 5) == 0)) {
                n->style["width"] = "12em";
                n->style["display"] = "inline-block";
            }
        }

        /* global text scale: multiply px/unitless font-size values so the
         * renderer (which reads font-size from the style) and all em-based
         * descendants scale together. Function values (clamp() etc.) are
         * left alone. */
        if (text_scale != 1.0f) {
            WhaleUIComputedStyle::iterator fit = n->style.find("font-size");
            if (fit != n->style.end() &&
                fit->second.find('(') == std::string::npos) {
                char* endp = nullptr;
                float v = std::strtof(fit->second.c_str(), &endp);
                if (endp != fit->second.c_str()) {
                    char buf[40];
                    std::snprintf(buf, sizeof(buf), "%g%s", v * text_scale,
                                  endp);
                    fit->second = buf;
                }
            }
        }

        /* animations/transitions interpolate on top of the computed style,
         * so layout (opacity, width, ...) and paint (colors) see them */
        if (anim) {
            whaleui_anim_apply(anim, el, n->style, whaleui_anim_now());
        }

        /* inherit font-size / color / font-family from parent */
        if (parent) {
            if (n->style.find("font-size") == n->style.end() &&
                parent->style.find("font-size") != parent->style.end()) {
                n->style["font-size"] = parent->style["font-size"];
            }
            if (n->style.find("color") == n->style.end() &&
                parent->style.find("color") != parent->style.end()) {
                n->style["color"] = parent->style["color"];
            }
            if (n->style.find("font-family") == n->style.end() &&
                parent->style.find("font-family") != parent->style.end()) {
                n->style["font-family"] = parent->style["font-family"];
            }
            if (n->style.find("line-height") == n->style.end() &&
                parent->style.find("line-height") != parent->style.end()) {
                n->style["line-height"] = parent->style["line-height"];
            }
            if (n->style.find("letter-spacing") == n->style.end() &&
                parent->style.find("letter-spacing") != parent->style.end()) {
                n->style["letter-spacing"] = parent->style["letter-spacing"];
            }
        }

        std::string disp = get(n->style, "display");
        if (display_kind(disp) == 0) {
            n->visible = 0;
        }
        std::string zs = get(n->style, "z-index");
        if (!zs.empty()) {
            n->z = std::atoi(zs.c_str());
        }
        std::string op = get(n->style, "opacity");
        if (!op.empty()) {
            float o = std::strtof(op.c_str(), nullptr);
            if (o < 0) {
                o = 0;
            }
            if (o > 1) {
                o = 1;
            }
            n->opacity = o;
        }
        if (parent) {
            n->opacity *= parent->opacity;
        }

        /* children in DOM order: own text runs and element children
         * interleave, so inline elements (strong/em/span/...) flow between
         * their surrounding text. ::before/::after pseudo content becomes
         * its own run (pseudo styles apply to it).
         * A <details> without the open attribute collapses: only its first
         * <summary> child renders (the clickable title). */
        std::string pre, post;
        pseudo_content(el, 1, pre);
        pseudo_content(el, 2, post);
        bool details_collapsed = false;
        if (n->el) {
            size_t dlen = 0;
            const lxb_char_t* dname = lxb_dom_element_local_name(n->el, &dlen);
            if (dname && dlen == 7 && std::memcmp(dname, "details", 7) == 0) {
                /* boolean attribute: <details open> may have no value, so
                 * has_attribute (not get_attribute) decides */
                if (!lxb_dom_element_has_attribute(
                        n->el, (const lxb_char_t*)"open", 4)) {
                    details_collapsed = true;
                }
            }
        }
        bool summary_seen = false;
        /* inline markers injected into the first text run (C++ work: the
         * engine has no ::marker or [attr] selector):
         *   <summary> in <details>  -> ▸▸collapse indicator
         *   <li> in <ul>/<ol>       -> bullet "•" / ordinal "N. " */
        std::string run_marker;
        bool run_marker_done = false;
        if (n->el && parent && parent->el) {
            size_t plen = 0;
            const lxb_char_t* pname =
                lxb_dom_element_local_name(parent->el, &plen);
            if (plen == 7 && std::memcmp(pname, "details", 7) == 0) {
                /* boolean attribute: presence (not value) decides */
                run_marker =
                    lxb_dom_element_has_attribute(
                        parent->el, (const lxb_char_t*)"open", 4)
                        ? "\xe2\x96\xbe "
                        : "\xe2\x96\xb8 ";
            } else if (plen == 2 && std::memcmp(pname, "ul", 2) == 0) {
                run_marker = "\xe2\x80\xa2 "; /* bullet */
            } else if (plen == 2 && std::memcmp(pname, "ol", 2) == 0) {
                int idx = 1;
                for (lxb_dom_node* s = n->el->node.prev; s; s = s->prev) {
                    if (s->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                        ++idx;
                    }
                }
                run_marker = std::to_string(idx) + ". ";
            }
        }
        whaleui_layout_node_t** link = &n->first_child;
        while (*link && (*link)->next) {
            link = &(*link)->next;
        }
        std::string cur_run;
        bool prev_run_space = false; /* last flushed run ends with a space */
        auto flush_run = [&]() {
            if (cur_run.empty()) {
                return;
            }
            /* add_run already appends t to the child chain */
            whaleui_layout_node_t* t = add_run(n, cur_run, n->style);
            prev_run_space = cur_run.back() == ' ' ||
                             cur_run.back() == '\t';
            link = &t->next;
            cur_run.clear();
        };
        if (n->visible && !pre.empty()) {
            whaleui_layout_node_t* t =
                add_run(n, pre, pseudo_style(n->style, el, 1));
            link = &t->next;
        }
        lxb_dom_node* c = el->node.first_child;
        while (c) {
            bool is_el = c->type == LXB_DOM_NODE_TYPE_ELEMENT;
            /* collapsed details: keep only the first <summary> child */
            if (details_collapsed && !summary_seen) {
                if (is_el) {
                    lxb_dom_element* se = lxb_dom_interface_element(c);
                    size_t slen = 0;
                    const lxb_char_t* sname =
                        lxb_dom_element_local_name(se, &slen);
                    if (sname && slen == 7 &&
                        std::memcmp(sname, "summary", 7) == 0) {
                        summary_seen = true; /* fall through: build it */
                    } else {
                        c = c->next;
                        continue;
                    }
                } else {
                    c = c->next;
                    continue;
                }
            } else if (details_collapsed) {
                break; /* everything after the summary is hidden */
            }
            if (n->visible &&
                (c->type == LXB_DOM_NODE_TYPE_TEXT ||
                 c->type == LXB_DOM_NODE_TYPE_CDATA_SECTION)) {
                const lexbor_str_t* s =
                    &lxb_dom_interface_text(c)->char_data.data;
                if (s->data && !text_is_blank(s->data, s->length)) {
                    if (cur_run.empty() && !run_marker.empty() &&
                        !run_marker_done) {
                        cur_run = run_marker;
                        run_marker_done = true;
                    }
                    const lxb_char_t* d = s->data;
                    size_t dlen = s->length;
                    /* element boundary: "a " + " b" collapses to "a b" */
                    if (cur_run.empty() && prev_run_space &&
                        (d[0] == ' ' || d[0] == '\t')) {
                        ++d;
                        --dlen;
                    }
                    cur_run.append(reinterpret_cast<const char*>(d), dlen);
                }
            } else if (is_el) {
                lxb_dom_element* e = lxb_dom_interface_element(c);
                size_t elen = 0;
                const lxb_char_t* ename = lxb_dom_element_local_name(e, &elen);
                if (ename && elen == 2 && std::memcmp(ename, "br", 2) == 0) {
                    cur_run += '\n'; /* line break inside the run */
                } else {
                    flush_run();
                    whaleui_layout_node_t* child = build(e, n);
                    if (!*link) {
                        *link = child;
                    } else {
                        (*link)->next = child;
                    }
                    link = &child->next;
                }
            }
            c = c->next;
        }
        flush_run();
        if (n->visible && !post.empty()) {
            whaleui_layout_node_t* t =
                add_run(n, post, pseudo_style(n->style, el, 2));
            link = &t->next;
        }
        return n;
    }

    /* --- layout pass --- */

    void layout(whaleui_layout_node_t* n, int cx, int cy, int cw, int ch,
                int font_px, int* cursor_y)
    {
        if (!n->visible) {
            return;
        }
        bool text_run = n->is_text != 0;
        float em = font_px > 0 ? static_cast<float>(font_px) : 16.0f;

        if (text_run) {
            /* inline text: width approximated, real metrics at render time */
            int fs = static_cast<int>(len_px_vp(get(n->style, "font-size"), 0, em,
                                               static_cast<float>(tree->viewport_w),
                                               static_cast<float>(tree->viewport_h)));
            if (fs <= 0) {
                fs = font_px > 0 ? font_px : 16;
            }
            /* multi-line text (\n from <br> or textarea content): height
             * scales with the line count, width with the longest line.
             * Long text also wraps to the parent content width: estimate
             * the wrapped line count (avg glyph ~ fs/2) so the box flows
             * like the renderer will paint it. */
            size_t max_line = 0, cur = 0, lines = 1;
            for (size_t i = 0; i < n->text.size(); ++i) {
                if (n->text[i] == '\n') {
                    ++lines;
                    if (cur > max_line) {
                        max_line = cur;
                    }
                    cur = 0;
                } else {
                    ++cur;
                }
            }
            if (cur > max_line) {
                max_line = cur;
            }
            int avail = cw > 0 ? cw : 0x7FFFFFFF;
            /* white-space: nowrap -> no wrapping (ticker tracks etc.) */
            if (get(n->style, "white-space") == "nowrap") {
                avail = 0x7FFFFFFF;
            }
            std::string fam2 = get(n->style, "font-family");
            bool bold2 = font_weight_bold(n->style);
            float lsp2 = letter_spacing_px(n->style, static_cast<float>(fs));
            /* wrap estimate (estimate-only for block runs: fast path);
             * longest run (a single \n-free line) sets width */
            size_t wrap_lines = est_wrap_lines(n->text, fs, avail, fam2,
                                               bold2, lsp2);
            if (wrap_lines > lines) {
                lines = wrap_lines;
            }
            n->border.x = cx;
            n->border.y = *cursor_y;
            float max_w = 0;
            for (size_t i = 0; i <= n->text.size();) {
                size_t j = n->text.find('\n', i);
                if (j == std::string::npos) {
                    j = n->text.size();
                }
                float lw = text_measure_est(n->text.substr(i, j - i), fs,
                                            lsp2);
                if (lw > max_w) {
                    max_w = lw;
                }
                if (j == n->text.size()) {
                    break;
                }
                i = j + 1;
            }
            int bw2 = static_cast<int>(max_w);
            n->border.w = bw2 > avail ? avail : bw2;
            /* line-height: number/px/em/% replaces the fixed 1.2 factor */
            float lh = line_height_px(n->style, fs);
            n->border.h = static_cast<int>(lh) * static_cast<int>(lines);
            n->content = n->border;
            *cursor_y += n->border.h;
            return;
        }

        /* margins/padding/border */
        int m[4], p[4];
        sides(get(n->style, "margin"), m, static_cast<float>(cw), em, 0);
        sides(get(n->style, "padding"), p, static_cast<float>(cw), em, 0);
        /* margin: X auto centers a fixed-width block ("0 auto" -> centered) */
        bool ml_auto = false, mr_auto = false;
        margin_auto_halves(get(n->style, "margin"), ml_auto, mr_auto);
        static const char* kBorderWidth[] = {
            "border-top-width", "border-right-width",
            "border-bottom-width", "border-left-width",
        };
        for (int i = 0; i < 4; ++i) {
            n->margin[i] = m[i];
            n->padding[i] = p[i];
            n->border_w[i] = border_width(get(n->style, kBorderWidth[i]), em);
        }
        /* border shorthand "border: 1px solid red" / border-width */
        std::string border = get(n->style, "border");
        if (!border.empty()) {
            char* end = nullptr;
            float bw = std::strtof(border.c_str(), &end);
            if (end != border.c_str()) {
                for (int i = 0; i < 4; ++i) {
                    n->border_w[i] = static_cast<int>(bw);
                }
            }
        }
        std::string bw_all = get(n->style, "border-width");
        if (!bw_all.empty()) {
            int bw4[4];
            sides(bw_all, bw4, static_cast<float>(cw), em, 0);
            for (int i = 0; i < 4; ++i) {
                n->border_w[i] = bw4[i];
            }
        }

        /* width/height */
        bool w_auto = true, h_auto = true;
        float vw = static_cast<float>(tree->viewport_w);
        float vh = static_cast<float>(tree->viewport_h);
        float wpx = len_or_auto_vp(get(n->style, "width"), static_cast<float>(cw), em, vw, vh, &w_auto);
        float hpx = len_or_auto_vp(get(n->style, "height"), static_cast<float>(ch), em, vw, vh, &h_auto);
        std::string box_sizing = get(n->style, "box-sizing");
        bool border_box = box_sizing == "border-box";

        /* position */
        int pkind = position_kind(get(n->style, "position"));
        std::string pos_v = get(n->style, "position");
        float off_top = len_px_vp(get(n->style, "top"), static_cast<float>(ch), em, vw, vh);
        float off_left = len_px_vp(get(n->style, "left"), static_cast<float>(cw), em, vw, vh);
        float off_right = len_px_vp(get(n->style, "right"), static_cast<float>(cw), em, vw, vh);
        float off_bottom = len_px_vp(get(n->style, "bottom"), static_cast<float>(ch), em, vw, vh);
        bool has_left = !get(n->style, "left").empty();
        bool has_right = !get(n->style, "right").empty();
        bool has_top = !get(n->style, "top").empty();
        bool has_bottom = !get(n->style, "bottom").empty();

        int x = cx, y = *cursor_y;
        int avail_w = cw;
        if (pkind == 2) {
            /* fixed: laid out against the viewport (immune to ancestor
             * scroll); the renderer zeroes ancestor offsets when painting */
            avail_w = tree->viewport_w;
        }

        int my = m[0] + m[2], mx = m[1] + m[3];

        /* box width */
        int bw;
        if (w_auto) {
            if (display_kind(get(n->style, "display")) == 2) {
                /* inline / inline-block shrink to content: the preferred
                 * width is the unwrapped content, capped to the available
                 * width so long editable text wraps instead of stretching
                 * the box (width follows the longest wrapped line) */
                int pref = static_cast<int>(estimate_content_width(n, em)) +
                           mx;
                int cap = avail_w - mx;
                bw = pref < cap ? pref : cap;
            } else {
                bw = avail_w - mx;
            }
        } else {
            bw = static_cast<int>(wpx);
            if (!border_box) {
                /* content-box: width is the content width, add padding/border */
                bw += p[1] + p[3] + n->border_w[1] + n->border_w[3];
            }
        }
        if (bw < 0) {
            bw = 0;
        }
        /* auto horizontal margins center a fixed-width block in the
         * available space ("margin: 0 auto" -> the page column stays
         * centered when the window is resized) */
        if ((ml_auto || mr_auto) && !w_auto && (pkind == 0 || pkind == 3)) {
            int taken = bw + (ml_auto ? 0 : m[1]) + (mr_auto ? 0 : m[3]);
            int free_w = avail_w - taken;
            if (free_w < 0) {
                free_w = 0;
            }
            if (ml_auto && mr_auto) {
                x += free_w / 2;
            } else if (ml_auto) {
                x += free_w;
            }
        }
        /* resolve offsets now that bw is known: right/bottom anchor to the
         * far edge (right: -2% -> 2% past the container's right edge).
         * bottom needs the height; auto-height boxes fall back to the top
         * edge (ponytail: bottom + auto height unsupported). sticky stays
         * in flow here; paint_node pins it while scrolling. */
        int bh_est = static_cast<int>(hpx);
        if (!h_auto && !border_box) {
            bh_est += p[0] + p[2] + n->border_w[0] + n->border_w[2];
        }
        if (pkind == 1) {
            /* absolute: relative to the (positioned) ancestor */
            x = has_left ? cx + static_cast<int>(off_left)
                         : cx + cw - bw - m[3] - static_cast<int>(off_right);
            y = has_top ? cy + static_cast<int>(off_top)
                        : (has_bottom && !h_auto)
                              ? cy + ch - bh_est - static_cast<int>(off_bottom)
                              : *cursor_y;
        } else if (pkind == 2) {
            /* fixed: relative to the viewport */
            x = has_left ? static_cast<int>(off_left)
                         : tree->viewport_w - bw - m[3] - static_cast<int>(off_right);
            y = has_top ? static_cast<int>(off_top)
                        : (has_bottom && !h_auto)
                              ? tree->viewport_h - bh_est - static_cast<int>(off_bottom)
                              : 0;
        } else if (pkind == 3 || pkind == 4) {
            /* relative / sticky: shift from the static position */
            x = cx + static_cast<int>(off_left);
            y = *cursor_y + static_cast<int>(off_top);
        }
        n->border.x = x + m[1];
        n->border.y = y + m[0];
        n->border.w = bw;
        n->content.x = n->border.x + p[1] + n->border_w[1];
        n->content.y = n->border.y + p[0] + n->border_w[0];

        /* children */
        int inner_w = bw - p[1] - p[3] - n->border_w[1] - n->border_w[3];
        int inner_h = static_cast<int>(hpx);
        int kid_cursor = 0;
        int dk = display_kind(get(n->style, "display"));

        /* scrollable container: lay children out shifted up by scroll_y.
         * The absolute child coords then match what the renderer paints
         * (clipped to the container), and hit-testing needs no offset math.
         * The root element (html) always scrolls: content taller than the
         * viewport scrolls the page (browser default). */
        std::string ov = get(n->style, "overflow");
        bool scrollable = ov == "auto" || ov == "scroll" ||
                          (n == tree->root && tree->root->scroll_y > 0);
        int saved_cy = n->content.y;
        if (scrollable && n->scroll_y > 0) {
            n->content.y -= n->scroll_y;
        }

        /* position:absolute children search the nearest positioned ancestor;
         * simplified: any positioned ancestor. */
        if (dk == 1) {
            layout_flex(n, inner_w, inner_h, font_px, kid_cursor);
        } else if (dk == 4) {
            layout_grid(n, inner_w, inner_h, font_px, kid_cursor);
        } else {
            layout_block(n, inner_w, inner_h, font_px, kid_cursor);
        }

        n->content.y = saved_cy;

        /* height: explicit or content height */
        int bh;
        if (!h_auto) {
            bh = static_cast<int>(hpx);
            if (!border_box) {
                /* content-box: height is the content height */
                bh += p[0] + p[2] + n->border_w[0] + n->border_w[2];
            }
            if (bh < kid_cursor && !scrollable) {
                bh = kid_cursor; /* content can overflow; keep explicit h */
            }
        } else {
            bh = kid_cursor + p[0] + p[2] + n->border_w[0] + n->border_w[2];
        }
        n->border.h = bh;
        n->content.w = n->border.w - p[1] - p[3] - n->border_w[1] - n->border_w[3];
        n->content.h = bh - p[0] - p[2] - n->border_w[0] - n->border_w[2];

        /* scroll range for overflow:auto/scroll containers (fixed height
         * only: auto-height boxes grow with their content) */
        n->scroll_max = 0;
        if (scrollable) {
            int cmax = kid_cursor - n->content.h;
            if (cmax < 0) {
                cmax = 0;
            }
            n->scroll_max = cmax;
        }

        /* a <select> always reserves enough height for its value text;
         * keep it at 28 so the control does not grow and squeeze neighbors
         * (the popup text fits: 16px content box + the value-text nudge).
         * Text <input>s have no child text runs either (the value lives in
         * an attribute), so their height would collapse to padding+border
         * (~12px) and the painted value text would overflow the border:
         * give them the same floor. */
        if (n->el) {
            size_t tlen = 0;
            const lxb_char_t* tname = lxb_dom_element_local_name(n->el, &tlen);
            bool is_sel = tname && tlen == 6 && std::memcmp(tname, "select", 6) == 0;
            bool is_inp = tname && tlen == 5 && std::memcmp(tname, "input", 5) == 0 &&
                          input_kind(n->el) == 0;
            if ((is_sel || is_inp) && n->border.h < 28) {
                n->border.h = 28;
                n->content.h = 28 - p[0] - p[2] - n->border_w[0] - n->border_w[2];
            }
        }

        /* min/max (viewport units resolve via len_px_vp) */
        std::string mn = get(n->style, "min-width");
        std::string mw = get(n->style, "max-width");
        if (!mn.empty() && n->border.w < static_cast<int>(len_px_vp(mn, static_cast<float>(cw), em, vw, vh))) {
            n->border.w = static_cast<int>(len_px_vp(mn, static_cast<float>(cw), em, vw, vh));
        }
        if (!mw.empty() && n->border.w > static_cast<int>(len_px_vp(mw, static_cast<float>(cw), em, vw, vh))) {
            n->border.w = static_cast<int>(len_px_vp(mw, static_cast<float>(cw), em, vw, vh));
        }
        std::string mnh = get(n->style, "min-height");
        std::string mxh = get(n->style, "max-height");
        if (!mnh.empty() && n->border.h < static_cast<int>(len_px_vp(mnh, static_cast<float>(ch), em, vw, vh))) {
            n->border.h = static_cast<int>(len_px_vp(mnh, static_cast<float>(ch), em, vw, vh));
        }
        if (!mxh.empty() && n->border.h > static_cast<int>(len_px_vp(mxh, static_cast<float>(ch), em, vw, vh))) {
            n->border.h = static_cast<int>(len_px_vp(mxh, static_cast<float>(ch), em, vw, vh));
        }

        /* advance flow cursor for block flow (static/relative/sticky) */
        if (pkind == 0 || pkind == 3 || pkind == 4) {
            *cursor_y = y + m[0] + n->border.h + m[2];
        }
        if (cursor_y && pkind == 0) {
            /* nothing extra */
        }
    }

    /* is this node part of an inline line (text run or inline/inline-block
     * element in static position)? */
    bool inline_member(whaleui_layout_node_t* c)
    {
        return c->visible &&
               (c->is_text ||
                (position_kind(get(c->style, "position")) == 0 &&
                 display_kind(get(c->style, "display")) == 2));
    }

    /* estimated inline width of a line-flow node (text run or inline
     * element). Real glyph widths (metric hook) only for runs that join an
     * inline line next to another member - their x advances against the
     * painted glyphs. An isolated run (nothing inline after it) uses the
     * fast estimate: its own width is a layout hint only. */
    int inline_est_w(whaleui_layout_node_t* c, int font_px)
    {
        float em = font_px > 0 ? static_cast<float>(font_px) : 16.0f;
        if (c->is_text) {
            float fs = len_px(get(c->style, "font-size"), 0, em);
            if (fs <= 0) {
                fs = em;
            }
            bool joins = c->next && inline_member(c->next);
            if (joins) {
                return static_cast<int>(text_measure(
                    c->text, fs, get(c->style, "font-family"),
                    font_weight_bold(c->style),
                    letter_spacing_px(c->style, fs)));
            }
            return static_cast<int>(text_measure_est(
                c->text, fs, letter_spacing_px(c->style, fs)));
        }
        return static_cast<int>(estimate_content_width(c, em));
    }

    void layout_block(whaleui_layout_node_t* n, int inner_w, int inner_h,
                      int font_px, int& kid_cursor)
    {
        (void)inner_h;
        /* block flow: children stack below each other, starting at the
         * container's content origin (padding/border are already excluded
         * via content.y). Consecutive text runs and inline elements flow
         * on one horizontal line (DOM order), wrapping at the edge - so
         * <p>a <em>e</em> b</p> paints as one line, not three boxes. */
        int cursor = n->content.y;
        int right_edge = n->content.x + inner_w;
        /* the container's text-align shifts the whole line (center/right),
         * like browsers justify the line box, not each run */
        std::string talign = get(n->style, "text-align");
        whaleui_layout_node_t* c = n->first_child;
        while (c) {
            if (!c->visible) {
                c = c->next;
                continue;
            }
            int pk = position_kind(get(c->style, "position"));
            bool inl = c->is_text ||
                       (pk == 0 &&
                        display_kind(get(c->style, "display")) == 2);
            if (!inl) {
                layout(c, n->content.x, n->content.y, inner_w, inner_h,
                       font_px, &cursor);
                c = c->next;
                continue;
            }
            /* one inline line (line_top fixed, x advances; wrap restarts
             * at the container's left edge). Members are marked in_inline
             * (paint wraps them to the line remainder and skips centering)
             * and their heights are unified to the line height so the
             * glyphs sit on one visual line. */
            int line_top = cursor;
            int x = n->content.x;
            int max_h = 0;
            std::vector<whaleui_layout_node_t*> line_members;
            while (c && inline_member(c)) {
                /* a fresh line honors text-align: measure the whole run of
                 * members and shift the line start accordingly */
                if (x == n->content.x && (talign == "center" || talign == "right")) {
                    int lw = 0;
                    for (whaleui_layout_node_t* m = c; m && inline_member(m);
                         m = m->next) {
                        lw += inline_est_w(m, font_px);
                    }
                    int avail = inner_w;
                    if (lw < avail) {
                        x = talign == "center"
                                ? n->content.x + (avail - lw) / 2
                                : n->content.x + avail - lw;
                    }
                }
                int est = inline_est_w(c, font_px);
                if (x > n->content.x && x + est > right_edge) {
                    cursor = line_top + max_h;
                    line_top = cursor;
                    x = n->content.x;
                    max_h = 0;
                    for (size_t mi = 0; mi < line_members.size(); ++mi) {
                        line_members[mi]->border.h = max_h;
                    }
                    line_members.clear();
                    /* wrapped line: also honor text-align */
                    if (talign == "center" || talign == "right") {
                        int lw = 0;
                        for (whaleui_layout_node_t* m = c;
                             m && inline_member(m); m = m->next) {
                            lw += inline_est_w(m, font_px);
                        }
                        int avail = inner_w;
                        if (lw < avail) {
                            x = talign == "center"
                                    ? n->content.x + (avail - lw) / 2
                                    : n->content.x + avail - lw;
                        }
                    }
                }
                int c2 = line_top;
                layout(c, x, line_top, right_edge - x, inner_h, font_px,
                       &c2);
                c->in_inline = 1;
                /* inline members advance by the REAL width (the block-pass
                 * estimate would leave gaps/overlap against painted glyphs);
                 * text runs' laid-out width is overwritten to match so the
                 * sequence stays consistent for hit-testing */
                const int real_w = c->is_text ? est : 0;
                if (real_w > 0) {
                    /* a run wider than the line remainder soft-wraps inside
                     * itself at paint time (avail_w = right_edge - x); cap
                     * the laid-out width to the remainder so the box matches
                     * the painted wrap instead of spilling past the edge
                     * (a long textarea line would stretch the column). The
                     * text-run pass already sized the height for wrap. */
                    int rem = right_edge - x;
                    c->border.w = rem > 0 && real_w > rem ? rem : real_w;
                }
                x += c->border.w;
                if (c->border.h > max_h) {
                    max_h = c->border.h;
                }
                line_members.push_back(c);
                c = c->next;
            }
            for (size_t mi = 0; mi < line_members.size(); ++mi) {
                line_members[mi]->border.h = max_h;
            }
            cursor = line_top + max_h;
        }
        kid_cursor = cursor - n->content.y;
    }

    void layout_flex(whaleui_layout_node_t* n, int inner_w, int inner_h,
                     int font_px, int& kid_cursor)
    {
        /* collect visible children */
        std::vector<whaleui_layout_node_t*> kids;
        whaleui_layout_node_t* c = n->first_child;
        while (c) {
            if (c->visible) {
                kids.push_back(c);
            }
            c = c->next;
        }
        if (kids.empty()) {
            return;
        }
        std::string dir = get(n->style, "flex-direction");
        bool column = dir == "column" || dir == "column-reverse";
        std::string justify = get(n->style, "justify-content");
        std::string align = get(n->style, "align-items");
        float gap = len_px(get(n->style, "gap"), static_cast<float>(inner_w), font_px > 0 ? font_px : 16);
        float em = font_px > 0 ? static_cast<float>(font_px) : 16;

        /* measure each child's main-axis size (min: content) */
        std::vector<int> main_size(kids.size(), 0);
        /* min-content floor for flex-basis:0 items (see flex shorthand) */
        std::vector<int> flex_min0(kids.size(), -1);
        for (size_t i = 0; i < kids.size(); ++i) {
            whaleui_layout_node_t* k = kids[i];
            float fs = len_px(get(k->style, "font-size"), 0, em);
            if (fs <= 0) {
                fs = font_px > 0 ? font_px : 16;
            }
            bool is_auto = true;
            if (column) {
                float h = len_or_auto(get(k->style, "height"), static_cast<float>(inner_h), em, &is_auto);
                if (is_auto) {
                    h = estimate_content_width(k, em); /* rough height proxy */
                }
                main_size[i] = k->is_text
                                   ? static_cast<int>(text_measure_est(k->text, fs, letter_spacing_px(k->style, fs)))
                                   : static_cast<int>(h);
            } else {
                float w = len_or_auto(get(k->style, "width"), static_cast<float>(inner_w), em, &is_auto);
                float minc = 0;
                if (is_auto) {
                    minc = w = estimate_content_width(k, em);
                }
                /* "flex: <grow>" shorthand means flex-basis: 0% - the item
                 * sizes purely by grow. Without this a flex:1 card holding a
                 * textarea grows with every typed char (min-content = the
                 * longest line). "flex: ... auto" keeps the content basis. */
                bool basis0 = false;
                std::string flex_s = get(k->style, "flex");
                if (!flex_s.empty() &&
                    flex_s.find("auto") == std::string::npos) {
                    w = 0;
                    basis0 = true;
                }
                /* min-width caps the measurement so space-between doesn't
                 * push items past the container edge */
                std::string mnw = get(k->style, "min-width");
                if (!mnw.empty()) {
                    float m = len_px(mnw, static_cast<float>(inner_w), em);
                    if (w < m) {
                        w = m;
                    }
                }
                main_size[i] = k->is_text
                                   ? static_cast<int>(text_measure_est(k->text, fs, letter_spacing_px(k->style, fs)))
                                   : static_cast<int>(w);
                if (basis0) {
                    flex_min0[i] = static_cast<int>(minc);
                } else {
                    flex_min0[i] = -1;
                }
            }
            if (is_auto && main_size[i] == 0) {
                main_size[i] = 1; /* avoid zero-size main axis items */
            }
        }

        /* gap spacing */
        float total_gap = gap * (kids.size() > 0 ? kids.size() - 1 : 0);
        float total_main = 0;
        for (size_t i = 0; i < kids.size(); ++i) {
            total_main += main_size[i];
        }
        float free = (column ? inner_h : inner_w) - total_main - total_gap;
        if (free < 0) {
            free = 0;
        }
        /* flex-grow distributes free space */
        float grow_sum = 0;
        for (size_t i = 0; i < kids.size(); ++i) {
            float g = flex_grow(kids[i]->style);
            if (g > 0) {
                grow_sum += g;
            }
        }
        float extra = 0;
        if (grow_sum > 0 && free > 0) {
            extra = free / grow_sum;
        }

        /* leading space from justify-content */
        float lead = 0;
        float between = gap; /* spacing between items (space-between) */
        if (justify == "center") {
            lead = free / 2;
        } else if (justify == "flex-end" || justify == "end") {
            lead = free;
        } else if (justify == "space-between") {
            lead = 0;
            between = kids.size() > 1 ? free / (kids.size() - 1) : 0;
        } else if (justify == "space-around") {
            lead = kids.size() ? free / (kids.size() * 2) : 0;
        }

        float pos = lead;
        bool flex_wrap = get(n->style, "flex-wrap") == "wrap";
        float row_cursor = 0;
        int row_h = 0;
        for (size_t i = 0; i < kids.size(); ++i) {
            whaleui_layout_node_t* k = kids[i];
            float grow = flex_grow(k->style);
            int sz = main_size[i] + (grow > 0 ? static_cast<int>(extra * grow) : 0);
            if (flex_min0[i] > sz) {
                sz = flex_min0[i]; /* flex-basis:0 still respects min-content */
            }
            if (column) {
                int c = n->content.y + static_cast<int>(pos);
                layout(k, n->content.x, n->content.y, inner_w, sz, font_px, &c);
                /* advance by the item's ACTUAL box (auto-height content grows
                 * beyond the measured main size) */
                pos = (k->border.y + k->border.h) - n->content.y + between;
            } else {
                /* flex-wrap: overflow moves to a new row below */
                float row_y = n->content.y + static_cast<float>(row_cursor);
                if (flex_wrap && pos > 0 && pos + sz > inner_w) {
                    row_cursor += row_h + gap;
                    pos = 0;
                    row_h = 0;
                    row_y = n->content.y + static_cast<float>(row_cursor);
                }
                int c = static_cast<int>(row_y);
                layout(k, n->content.x + static_cast<int>(pos),
                       static_cast<int>(row_y),
                       sz, inner_h, font_px, &c);
                pos = (k->border.x + k->border.w) - n->content.x + between;
                if (k->border.h > row_h) {
                    row_h = k->border.h;
                }
                /* align-items: stretch (default) / center / flex-end */
                if (align == "center") {
                    int dy = (row_h - k->border.h) / 2;
                    k->border.y += dy;
                    k->content.y += dy; /* keep content box glued to border */
                } else if (align == "flex-end" || align == "end") {
                    int dy = row_h - k->border.h;
                    k->border.y += dy;
                    k->content.y += dy; /* keep content box glued to border */
                } else if (inner_h > 0) {
                    /* stretch: only when the container has a definite height */
                    k->border.h = inner_h;
                }
            }
        }
        if (column) {
            kid_cursor = static_cast<int>(pos - between);
        } else if (flex_wrap) {
            /* wrap: total height = last row bottom */
            kid_cursor = static_cast<int>(row_cursor + row_h);
        } else {
            /* row: the container height is the tallest item */
            int maxh = 0;
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kids[i]->border.h > maxh) {
                    maxh = kids[i]->border.h;
                }
            }
            kid_cursor = maxh;
        }
    }

    /* --- CSS Grid subset ---
     * grid-template-columns: px / fr / auto / repeat(N, ...) tracks;
     * auto-placement fills rows; "grid-column: 1/-1" spans a full row.
     * No explicit row tracks / named areas / dense packing (ponytail:
     * add when a page needs them). */

    struct GridTrack
    {
        float len; /* px or fr number */
        int type;  /* 0=px, 1=fr, 2=auto */
    };

    void parse_grid_tracks(const std::string& v, std::vector<GridTrack>& out)
    {
        size_t rp = v.find("repeat(");
        if (rp != std::string::npos) {
            size_t close = v.find(')', rp);
            if (close != std::string::npos) {
                std::string pre = v.substr(0, rp);
                std::string inner = v.substr(rp + 7, close - rp - 7);
                std::string post = v.substr(close + 1);
                parse_grid_tracks(pre, out);
                size_t comma = inner.find(',');
                if (comma != std::string::npos) {
                    int count = std::atoi(inner.substr(0, comma).c_str());
                    std::string tl = inner.substr(comma + 1);
                    std::vector<GridTrack> sub;
                    parse_grid_tracks(tl, sub);
                    if (count < 0) {
                        count = 0;
                    }
                    if (count > 64) {
                        count = 64;
                    }
                    for (int i = 0; i < count; ++i) {
                        for (size_t j = 0; j < sub.size(); ++j) {
                            out.push_back(sub[j]);
                        }
                    }
                }
                parse_grid_tracks(post, out);
                return;
            }
        }
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
            std::string tok(s, static_cast<size_t>(p - s));
            char* end = nullptr;
            float num = std::strtof(tok.c_str(), &end);
            if (end != tok.c_str() && end[0] == 'f' && end[1] == 'r') {
                out.push_back(GridTrack{num, 1});
            } else if (end != tok.c_str()) {
                out.push_back(GridTrack{len_px(tok, 0, 16), 0});
            } else {
                out.push_back(GridTrack{0, 2}); /* auto / unknown */
            }
        }
    }

    /* does the item span the whole row? (grid-column: 1/-1 or equivalents) */
    static bool grid_span_row(const whaleui_layout_node_t* k)
    {
        std::string g = get(k->style, "grid-column");
        return g == "1/-1" || g == "1 / -1" || g == "span all";
    }

    int est_node_height(whaleui_layout_node_t* k, int inner_w, float em)
    {
        bool is_auto = true;
        float h = len_or_auto(get(k->style, "height"),
                              static_cast<float>(inner_w), em, &is_auto);
        if (is_auto) {
            /* estimate the height from wrapped text (like the text-run
             * layout does): line count = chars / chars-per-line, NOT the
             * text width - using the width made long paragraphs blow up */
            float fs = len_px(get(k->style, "font-size"), 0, em);
            if (fs <= 0) {
                fs = 16;
            }
            size_t chars = 0;
            std::string all;
            for (whaleui_layout_node_t* c = k->first_child; c; c = c->next) {
                if (c->is_text) {
                    chars += c->text.size();
                    all += c->text;
                }
            }
            if (chars > 0) {
                size_t lines = est_wrap_lines(
                    all, fs, inner_w, get(k->style, "font-family"),
                    font_weight_bold(k->style),
                    letter_spacing_px(k->style, fs));
                h = static_cast<float>(lines) * line_height_px(k->style, fs);
            } else {
                /* no direct text: the height is the sum of the block-flow
                 * children (grid cells holding <b> + <span> rows etc.) */
                float sum = 0;
                for (whaleui_layout_node_t* c = k->first_child; c;
                     c = c->next) {
                    if (!c->is_text) {
                        sum += static_cast<float>(
                            est_node_height(c, inner_w, em));
                    }
                }
                h = sum;
            }
        }
        int ih = static_cast<int>(h);
        return ih > 0 ? ih : 1;
    }

    void layout_grid(whaleui_layout_node_t* n, int inner_w, int inner_h,
                     int font_px, int& kid_cursor)
    {
        std::vector<whaleui_layout_node_t*> kids;
        for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
            if (c->visible) {
                kids.push_back(c);
            }
        }
        if (kids.empty()) {
            return;
        }
        float em = font_px > 0 ? static_cast<float>(font_px) : 16;
        std::vector<GridTrack> tracks;
        parse_grid_tracks(get(n->style, "grid-template-columns"), tracks);
        if (tracks.empty()) {
            layout_block(n, inner_w, inner_h, font_px, kid_cursor);
            return;
        }
        const size_t ncols = tracks.size();
        std::string gapv = get(n->style, "gap");
        if (gapv.empty()) {
            gapv = get(n->style, "column-gap");
        }
        float col_gap = len_px(gapv, static_cast<float>(inner_w), em);
        std::string rgv = get(n->style, "row-gap");
        float row_gap = rgv.empty() ? col_gap
                                    : len_px(rgv, static_cast<float>(inner_w), em);
        std::string align = get(n->style, "align-items");

        /* column widths: fixed px / auto = content / fr = share of free */
        std::vector<int> col_w(ncols, 0);
        float fr_sum = 0;
        for (size_t i = 0; i < ncols; ++i) {
            if (tracks[i].type == 0) {
                col_w[i] = static_cast<int>(tracks[i].len);
            } else if (tracks[i].type == 1) {
                fr_sum += tracks[i].len;
            }
        }
        for (size_t i = 0; i < ncols; ++i) {
            if (tracks[i].type == 2) {
                int mx = 0;
                for (size_t j = 0; j < kids.size(); ++j) {
                    if (!grid_span_row(kids[j]) && j % ncols == i) {
                        int w = static_cast<int>(estimate_content_width(kids[j], em));
                        if (w > mx) {
                            mx = w;
                        }
                    }
                }
                col_w[i] = mx;
            }
        }
        float gap_sum = col_gap * (ncols > 1 ? static_cast<float>(ncols - 1) : 0);
        float fixed_sum = 0;
        for (size_t i = 0; i < ncols; ++i) {
            fixed_sum += static_cast<float>(col_w[i]);
        }
        float free = static_cast<float>(inner_w) - fixed_sum - gap_sum;
        if (free < 0) {
            free = 0;
        }
        float fr_unit = fr_sum > 0 ? free / fr_sum : 0;
        for (size_t i = 0; i < ncols; ++i) {
            if (tracks[i].type == 1) {
                col_w[i] = static_cast<int>(tracks[i].len * fr_unit);
            }
        }

        /* auto-placement: fill rows left to right; whole-row items start a
         * new row and occupy it alone */
        std::vector<std::vector<whaleui_layout_node_t*> > rows;
        rows.push_back(std::vector<whaleui_layout_node_t*>());
        size_t col = 0;
        for (size_t i = 0; i < kids.size(); ++i) {
            if (grid_span_row(kids[i])) {
                if (!rows.back().empty()) {
                    rows.push_back(std::vector<whaleui_layout_node_t*>());
                    col = 0;
                }
                rows.back().push_back(kids[i]);
                rows.push_back(std::vector<whaleui_layout_node_t*>());
                col = 0;
                continue;
            }
            if (col >= ncols) {
                rows.push_back(std::vector<whaleui_layout_node_t*>());
                col = 0;
            }
            rows.back().push_back(kids[i]);
            ++col;
        }
        if (rows.back().empty()) {
            rows.pop_back();
        }

        /* row height = tallest item (estimated) */
        std::vector<int> row_h(rows.size(), 0);
        for (size_t r = 0; r < rows.size(); ++r) {
            for (size_t i = 0; i < rows[r].size(); ++i) {
                int h = est_node_height(rows[r][i], inner_w, em);
                if (h > row_h[r]) {
                    row_h[r] = h;
                }
            }
            if (row_h[r] <= 0) {
                row_h[r] = 1;
            }
        }

        int y = n->content.y;
        for (size_t r = 0; r < rows.size(); ++r) {
            int x = n->content.x;
            bool whole = rows[r].size() == 1 && grid_span_row(rows[r][0]);
            for (size_t i = 0; i < rows[r].size(); ++i) {
                whaleui_layout_node_t* k = rows[r][i];
                int w = whole ? inner_w : col_w[i];
                if (w < 0) {
                    w = 0;
                }
                int cy = y;
                layout(k, x, y, w, row_h[r], font_px, &cy);
                /* align-items: center vertically centers shorter items */
                if (align == "center") {
                    int dy = (row_h[r] - k->border.h) / 2;
                    if (dy > 0) {
                        k->border.y += dy;
                        k->content.y += dy;
                        /* move the item's subtree down with it */
                        for (whaleui_layout_node_t* cc = k->first_child; cc;
                             cc = cc->next) {
                            cc->border.y += dy;
                            cc->content.y += dy;
                        }
                    }
                }
                x += w + static_cast<int>(col_gap);
            }
            y += row_h[r] + static_cast<int>(row_gap);
        }
        kid_cursor = y - n->content.y - static_cast<int>(row_gap);
        if (kid_cursor < 0) {
            kid_cursor = 0;
        }
    }
};

} // namespace

/* C wrapper for tests (the helper lives in the anonymous namespace) */
extern "C" size_t whaleui_est_wrap_lines(const char* utf8, size_t len,
                                         float fs, int avail, bool bold,
                                         const char* family, float lsp_px)
{
    return est_wrap_lines(std::string(utf8, len), fs, avail,
                          family ? family : "", bold, lsp_px);
}

extern "C" whaleui_layout_tree_t* whaleui_layout_compute(
    whaleui_dom_document_t* doc,
    const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>* theme_vars,
    int viewport_w, int viewport_h,
    const whaleui_style_state* st,
    const std::map<lxb_dom_element*, int>* scrolls,
    struct whaleui_anim* anim, float text_scale)
{
    if (!doc || viewport_w <= 0 || viewport_h <= 0) {
        return nullptr;
    }
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    lxb_dom_element* root = lxb_dom_document_element(&hd->dom_document);
    if (!root) {
        return nullptr;
    }

    whaleui_layout_tree_t* tree = new whaleui_layout_tree_t;
    tree->viewport_w = viewport_w;
    tree->viewport_h = viewport_h;

    Builder b;
    b.tree = tree;
    b.rules = rules;
    b.rule_count = count;
    b.st = st ? *st : whaleui_style_state();
    b.scrolls = scrolls;
    b.anim = anim;
    b.text_scale = text_scale > 0 ? text_scale : 1.0f;
    if (theme_vars) {
        b.vars = *theme_vars;
    }
    whaleui_style_collect_vars_full(root, rules, count, b.vars);

    whaleui_layout_node_t* n = b.build(root, nullptr);
    tree->root = n;

    /* layout the html element into the viewport */
    int cursor = 0;
    b.layout(n, 0, 0, viewport_w, viewport_h, 16, &cursor);
    /* the root element spans the whole viewport so its background covers the
     * window even when the content is shorter than the viewport; content
     * taller than the viewport makes the page scrollable (html root) */
    n->border.h = viewport_h;
    n->content.h = viewport_h;
    {
        int cmax = cursor - viewport_h;
        n->scroll_max = cmax > 0 ? cmax : 0;
    }
    return tree;
}

extern "C" void whaleui_layout_destroy(whaleui_layout_tree_t* tree)
{
    delete tree;
}

extern "C" int whaleui_layout_relayout(
    whaleui_layout_tree_t* tree,
    lxb_dom_element* el,
    const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>* theme_vars,
    const whaleui_style_state* st,
    const std::map<lxb_dom_element*, int>* scrolls,
    struct whaleui_anim* anim, float text_scale)
{
    if (!tree || !el) {
        return 1; /* nothing to do for this element */
    }
    auto found = tree->by_el.find(el);
    if (found == tree->by_el.end()) {
        return 1; /* el has no node here (other document / already gone) */
    }
    whaleui_layout_node_t* old = found->second;
    whaleui_layout_node_t* parent = old->parent;

    /* drop the old subtree from the element map (text runs share el but
     * never own the entry, so the value check keeps them in place) */
    std::function<void(whaleui_layout_node_t*)> unmap =
        [&](whaleui_layout_node_t* nd) {
            if (nd->el) {
                auto m = tree->by_el.find(nd->el);
                if (m != tree->by_el.end() && m->second == nd) {
                    tree->by_el.erase(m);
                }
            }
            for (whaleui_layout_node_t* c = nd->first_child; c;
                 c = c->next) {
                unmap(c);
            }
        };
    unmap(old);

    /* rebuild the subtree from the live DOM (same inputs as the full pass) */
    Builder b;
    b.tree = tree;
    b.rules = rules;
    b.rule_count = count;
    b.st = st ? *st : whaleui_style_state();
    b.scrolls = scrolls;
    b.anim = anim;
    b.text_scale = text_scale > 0 ? text_scale : 1.0f;
    if (theme_vars) {
        b.vars = *theme_vars;
    }
    if (tree->root && tree->root->el) {
        whaleui_style_collect_vars_full(tree->root->el, rules, count, b.vars);
    }
    whaleui_layout_node_t* fresh = b.build(el, parent);
    if (!fresh) {
        return -1;
    }

    /* splice: replace `old` with `fresh` in the parent's child chain */
    fresh->next = old->next;
    if (parent) {
        whaleui_layout_node_t** link = &parent->first_child;
        while (*link && *link != old) {
            link = &(*link)->next;
        }
        if (*link != old) {
            return -1; /* tree inconsistent: caller falls back to a rebuild */
        }
        *link = fresh;
    } else {
        tree->root = fresh;
    }

    /* re-run the box pass; untouched branches keep their computed styles
     * and are only re-positioned. Same tail as whaleui_layout_compute. */
    int cursor = 0;
    b.layout(tree->root, 0, 0, tree->viewport_w, tree->viewport_h, 16,
             &cursor);
    tree->root->border.h = tree->viewport_h;
    tree->root->content.h = tree->viewport_h;
    {
        int cmax = cursor - tree->viewport_h;
        tree->root->scroll_max = cmax > 0 ? cmax : 0;
    }
    return 0;
}

extern "C" void whaleui_layout_set_text_metric(whaleui_text_metric_fn fn)
{
    g_text_metric = fn;
}

extern "C" void whaleui_layout_set_line_height_metric(whaleui_line_height_fn fn)
{
    g_line_height = fn;
}
