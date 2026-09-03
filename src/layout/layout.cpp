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
#include <cmath>
#include <cstring>
#include <functional>
#include <algorithm>
#include <vector>
#include <mutex>

namespace {

/* --- tiny value helpers (kept local so minimal target builds without
 *      style.cpp) --- */

const std::string& get(const WhaleUIComputedStyle& s, const char* k)
{
    auto it = s.find(k);
    static const std::string kEmpty;
    return it == s.end() ? kEmpty : it->second;
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
    if (v.compare(0, 5, "calc(") == 0) {
        /* calc(expr): a +/- b chains of the units above ("calc(3ch + 30px)").
         * Split on +/- outside parens and sum; each term resolves like a
         * plain length. */
        std::vector<std::string> args;
        split_len_args(v, 5, args);
        if (args.size() != 1) {
            return 0;
        }
        const std::string& expr = args[0];
        float acc = 0;
        size_t i = 0;
        int sign = 1;
        while (i < expr.size()) {
            while (i < expr.size() &&
                   (expr[i] == ' ' || expr[i] == '\t')) {
                ++i;
            }
            if (i < expr.size() && (expr[i] == '+' || expr[i] == '-')) {
                sign = expr[i] == '+' ? 1 : -1;
                ++i;
            }
            size_t start = i;
            while (i < expr.size() && expr[i] != '+' && expr[i] != '-') {
                ++i;
            }
            std::string term = expr.substr(start, i - start);
            size_t b2 = term.find_first_not_of(" \t");
            size_t e2 = term.find_last_not_of(" \t");
            if (b2 != std::string::npos) {
                term = term.substr(b2, e2 - b2 + 1);
                acc += sign * len_px_impl(term, base_px, em_base, vp_w, vp_h);
            }
            sign = 1;
        }
        return acc;
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
    if (end[0] == 'c' && end[1] == 'h') {
        return n * em_base * 0.5f; /* 1ch ~ half an em (zero width) */
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

/* resolved per-side border widths: the border-top/right/bottom/left-width
 * longhands, then the "border-width" and "border" shorthands on top (a
 * shorthand-only border - "border:1px solid red" - is invisible to the
 * longhand lookup). Shared by the box layout pass and the grid auto-track
 * sizing, so a track keeps its cell's content box wide enough for the
 * text even with a border + margin on the cell. */
void box_border_widths(const WhaleUIComputedStyle& s, float em_base,
                       int out[4])
{
    static const char* kBorderWidth[] = {
        "border-top-width", "border-right-width",
        "border-bottom-width", "border-left-width",
    };
    for (int i = 0; i < 4; ++i) {
        out[i] = border_width(get(s, kBorderWidth[i]), em_base);
    }
    std::string all = get(s, "border-width");
    if (!all.empty()) {
        int b4[4];
        sides(all, b4, 0, em_base, 0);
        for (int i = 0; i < 4; ++i) {
            out[i] = b4[i];
        }
    }
    std::string border = get(s, "border");
    if (!border.empty()) {
        char* end = nullptr;
        float bw = std::strtof(border.c_str(), &end);
        if (end != border.c_str()) {
            for (int i = 0; i < 4; ++i) {
                out[i] = static_cast<int>(bw);
            }
        }
    }
}

/* margin + padding + border in one pass (layout.cpp resolves these for
 * every box; grid auto-tracks only need the horizontal border/margin) */
struct BoxMetrics
{
    int m[4], p[4], b[4];
};
BoxMetrics box_metrics(const WhaleUIComputedStyle& s, float cw, float em)
{
    BoxMetrics r;
    /* shorthand (margin: 1px 2px) OR per-side properties
     * (margin-top/bottom/...). q21k's .kicker uses margin-bottom:22px -
     * reading only the shorthand left it 0 and the hero text sat glued to
     * the heading ("开屏文字紧凑"). */
    std::string mv = get(s, "margin");
    if (mv.empty()) {
        sides("", r.m, cw, em, 0);
    } else {
        sides(mv, r.m, cw, em, 0);
    }
    /* per-side properties override the shorthand when both are present:
     * "*{margin:0}" + ".kicker{margin-bottom:22px}" must end at 22px for
     * the bottom side, not 0 (q21k hero text glued together). */
    const char* mk[4] = {"margin-top", "margin-right", "margin-bottom",
                         "margin-left"};
    for (int i = 0; i < 4; ++i) {
        std::string side = get(s, mk[i]);
        if (!side.empty()) {
            r.m[i] = static_cast<int>(len_px(side, cw, em));
        }
    }
    std::string pv = get(s, "padding");
    if (pv.empty()) {
        sides("", r.p, cw, em, 0);
    } else {
        sides(pv, r.p, cw, em, 0);
    }
    const char* pk[4] = {"padding-top", "padding-right", "padding-bottom",
                         "padding-left"};
    for (int i = 0; i < 4; ++i) {
        std::string side = get(s, pk[i]);
        if (!side.empty()) {
            r.p[i] = static_cast<int>(len_px(side, cw, em));
        }
    }
    box_border_widths(s, em, r.b);
    return r;
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
    if (d == "table" || d == "inline-table") {
        return 5;
    }
    if (d == "table-row") {
        return 6;
    }
    if (d == "table-cell") {
        return 7;
    }
    return 3; /* block (default) */
}

/* "flex: [grow] [shrink] [basis]" shorthand + the three longhands.
 * PureLayout's flex algorithm starts from flex-basis, so all three must
 * resolve together. Defaults: grow 0, shrink 1, basis auto (-1).
 * One number ("flex: 1") = grow, basis 0% (0) - standard behavior. */
static void flex_shorthand(const WhaleUIComputedStyle& s,
                           float& grow, float& shrink, float& basis)
{
    grow = 0;
    shrink = 1;
    basis = -1; /* -1 = auto (use width/height/content) */
    std::string g = get(s, "flex-grow");
    if (!g.empty()) {
        grow = std::strtof(g.c_str(), nullptr);
    }
    std::string sh = get(s, "flex-shrink");
    if (!sh.empty()) {
        shrink = std::strtof(sh.c_str(), nullptr);
    }
    std::string b = get(s, "flex-basis");
    if (!b.empty() && b != "auto" && b != "content") {
        basis = len_px(b, 0, 16);
    }
    std::string f = get(s, "flex");
    if (f.empty() || f == "none") {
        if (f == "none") {
            grow = 0;
            shrink = 0;
            basis = -1;
        }
        return;
    }
    if (f == "auto") { /* 1 1 auto */
        grow = 1;
        shrink = 1;
        basis = -1;
        return;
    }
    /* tokenize the shorthand */
    std::vector<std::string> toks;
    const char* p = f.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char* s2 = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
        toks.push_back(std::string(s2, static_cast<size_t>(p - s2)));
    }
    if (toks.empty()) {
        return;
    }
    auto is_num = [](const std::string& t) {
        char* e = nullptr;
        std::strtof(t.c_str(), &e);
        return e != t.c_str() && *e == '\0';
    };
    auto is_basis = [&](const std::string& t) {
        return t == "auto" || t == "content" || !is_num(t);
    };
    grow = std::strtof(toks[0].c_str(), nullptr);
    if (toks.size() >= 2) {
        if (is_basis(toks[1])) {
            basis = toks[1] == "auto" || toks[1] == "content"
                        ? -1
                        : len_px(toks[1], 0, 16);
        } else {
            shrink = std::strtof(toks[1].c_str(), nullptr);
        }
    }
    if (toks.size() >= 3) {
        basis = toks[2] == "auto" || toks[2] == "content"
                    ? -1
                    : len_px(toks[2], 0, 16);
    }
    if (toks.size() == 1) {
        basis = 0; /* flex: <number> -> basis 0% */
    }
}

/* estimated pixel width of a UTF-8 string: ASCII ~0.6em (average of
 * proportional + monospace, where mono runs ~0.6em), CJK/fullwidth ~1em.
 * The old 0.5em under-counted mono labels ("Ctrl+Enter" in a <kbd>), so
 * their min-content estimate fell short of the painted width and the text
 * wrapped inside a button. */
float text_est_width(const std::string& s, float fs)
{
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            w += fs * 0.6f;
            ++i;
        } else {
            size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            /* CJK glyphs are full-width: 1em, not 0.95em. The 0.95
             * under-estimate made grid auto-tracks ~a character too
             * narrow, so "鍐呭鑷€傚簲" wrapped instead of fitting. */
            w += fs;
            i += n;
        }
    }
    return w;
}

/* renderer-installed real text metric (NULL in pure layout tests) */
static whaleui_text_metric_fn g_text_metric = nullptr;
/* renderer-installed real line height (NULL uses fs*1.2) */
static whaleui_line_height_fn g_line_height = nullptr;
/* exact wrapped-line-count hook (renderer's per-glyph wrap) */
static whaleui_wrap_lines_fn g_wrap_lines = nullptr;

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
 * the painted wrap (an under-estimate left "鎺ㄧ悊鑳藉姏寮? splitting its last
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
    /* the renderer's exact per-glyph wrap (installed during render
     * layout): same line count the paint pass will use, so run heights
     * and positions agree to the pixel. Without it, the estimate can be
     * one line short and the page bottom unreachable by ~a line. */
    if (g_wrap_lines) {
        size_t exact = g_wrap_lines(s.c_str(), s.size(), avail,
                                    static_cast<int>(fs), bold,
                                    family.c_str(), lsp_px);
        if (exact > 0) {
            return exact;
        }
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

/* line height in px from the style: number (脳 fs), px, em (脳 fs) or % */
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
float estimate_content_width(whaleui_layout_node_t* k, float em,
                             bool force_real = false)
{
    float fs = len_px(get(k->style, "font-size"), 0, em);
    if (fs <= 0) {
        fs = em;
    }
    /* a textarea's min-content is its default width: the value wraps
     * inside the box, so the un-wrapped longest line must never stretch
     * the parent (typing past the edge widened the box - "it grows
     * after the line wraps"). Other controls have no text runs. */
    if (k->el) {
        size_t tlen = 0;
        const lxb_char_t* tn = lxb_dom_element_local_name(k->el, &tlen);
        if (tlen == 8 && std::memcmp(tn, "textarea", 8) == 0) {
            return fs * 12.0f; /* 12em default */
        }
        /* checkbox/radio are native 16px controls with no text runs; a
         * flex/inline container holding one must count its width, or it
         * measures short and the following text wraps/overflows (a label
         * with a checkbox estimated 28px instead of 44+ and wrapped its
         * text onto two lines) */
        if (tlen == 5 && std::memcmp(tn, "input", 5) == 0) {
            size_t tl0 = 0;
            const lxb_char_t* tv = lxb_dom_element_get_attribute(
                k->el, (const lxb_char_t*)"type", 4, &tl0);
            bool cr = (tv && tl0 == 8 &&
                       std::memcmp(tv, "checkbox", 8) == 0) ||
                      (tv && tl0 == 5 && std::memcmp(tv, "radio", 5) == 0);
            if (cr) {
                bool wauto = true;
                float wpx =
                    len_or_auto(get(k->style, "width"), 0, em, &wauto);
                return wauto ? 16.0f : wpx;
            }
        }
    }
    /* a block container's natural width is the widest child, not the sum:
     * summing every child made a card's "content width" include hidden
     * placeholders (h2 + input + textarea + contenteditable), so the
     * flex min-content floor grew with typed text before it was needed.
     * Only inline content (text runs / inline boxes on one line) sums -
     * and a flex ROW, whose items sit side by side (an inline-flex
     * button or a tab must be at least badge+label+padding wide, or its
     * text wraps). */
    int dk = display_kind(get(k->style, "display"));
    std::string fdir = get(k->style, "flex-direction");
    bool flex_row = dk == 1 && fdir != "column" && fdir != "column-reverse";
    bool container_inline = dk == 2 || flex_row;
    /* inline boxes (b/i/span/em...) size by the REAL glyph width: the
     * painted wrap width follows the box, so an under-sized estimate
     * splits short text mid-word ("01", "V3", "鎺ㄧ悊鑳藉姏寮?).
     * Flex-row children (a button's label next to its kbd) need the same
     * real measure - the 0.5em estimate runs 10px short on mono labels
     * and the text wraps. */
    bool inline_box = dk == 2;
    /* force_real: flex items and grid auto-track cells measure with the
     * real glyph width - the 0.6em ASCII estimate runs short on mixed
     * text (a flex h1 "WhaleUI Demo" at 173px estimate wrapped its
     * "Demo" onto a second line). */
    bool measure_real = force_real || inline_box || flex_row;
    std::string fam = get(k->style, "font-family");
    bool bold = font_weight_bold(k->style);
    float lsp = letter_spacing_px(k->style, fs);
    float w = 0;
    size_t n_inline = 0; /* direct inline/flex-row children (for the gap) */
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
                float lw = measure_real
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
            if (container_inline) {
                w += best;
                ++n_inline;
            } else if (best > w) {
                w = best;
            }
        } else {
            float ew = estimate_content_width(c, em, force_real);
            bool cinline = display_kind(get(c->style, "display")) == 2;
            if (container_inline || cinline) {
                w += ew;
                ++n_inline;
            } else if (ew > w) {
                w = ew;
            }
        }
    }
    /* flex-row children sit side by side with a gap between them: the
     * natural width must include it, or the container measures short and
     * its items overflow the right edge (a <nav> with gap:26px estimated
     * 126px for 174px of links + gaps - the last link painted past the
     * header's right edge). */
    if (flex_row && n_inline > 1) {
        std::string gapv = get(k->style, "gap");
        if (gapv.empty()) {
            gapv = get(k->style, "column-gap");
        }
        if (!gapv.empty()) {
            float g = len_px(gapv, 0, em);
            if (g > 0) {
                w += g * static_cast<float>(n_inline - 1);
            }
        }
    }
    /* padding left/right (full shorthand: "7px 12px" adds 12 each side) */
    std::string p = get(k->style, "padding");
    if (!p.empty()) {
        int pad4[4] = {0, 0, 0, 0};
        sides(p, pad4, 0, em, 0);
        w += pad4[1] + pad4[3];
    }
    if (inline_box || flex_row) {
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

/* box properties a ::before/::after can carry (beyond the paint list in
 * pseudo_style): presence of any of these turns the pseudo-element into
 * a real box (state layer, focus underline) instead of a text run */
static const char* const kPseudoBoxProps[] = {
    "background", "background-color", "background-image", "position",
    "width", "height", "inset", "left", "right", "top", "bottom",
    "transform", "opacity", "border", "border-radius", "margin",
    "padding", "box-shadow", "pointer-events",
};

/* ::before/::after rule index: build() probes pseudo content/box for EVERY
 * element (4 full-rule scans per element before this index), so the scan
 * cost scales with the whole stylesheet. Bucketing the pseudo rules once
 * per rule set (keyed like style.cpp's g_rule_index) turns the per-element
 * probe into "iterate only the ::before / ::after rules". */
struct PseudoRuleIndex
{
    const whaleui_css_rule_t* rules = nullptr;
    size_t count = 0;
    std::vector<size_t> before; /* rules whose selector contains ::before */
    std::vector<size_t> after;  /* rules whose selector contains ::after */
};
PseudoRuleIndex g_pseudo_idx;
std::mutex g_pseudo_mtx;

struct Builder
{
    whaleui_layout_tree_t* tree;
    const whaleui_css_rule_t* rules;
    size_t rule_count;
    std::map<std::string, std::string> vars;
    whaleui_style_state st;

    /* bucket of rule indices carrying the pseudo-element `which` (1/2) */
    const std::vector<size_t>* pseudo_bucket(int which)
    {
        std::lock_guard<std::mutex> lk(g_pseudo_mtx);
        if (g_pseudo_idx.rules != rules || g_pseudo_idx.count != rule_count) {
            g_pseudo_idx.rules = rules;
            g_pseudo_idx.count = rule_count;
            g_pseudo_idx.before.clear();
            g_pseudo_idx.after.clear();
            for (size_t i = 0; i < rule_count; ++i) {
                const char* sel = rules[i].selector;
                if (!sel) {
                    continue;
                }
                if (std::strstr(sel, "::before") != nullptr) {
                    g_pseudo_idx.before.push_back(i);
                }
                if (std::strstr(sel, "::after") != nullptr) {
                    g_pseudo_idx.after.push_back(i);
                }
            }
        }
        return which == 1 ? &g_pseudo_idx.before : &g_pseudo_idx.after;
    }
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
        const std::vector<size_t>* bucket = pseudo_bucket(which);
        for (size_t bi = 0; bi < bucket->size(); ++bi) {
            const whaleui_css_rule_t* rl = &rules[(*bucket)[bi]];
            int pseudo = 0;
            if (!whaleui_style_match_pseudo(rl->selector, el, &st, &pseudo) ||
                pseudo != which) {
                continue;
            }
            const char* c = whaleui_css_get_property(rl, "content");
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
        const std::vector<size_t>* bucket = pseudo_bucket(which);
        for (size_t bi = 0; bi < bucket->size(); ++bi) {
            const whaleui_css_rule_t* rl = &rules[(*bucket)[bi]];
            int pseudo = 0;
            if (!whaleui_style_match_pseudo(rl->selector, el, &st, &pseudo) ||
                pseudo != which) {
                continue;
            }
            for (size_t d = 0; d < rl->decl_count; ++d) {
                char* kv = rl->decls[d];
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

    /* does any matching ::before(1)/::after(2) rule for el carry a box
     * property? A pseudo with only text properties stays a text run; one
     * with box properties becomes a real painted box. */
    bool pseudo_has_box(lxb_dom_element* el, int which)
    {
        if (!el || !rules) {
            return false;
        }
        const std::vector<size_t>* bucket = pseudo_bucket(which);
        for (size_t bi = 0; bi < bucket->size(); ++bi) {
            const whaleui_css_rule_t* rl = &rules[(*bucket)[bi]];
            int pseudo = 0;
            if (!whaleui_style_match_pseudo(rl->selector, el, &st, &pseudo) ||
                pseudo != which) {
                continue;
            }
            for (size_t d = 0; d < rl->decl_count; ++d) {
                char* kv = rl->decls[d];
                char* eq = std::strchr(kv, '=');
                if (!eq) {
                    continue;
                }
                std::string name(kv, static_cast<size_t>(eq - kv));
                for (size_t p = 0;
                     p < sizeof(kPseudoBoxProps) / sizeof(kPseudoBoxProps[0]);
                     ++p) {
                    if (name == kPseudoBoxProps[p]) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /* computed style for a pseudo-element BOX: the element's style plus
     * every matching pseudo rule's box + paint properties. The box pins to
     * the parent's content box (position:absolute + inset:0) unless the
     * pseudo rule specifies offsets/dimensions itself (the focus underline
     * sets left/right/bottom/height), and never receives pointer events. */
    WhaleUIComputedStyle pseudo_box_style(const WhaleUIComputedStyle& base,
                                          lxb_dom_element* el, int which)
    {
        WhaleUIComputedStyle s = base;
        /* the box sizes itself from its own rules, not from the owner's:
         * inherit text/font, but drop the owner's box properties (an
         * input's min-height:32px would otherwise stretch the 2px focus
         * underline to the full control height). */
        static const char* kInheritClear[] = {
            "width", "height", "min-width", "min-height", "max-width",
            "max-height", "padding", "margin", "border", "border-width",
            "border-top", "border-bottom", "border-left", "border-right",
            "display", "position", "box-sizing",
        };
        for (size_t i = 0;
             i < sizeof(kInheritClear) / sizeof(kInheritClear[0]); ++i) {
            s.erase(kInheritClear[i]);
        }
        if (el && rules) {
            const std::vector<size_t>* bucket = pseudo_bucket(which);
            for (size_t bi = 0; bi < bucket->size(); ++bi) {
                const whaleui_css_rule_t* rl = &rules[(*bucket)[bi]];
                int pseudo = 0;
                if (!whaleui_style_match_pseudo(rl->selector, el, &st,
                                                &pseudo) ||
                    pseudo != which) {
                    continue;
                }
                for (size_t d = 0; d < rl->decl_count; ++d) {
                    char* kv = rl->decls[d];
                    char* eq = std::strchr(kv, '=');
                    if (!eq) {
                        continue;
                    }
                    std::string name(kv, static_cast<size_t>(eq - kv));
                    bool box = false;
                    for (size_t p = 0;
                         p < sizeof(kPseudoBoxProps) /
                                 sizeof(kPseudoBoxProps[0]);
                         ++p) {
                        if (name == kPseudoBoxProps[p]) {
                            box = true;
                            break;
                        }
                    }
                    if (box) {
                        s[name] = resolve_var_in(eq + 1);
                    }
                }
            }
        }
        if (s.find("position") == s.end()) {
            s["position"] = "absolute";
        }
        if (s.find("inset") == s.end() && s.find("left") == s.end() &&
            s.find("right") == s.end() && s.find("top") == s.end() &&
            s.find("bottom") == s.end() && s.find("width") == s.end() &&
            s.find("height") == s.end()) {
            s["inset"] = "0";
        }
        s["pointer-events"] = "none";
        return s;
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
        t->pseudo = 0;
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
        n->pseudo = 0;
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

        /* computed style: cached per (element, interaction state) - the
         * cascade is static (animations are applied on top below) and a
         * width-animation relayout rebuilds the same element every frame.
         * Only used when an animation context is present: a plain relayout
         * (anim == NULL, e.g. after a DOM style edit) must re-cascade, the
         * cache would return the stale pre-edit style. */
        if (anim) {
            whaleui_layout_style_key sk = {el, st.hover, st.focus,
                                           st.pressed};
            auto sit = tree->style_cache.find(sk);
            if (sit != tree->style_cache.end()) {
                n->style = sit->second;
            } else {
                n->style = whaleui_style_compute(el, rules, rule_count, vars,
                                                 &st);
                tree->style_cache[sk] = n->style;
            }
        } else {
            n->style = whaleui_style_compute(el, rules, rule_count, vars,
                                             &st);
        }
        /* <img> has intrinsic size (300x150, browser default) and is
         * inline-level when the page sets nothing; explicit CSS wins */
        size_t tlen0 = 0;
        const lxb_char_t* tname0 = lxb_dom_element_local_name(el, &tlen0);
        bool is_img = tname0 && tlen0 == 3 &&
                      std::memcmp(tname0, "img", 3) == 0;
        /* <option> is a select's choice, not in-flow content: display:none
         * so it neither lays out (stacking would blow the <select> up to the
         * option count) nor paints. The dropdown reads options from the DOM
         * (select_options) and draws its own list. */
        if (tname0 && tlen0 == 6 && std::memcmp(tname0, "option", 6) == 0) {
            n->style["display"] = "none";
        }
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
        /* tables: UA default display maps them onto the grid engine
         * (table-row = a grid row, table-cell = a grid cell) */
        if (tname0 && n->style.find("display") == n->style.end()) {
            if (tlen0 == 5 && std::memcmp(tname0, "table", 5) == 0) {
                n->style["display"] = "table";
            } else if (tlen0 == 2 && std::memcmp(tname0, "tr", 2) == 0) {
                n->style["display"] = "table-row";
            } else if (tlen0 == 2 &&
                       (std::memcmp(tname0, "td", 2) == 0 ||
                        std::memcmp(tname0, "th", 2) == 0)) {
                n->style["display"] = "table-cell";
            }
        }
        /* document plumbing never renders: <head> and its children (style,
         * title, meta, link) plus <script> bodies would otherwise lay out
         * their raw text and inflate the page height by thousands of px */
        if (tname0 && n->style.find("display") == n->style.end()) {
            bool doc_meta =
                (tlen0 == 4 && std::memcmp(tname0, "head", 4) == 0) ||
                (tlen0 == 5 && std::memcmp(tname0, "style", 5) == 0) ||
                (tlen0 == 5 && std::memcmp(tname0, "title", 5) == 0) ||
                (tlen0 == 4 && std::memcmp(tname0, "meta", 4) == 0) ||
                (tlen0 == 4 && std::memcmp(tname0, "link", 4) == 0) ||
                (tlen0 == 6 && std::memcmp(tname0, "script", 6) == 0) ||
                (tlen0 == 4 && std::memcmp(tname0, "base", 4) == 0);
            if (doc_meta) {
                n->style["display"] = "none";
            }
        }
        /* form controls get the browser-default inline size (~20ch): as
         * inline-blocks their content estimate is 0 (option/value text is
         * not a text run), which would collapse them to zero width.
         * checkbox/radio are sized by the renderer (native control look)
         * and drop the field border/padding. */
        std::string cw0 = get(n->style, "width");
        bool cw_missing = cw0.empty() || cw0 == "auto";
        /* absolute/fixed controls are sized by their offsets (inset:0
         * fills the positioned ancestor), not by the UA default size */
        std::string ppos = get(n->style, "position");
        bool positioned = ppos == "absolute" || ppos == "fixed";
        if (tname0 && cw_missing && !positioned) {
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
                    /* select + text input: browser-like field height so the
                     * control doesn't stretch in a flex row (e.g. the demo
                     * header) or grow to its option count. A select's width
                     * adapts to its longest option (browser default) - the
                     * fixed 12em clipped long option labels (the value text
                     * overflowed the field). */
                    bool is_sel_tag = tname0 && tlen0 == 6 &&
                                      std::memcmp(tname0, "select", 6) == 0;
                    if (is_sel_tag && n->el) {
                        float fs2 = len_px(get(n->style, "font-size"), 0,
                                           16.0f);
                        if (fs2 <= 0) {
                            fs2 = 16.0f;
                        }
                        float maxw = 0;
                        for (lxb_dom_node* oc = n->el->node.first_child; oc;
                             oc = oc->next) {
                            if (oc->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                                continue;
                            }
                            lxb_dom_element* oe =
                                lxb_dom_interface_element(oc);
                            size_t olen = 0;
                            const lxb_char_t* on =
                                lxb_dom_element_local_name(oe, &olen);
                            if (!on || olen != 6 ||
                                std::memcmp(on, "option", 6) != 0) {
                                continue;
                            }
                            std::string txt;
                            for (lxb_dom_node* t = oe->node.first_child; t;
                                 t = t->next) {
                                if (t->type == LXB_DOM_NODE_TYPE_TEXT) {
                                    const lexbor_str_t* s =
                                        &lxb_dom_interface_text(t)
                                             ->char_data.data;
                                    if (s->data) {
                                        txt.append(
                                            reinterpret_cast<const char*>(
                                                s->data),
                                            s->length);
                                    }
                                }
                            }
                            float w = text_measure(
                                txt, fs2, get(n->style, "font-family"),
                                false, 0);
                            if (w > maxw) {
                                maxw = w;
                            }
                        }
                        char wbuf[32];
                        std::snprintf(wbuf, sizeof(wbuf), "%dpx",
                                      static_cast<int>(maxw + 26));
                        n->style["width"] = wbuf;
                    } else {
                        n->style["width"] = "12em";
                    }
                    n->style["height"] = "28px";
                }
            }
        }
        /* textarea default height must apply regardless of the width check
         * above (an explicit width skips that block entirely) */
        if (tname0 && tlen0 == 8 && std::memcmp(tname0, "textarea", 8) == 0 &&
            input_kind(n->el) == 0 && !positioned) {
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
        if (tname0 && cw_missing && !positioned) {
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
            if (n->style.find("cursor") == n->style.end() &&
                parent->style.find("cursor") != parent->style.end()) {
                n->style["cursor"] = parent->style["cursor"];
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
        n->hz = n->z > 0;
        /* paint-path flags: transform/position are tested on every node
         * during the paint walk - cache them here so paint reads bits
         * instead of std::map lookups */
        {
            std::string tfv = get(n->style, "transform");
            n->xf = !tfv.empty() && tfv != "none";
            std::string pv = get(n->style, "position");
            n->fx = pv == "fixed";
            n->sk = pv == "sticky";
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
         *   <summary> in <details>  -> 鈻糕柛collapse indicator
         *   <li> in <ul>/<ol>       -> bullet "鈥? / ordinal "N. " */
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
        if (n->visible && (pseudo_has_box(el, 1) || !pre.empty())) {
            if (pseudo_has_box(el, 1)) {
                /* box pseudo-element (::before): a real painted box (state
                 * layer / gradient overlay) with its content as a text run */
                whaleui_layout_node_t* pb = new_node();
                pb->el = el;
                pb->parent = n;
                pb->pseudo = 1;
                pb->visible = 1;
                pb->is_text = 0;
                pb->in_inline = 0;
                pb->opacity = n->opacity;
                pb->style = pseudo_box_style(n->style, el, 1);
                if (!*link) {
                    *link = pb;
                } else {
                    (*link)->next = pb;
                }
                link = &pb->next;
                if (!pre.empty()) {
                    add_run(pb, pre, pb->style);
                }
            } else {
                whaleui_layout_node_t* t =
                    add_run(n, pre, pseudo_style(n->style, el, 1));
                link = &t->next;
            }
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
        /* native controls (checkbox/radio) never get a ::after underline
         * pseudo-box, whatever the theme says: they are 16px self-painted
         * controls, and the 2px focus underline leaks below them as a
         * stray bar */
        bool nc_el = false;
        if (el) {
            size_t tlen0 = 0;
            const lxb_char_t* tv = lxb_dom_element_get_attribute(
                el, (const lxb_char_t*)"type", 4, &tlen0);
            nc_el = (tv && tlen0 == 8 && std::memcmp(tv, "checkbox", 8) == 0) ||
                    (tv && tlen0 == 5 && std::memcmp(tv, "radio", 5) == 0);
        }
        if (n->visible && !nc_el &&
            (pseudo_has_box(el, 2) || !post.empty())) {
            if (pseudo_has_box(el, 2)) {
                /* box pseudo-element (::after): focus underline / active
                 * state layer (see ::before above) */
                whaleui_layout_node_t* pb = new_node();
                pb->el = el;
                pb->parent = n;
                pb->pseudo = 2;
                pb->visible = 1;
                pb->is_text = 0;
                pb->in_inline = 0;
                pb->opacity = n->opacity;
                pb->style = pseudo_box_style(n->style, el, 2);
                if (!*link) {
                    *link = pb;
                } else {
                    (*link)->next = pb;
                }
                link = &pb->next;
                if (!post.empty()) {
                    add_run(pb, post, pb->style);
                }
            } else {
                whaleui_layout_node_t* t =
                    add_run(n, post, pseudo_style(n->style, el, 2));
                link = &t->next;
            }
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
            /* white-space: nowrap/pre -> no soft wrapping (ticker tracks,
             * code panes); <pre> keeps its \n line breaks */
            std::string ws = get(n->style, "white-space");
            if (ws == "nowrap" || ws == "pre") {
                avail = 0x7FFFFFFF;
            }
            /* an inline element's text flows with the OUTER line box: its
             * wrap width is the block ancestor's remaining line space, not
             * the inline box's own content width (~= the text width, at
             * which a line wrapping exactly at the boundary estimates 2
             * lines and the paint pass duplicates them - <code> text
             * overlapped the line below it). Walk up past inline boxes
             * (code/span/b/em...) to the first block/flex box. */
            if (avail != 0x7FFFFFFF) {
                whaleui_layout_node_t* anc = n->parent;
                while (anc && !anc->is_text &&
                       display_kind(get(anc->style, "display")) == 2 &&
                       get(anc->style, "display") == "inline") {
                    anc = anc->parent;
                }
                if (anc && anc->content.w > 0) {
                    int right = anc->content.x + anc->content.w;
                    int av2 = right - cx;
                    if (av2 > 0) {
                        avail = av2;
                    }
                }
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
            /* box width must match the measured (real) glyph width: an
             * estimate narrower than the real ink (CJK vs latin metrics,
             * letter-spacing) leaves the tail characters clipped at render
             * time. Use the real metric hook when installed. */
            float max_w = 0;
            for (size_t i = 0; i <= n->text.size();) {
                size_t j = n->text.find('\n', i);
                if (j == std::string::npos) {
                    j = n->text.size();
                }
                float lw = text_measure(n->text.substr(i, j - i), fs, fam2,
                                        bold2, lsp2);
                if (lw > max_w) {
                    max_w = lw;
                }
                if (j == n->text.size()) {
                    break;
                }
                i = j + 1;
            }
            int bw2 = static_cast<int>(max_w);
            /* an inline run's content width IS its glyph width - do NOT
             * clamp to the parent avail, or the tail characters get
             * clipped when the run box is narrower than the ink. (A block
             * container still clips its own overflow - that is the parent's
             * <div>/<p> box, not this run.) Wrapping (est_wrap_lines) keeps
             * the height right; the run box only gives the text extent. */
            n->border.w = bw2;
            /* line-height: number/px/em/% replaces the fixed 1.2 factor */
            float lh = line_height_px(n->style, fs);
            n->border.h = static_cast<int>(lh) * static_cast<int>(lines);
            n->content = n->border;
            *cursor_y += n->border.h;
            return;
        }

        /* margins/padding/border */
        BoxMetrics bm = box_metrics(n->style, static_cast<float>(cw), em);
        int m[4], p[4];
        for (int i = 0; i < 4; ++i) {
            m[i] = bm.m[i];
            p[i] = bm.p[i];
            n->margin[i] = bm.m[i];
            n->padding[i] = bm.p[i];
            n->border_w[i] = bm.b[i];
        }
        /* margin: X auto centers a fixed-width block ("0 auto" -> centered) */
        bool ml_auto = false, mr_auto = false;
        margin_auto_halves(get(n->style, "margin"), ml_auto, mr_auto);

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
        /* inset: top/right/bottom/left shorthand ("inset:0" fills a
         * positioned ancestor); longhands win when both are present */
        std::string ins = get(n->style, "inset");
        bool has_inset = !ins.empty();
        float off_top = has_inset
                            ? len_px(ins, static_cast<float>(ch), em)
                            : len_px_vp(get(n->style, "top"), static_cast<float>(ch), em, vw, vh);
        float off_left = has_inset
                             ? len_px(ins, static_cast<float>(cw), em)
                             : len_px_vp(get(n->style, "left"), static_cast<float>(cw), em, vw, vh);
        float off_right = has_inset
                              ? len_px(ins, static_cast<float>(cw), em)
                              : len_px_vp(get(n->style, "right"), static_cast<float>(cw), em, vw, vh);
        float off_bottom = has_inset
                               ? len_px(ins, static_cast<float>(ch), em)
                               : len_px_vp(get(n->style, "bottom"), static_cast<float>(ch), em, vw, vh);
        /* "inset: a b c d" expands like margin */
        if (has_inset) {
            int iv[4] = {0, 0, 0, 0};
            sides(ins, iv, static_cast<float>(cw), em, 0);
            off_top = static_cast<float>(iv[0]);
            off_right = static_cast<float>(iv[1]);
            off_bottom = static_cast<float>(iv[2]);
            off_left = static_cast<float>(iv[3]);
        }
        bool has_left = has_inset || !get(n->style, "left").empty();
        bool has_right = has_inset || !get(n->style, "right").empty();
        bool has_top = has_inset || !get(n->style, "top").empty();
        bool has_bottom = has_inset || !get(n->style, "bottom").empty();

        int x = cx, y = *cursor_y;
        int avail_w = cw;
        if (pkind == 2) {
            /* fixed: laid out against the viewport (immune to ancestor
             * scroll); the renderer zeroes ancestor offsets when painting */
            avail_w = tree->viewport_w;
        }
        /* absolute/fixed with both offsets stretch between them (inset:0
         * fills the positioned ancestor / viewport) */
        bool pabs = pkind == 1 || pkind == 2;
        if (pabs && has_top && has_bottom && h_auto) {
            int pbase_h = pkind == 2 ? tree->viewport_h : ch;
            float span = static_cast<float>(pbase_h) - off_top - off_bottom;
            if (span > 0) {
                hpx = span;
                h_auto = false;
            }
        } else if (pabs && has_bottom && !has_top && h_auto) {
            /* absolute + bottom (no top) + auto height: the box sizes to
             * its content and anchors to the bottom edge. Without this it
             * kept the in-flow cursor y ("right/bottom" corner badge sat
             * at the wrong y). */
            hpx = est_node_height(n, cw, em);
            h_auto = false;
        }

        int mx = m[1] + m[3];

        /* box width */
        int bw;
        if (w_auto) {
            if (pabs && has_left && has_right) {
                /* absolute + both horizontal offsets: the width is the
                 * space between them (inset:0 fills the ancestor).
                 * Checked BEFORE the inline shrink below: an absolute
                 * inline-block (e.g. a <textarea> or <button> with
                 * position:absolute;inset:0) must stretch to the offsets,
                 * not shrink to its content width (a 12em-default
                 * textarea laid out at 168px inside a 644px editor
                 * wrapped every line). */
                int span = avail_w - static_cast<int>(off_left) -
                           static_cast<int>(off_right) - mx;
                bw = span > 0 ? span : 0;
            } else if (pabs) {
                /* absolute/fixed with no explicit width and no both-side
                 * stretch: shrink to fit the content (browser behavior).
                 * Previously these took the whole available width, so a
                 * "left:10px; top:10px" badge stretched across the parent
                 * and a fixed corner badge spanned the whole viewport. */
                int pref = static_cast<int>(std::ceil(
                               estimate_content_width(n, em))) +
                           mx;
                int cap = avail_w - mx;
                bw = pref < cap ? pref : cap;
            } else if (display_kind(get(n->style, "display")) == 2) {
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
            /* relative / sticky: shift FROM the static position (which may
             * already include margin:auto centering above); add, not
             * overwrite - otherwise a positioned centered block
             * (position:relative + margin:0 auto) loses its centering */
            x += static_cast<int>(off_left);
            y = *cursor_y + static_cast<int>(off_top);
        }
        n->border.x = x + m[1];
        n->border.y = y + m[0];
        n->border.w = bw;
        n->content.x = n->border.x + p[1] + n->border_w[1];
        n->content.y = n->border.y + p[0] + n->border_w[0];

        /* min/max-width clamp the box BEFORE children are laid out: a
         * max-width container must not hand its pre-clamp width to them
         * (a .app{max-width:1120px} inside a 1280 viewport would otherwise
         * lay every child out at 1260px and overflow) */
        std::string mnw = get(n->style, "min-width");
        std::string mxw = get(n->style, "max-width");
        if (!mnw.empty() &&
            n->border.w < static_cast<int>(
                              len_px_vp(mnw, static_cast<float>(cw), em, vw, vh))) {
            n->border.w = static_cast<int>(
                len_px_vp(mnw, static_cast<float>(cw), em, vw, vh));
        }
        if (!mxw.empty() &&
            n->border.w > static_cast<int>(
                              len_px_vp(mxw, static_cast<float>(cw), em, vw, vh))) {
            n->border.w = static_cast<int>(
                len_px_vp(mxw, static_cast<float>(cw), em, vw, vh));
        }

        /* children */
        int inner_w = n->border.w - p[1] - p[3] - n->border_w[1] - n->border_w[3];
        int inner_h = static_cast<int>(hpx);
        if (border_box) {
            /* border-box: hpx includes padding/border - children lay out
             * inside them */
            inner_h -= p[0] + p[2] + n->border_w[0] + n->border_w[2];
        }
        if (inner_h < 0) {
            inner_h = 0;
        }
        int kid_cursor = 0;
        int dk = display_kind(get(n->style, "display"));

        /* scrollable container: lay children out shifted up by scroll_y.
         * The absolute child coords then match what the renderer paints
         * (clipped to the container), and hit-testing needs no offset math.
         * The root element (html) always scrolls: content taller than the
         * viewport scrolls the page (browser default). */
        std::string ov = get(n->style, "overflow");
        bool scrollable = !n->pseudo &&
                          (ov == "auto" || ov == "scroll" ||
                           (n == tree->root && tree->root->scroll_y > 0));
        /* children keep their DOCUMENT coordinates (no scroll bake): the
         * paint path offsets them by scroll_delta(current) at render time.
         * Baking scroll_y into content.y here is what made every scroll
         * range / auto-height computation depend on the live scroll (the
         * smax-drift bug) - the user's guidance: each element computes its
         * own scroll space from children's sizes/positions only. */
        int saved_cy = n->content.y;

        /* position:absolute children search the nearest positioned ancestor;
         * simplified: any positioned ancestor. */
        bool is_sel = false, is_inp = false;
        if (n->el) {
            size_t tlen = 0;
            const lxb_char_t* tname =
                lxb_dom_element_local_name(n->el, &tlen);
            is_sel = tname && tlen == 6 &&
                     std::memcmp(tname, "select", 6) == 0;
            is_inp = tname && tlen == 5 && std::memcmp(tname, "input", 5) == 0 &&
                     input_kind(n->el) == 0;
        }
        if (dk == 1) {
            layout_flex(n, inner_w, inner_h, font_px, kid_cursor);
        } else if (dk == 4 || dk == 5) {
            /* grid, or table (which is laid out as a grid - see the
             * collect_table_items path in layout_grid) */
            layout_grid(n, inner_w, inner_h, font_px, kid_cursor);
        } else if (!is_sel && !is_inp) {
            layout_block(n, inner_w, inner_h, font_px, kid_cursor);
        } else {
            /* a <select>/<input> renders its value from an attribute, not
             * from a child text run: do NOT lay out the option/value
             * children (they would stack and blow the control up to the
             * option count). Pseudo-element boxes (::before/::after state
             * layers, focus underline) DO lay out - they are absolutely
             * positioned against the control's content box. */
            for (whaleui_layout_node_t* c = n->first_child; c;
                 c = c->next) {
                if (c->pseudo) {
                    layout(c, n->content.x, n->content.y, inner_w, inner_h,
                           font_px, &kid_cursor);
                }
            }
        }
        /* a <select>/<input> renders its value from an attribute, not from
         * a child text run: do NOT lay out children here (the <option>s
         * would stack and blow the control up to the option count). The
         * 28px floor below sets the height; hit-testing/hover use the box. */

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
                /* content taller than an explicit height: overflow:visible
                 * keeps the content reachable (page height covers it),
                 * but overflow:hidden clips instead of stretching (a
                 * height:100% app pane must stay at its viewport share) */
                std::string ov2 = get(n->style, "overflow");
                if (ov2 != "hidden") {
                    bh = kid_cursor;
                }
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
        /* floor the control height (is_sel/is_inp from above): an inline
         * select/input reserves one value line; the popup text fits 16px
         * content box + the value-text nudge. Pseudo-element boxes keep
         * their own dimensions (a 2px focus underline must not grow). */
        if (n->el && !n->pseudo && (is_sel || is_inp) && n->border.h < 28) {
            n->border.h = 28;
            n->content.h = 28 - p[0] - p[2] - n->border_w[0] - n->border_w[2];
        }

        /* min/max-height (viewport units resolve via len_px_vp) */
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
     * element). Real glyph widths (metric hook) for EVERY text run: the
     * cheap estimate is narrower than the real ink (CJK/latin metrics) and
     * an isolated run (nothing inline after it) computed at the estimate
     * left the tail glyphs clipped by an overflow ancestor - measured for
     * the qwen 21k page ('娣? at font 200: 190px estimate vs 220px ink).
     * The measure is cached (glyph_w_cache), so the per-run cost is low. */
    int inline_est_w(whaleui_layout_node_t* c, int font_px)
    {
        float em = font_px > 0 ? static_cast<float>(font_px) : 16.0f;
        if (c->is_text) {
            float fs = len_px(get(c->style, "font-size"), 0, em);
            if (fs <= 0) {
                fs = em;
            }
            /* box pass: the width was already laid out by build (real
             * measure), so return it instead of re-measuring the string -
             * text-alignment re-reads every inline run here every frame. */
            if (c->border.w > 0) {
                return c->border.w;
            }
            /* wrap-decision width: the FIRST \n-free segment only. A run
             * with embedded newlines (a long <p> that folded onto a second
             * source line) measured whole is far wider than the line
             * remainder, so the wrap check below kicked the whole run to a
             * fresh line even though its first segment fit - the tail
             * started one line early ("用 <code>..</code> 打开任意页面..."
             * broke right after the code). The run soft-wraps internally
             * at paint time; only the first segment decides placement. */
            std::string seg = c->text;
            size_t nl = seg.find('\n');
            if (nl != std::string::npos) {
                seg = seg.substr(0, nl);
            }
            if (seg.empty()) {
                return 0;
            }
            return static_cast<int>(text_measure(
                seg, fs, get(c->style, "font-family"),
                font_weight_bold(c->style),
                letter_spacing_px(c->style, fs)));
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

    /* shift a node and its whole subtree by (dx, dy) - used by flex/grid
     * cross-axis alignment and direction reversal after layout */
    void shift_subtree(whaleui_layout_node_t* k, int dx, int dy)
    {
        k->border.x += dx;
        k->border.y += dy;
        k->content.x += dx;
        k->content.y += dy;
        for (whaleui_layout_node_t* cc = k->first_child; cc; cc = cc->next) {
            shift_subtree(cc, dx, dy);
        }
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
        float em = font_px > 0 ? static_cast<float>(font_px) : 16;
        std::string dir = get(n->style, "flex-direction");
        bool column = dir == "column" || dir == "column-reverse";
        bool reverse = dir == "row-reverse" || dir == "column-reverse";
        std::string justify = get(n->style, "justify-content");
        std::string align = get(n->style, "align-items");
        std::string align_c = get(n->style, "align-content");
        float gap = len_px(get(n->style, "gap"), static_cast<float>(inner_w), em);
        std::string rg = get(n->style, "row-gap");
        std::string cg = get(n->style, "column-gap");
        float gap_main = column
                             ? (rg.empty() ? gap : len_px(rg, static_cast<float>(inner_w), em))
                             : (cg.empty() ? gap : len_px(cg, static_cast<float>(inner_w), em));
        float gap_cross = column
                              ? (cg.empty() ? gap : len_px(cg, static_cast<float>(inner_w), em))
                              : (rg.empty() ? gap : len_px(rg, static_cast<float>(inner_w), em));

        /* order: stable sort keeps DOM order among equal orders */
        auto order_of = [](whaleui_layout_node_t* k) {
            std::string o = get(k->style, "order");
            return o.empty() ? 0 : std::atoi(o.c_str());
        };
        std::stable_sort(kids.begin(), kids.end(),
                         [&](whaleui_layout_node_t* a,
                             whaleui_layout_node_t* b) {
                             return order_of(a) < order_of(b);
                         });

        /* container main size: 0 = indefinite (auto height) */
        int container_main = column ? inner_h : inner_w;
        std::vector<float> grow(kids.size()), shrink(kids.size());
        std::vector<float> main_size(kids.size());
        std::vector<int> min_main(kids.size(), 0); /* min-content floor */
        bool any_shrink = false;
        for (size_t i = 0; i < kids.size(); ++i) {
            whaleui_layout_node_t* k = kids[i];
            /* checkbox/radio UA default 16x16 must be visible BEFORE the
             * flex main-size pass reads width: the per-node default lives
             * in layout() (1228), which runs AFTER flex sizes the items -
             * an unchecked checkbox then sized as an empty inline-block
             * (11px wide) instead of 16px and painted squashed. */
            if (k->el && input_kind(k->el) == 2) {
                if (k->style.find("width") == k->style.end()) {
                    k->style["width"] = "16px";
                }
                if (k->style.find("height") == k->style.end()) {
                    k->style["height"] = "16px";
                }
                if (k->style.find("padding") == k->style.end()) {
                    k->style["padding"] = "0";
                }
            }
            float basis = 0;
            flex_shorthand(k->style, grow[i], shrink[i], basis);
            if (shrink[i] > 0) {
                any_shrink = true;
            }
            float fs = len_px(get(k->style, "font-size"), 0, em);
            if (fs <= 0) {
                fs = font_px > 0 ? font_px : 16;
            }
            int minc = k->is_text
                           ? static_cast<int>(std::ceil(text_measure(
                                 k->text, fs, get(k->style, "font-family"),
                                 font_weight_bold(k->style),
                                 letter_spacing_px(k->style, fs)))) +
                                 1
                           : (column
                                  ? static_cast<int>(
                                        est_node_height(k, inner_w, em))
                                  : static_cast<int>(std::ceil(
                                        estimate_content_width(k, em, true))));
            std::string mn = get(k->style, column ? "min-height" : "min-width");
            if (!mn.empty()) {
                /* an explicit min-* (including min-height:0) replaces the
                 * content floor: "min-height:0" lets a flex:1 sibling
                 * shrink instead of keeping its content height */
                minc = static_cast<int>(
                    len_px(mn, static_cast<float>(inner_w), em));
            }
            std::string ovf = get(k->style, "overflow");
            if (ovf == "auto" || ovf == "scroll" || ovf == "hidden") {
                /* overflow clips/scolls: the min-content floor drops to 0
                 * (a flex:1 scroll pane must not be pushed by its content,
                 * matching the spec's min-height:auto rule) */
                if (mn.empty()) {
                    minc = 0;
                }
            }
            min_main[i] = minc;
            /* native controls (checkbox/radio) have no text content, so
             * the content-based min floor would shrink them below their
             * fixed control width; floor at the explicit width */
            if (k->el && input_kind(k->el) == 2) {
                bool wauto2 = true;
                float wpx2 = len_or_auto(get(k->style, "width"),
                                         static_cast<float>(inner_w), em,
                                         &wauto2);
                if (!wauto2 && wpx2 > min_main[i]) {
                    min_main[i] = static_cast<int>(wpx2);
                }
            }
            if (basis >= 0) {
                main_size[i] = basis; /* "flex: 1" -> basis 0, grows fully */
                if (container_main <= 0 && main_size[i] < min_main[i]) {
                    /* indefinite container: no free space to grow into,
                     * a basis:0 item sizes to its content instead */
                    main_size[i] = static_cast<float>(min_main[i]);
                }
            } else {
                bool is_auto = true;
                float sz = len_or_auto(get(k->style, column ? "height" : "width"),
                                       static_cast<float>(inner_w), em,
                                       &is_auto);
                if (is_auto) {
                    if (column) {
                        /* measure the REAL content height. A column flex
                         * container lays out like a block flow (stacked),
                         * so measuring it as block makes its flex:1
                         * children size to their content instead of 0
                         * (a definite-height flex column would grow them
                         * to the measurement box). Row containers keep
                         * their own display: the natural content height. */
                        std::string kd = get(k->style, "display");
                        std::string kdir = get(k->style, "flex-direction");
                        bool k_col =
                            (kd == "flex" || kd == "inline-flex") &&
                            (kdir == "column" || kdir == "column-reverse");
                        std::string saved_disp;
                        bool had_disp = k->style.count("display") != 0;
                        if (k_col) {
                            if (had_disp) {
                                saved_disp = k->style["display"];
                            }
                            k->style["display"] = "block";
                        }
                        int c0 = 0;
                        layout(k, 0, 0, inner_w, 0x7FFFFFFF, font_px, &c0);
                        sz = static_cast<float>(k->border.h);
                        if (k_col) {
                            if (had_disp) {
                                k->style["display"] = saved_disp;
                            } else {
                                k->style.erase("display");
                            }
                        }
                        if (sz < 1) {
                            sz = static_cast<float>(minc);
                        }
                    } else {
                        sz = static_cast<float>(minc);
                    }
                }
                main_size[i] = sz;
            }
            if (main_size[i] < 1 && grow[i] <= 0) {
                main_size[i] = 1; /* avoid zero-size main-axis items */
            }
        }

        /* split into rows by the hypothetical sizes first (wrap decides on
         * the un-shrunk sizes), then grow/shrink per row below */
        struct FlexRow
        {
            std::vector<whaleui_layout_node_t*> items;
            std::vector<float> sizes;
            int y; /* cross start */
            int est_h;
            int real_h;
        };
        std::vector<FlexRow> rows;
        {
            FlexRow cur;
            float pos = 0;
            bool flex_wrap = get(n->style, "flex-wrap") == "wrap" ||
                             get(n->style, "flex-wrap") == "wrap-reverse";
            for (size_t i = 0; i < kids.size(); ++i) {
                float sz = main_size[i];
                if (flex_wrap && pos > 0 &&
                    pos + sz > static_cast<float>(container_main)) {
                    rows.push_back(cur);
                    cur = FlexRow();
                    pos = 0;
                }
                cur.items.push_back(kids[i]);
                cur.sizes.push_back(sz);
                pos += sz + gap_main;
            }
            rows.push_back(cur);
        }

        /* estimate each row's cross size to place row starts */
        for (size_t r = 0; r < rows.size(); ++r) {
            int est = 1;
            for (size_t i = 0; i < rows[r].items.size(); ++i) {
                int h = static_cast<int>(est_node_height(
                    rows[r].items[i], inner_w, em));
                if (h > est) {
                    est = h;
                }
            }
            rows[r].est_h = est;
            rows[r].real_h = 0;
        }
        /* row start y: wrap-reverse lays the first row at the bottom */
        {
            std::string wrev = get(n->style, "flex-wrap");
            bool wrap_reverse = wrev == "wrap-reverse";
            int y = n->content.y;
            if (wrap_reverse) {
                int total = 0;
                for (size_t r = 0; r < rows.size(); ++r) {
                    total += rows[r].est_h;
                }
                total += static_cast<int>(gap_cross) *
                         (rows.size() > 1
                              ? static_cast<int>(rows.size() - 1)
                              : 0);
                for (size_t r = 0; r < rows.size(); ++r) {
                    rows[r].y = n->content.y + inner_h - total;
                    total -= rows[r].est_h + static_cast<int>(gap_cross);
                }
            } else {
                for (size_t r = 0; r < rows.size(); ++r) {
                    rows[r].y = y;
                    y += rows[r].est_h + static_cast<int>(gap_cross);
                }
            }
        }
        /* layout each item; main pos per row from justify-content */
        int column_total = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            FlexRow& row = rows[r];
            /* per-row grow/shrink: the row's own free space decides
             * (PureLayout resolveFlexibleLengths, single pass) */
            auto kid_idx = [&](whaleui_layout_node_t* k) -> size_t {
                for (size_t i = 0; i < kids.size(); ++i) {
                    if (kids[i] == k) {
                        return i;
                    }
                }
                return 0;
            };
            float row_free = static_cast<float>(container_main);
            for (size_t i = 0; i < row.sizes.size(); ++i) {
                row_free -= row.sizes[i];
            }
            row_free -= gap_main *
                        (row.sizes.size() > 1
                             ? static_cast<float>(row.sizes.size() - 1)
                             : 0);
            /* an indefinite container (auto height column) has no free
             * space: items keep their hypothetical size, no grow/shrink */
            bool definite = container_main > 0;
            if (definite && row_free > 0) {
                float gsum = 0;
                for (size_t i = 0; i < row.sizes.size(); ++i) {
                    gsum += grow[kid_idx(row.items[i])];
                }
                if (gsum > 0) {
                    float extra = row_free / gsum;
                    for (size_t i = 0; i < row.sizes.size(); ++i) {
                        size_t gi = kid_idx(row.items[i]);
                        if (grow[gi] > 0) {
                            row.sizes[i] += extra * grow[gi];
                            /* a flex item never shrinks below its
                             * min-content floor ("flex: 1" cards keep their
                             * content width even when the share is smaller) */
                            if (row.sizes[i] < min_main[gi]) {
                                row.sizes[i] = static_cast<float>(min_main[gi]);
                            }
                        }
                    }
                }
            } else if (definite && row_free < 0 && any_shrink) {
                float ssum = 0;
                for (size_t i = 0; i < row.sizes.size(); ++i) {
                    size_t gi = kid_idx(row.items[i]);
                    ssum += shrink[gi] * row.sizes[i];
                }
                if (ssum > 0) {
                    float overflow = -row_free;
                    for (size_t i = 0; i < row.sizes.size(); ++i) {
                        size_t gi = kid_idx(row.items[i]);
                        if (shrink[gi] > 0) {
                            float cut = overflow * shrink[gi] * row.sizes[i] /
                                        ssum;
                            float sz = row.sizes[i] - cut;
                            if (sz < min_main[gi]) {
                                sz = static_cast<float>(min_main[gi]);
                            }
                            row.sizes[i] = sz;
                        }
                    }
                }
            }
            /* a flex item never sizes below its explicit min-* even when
             * there is no grow/shrink pressure to clamp it there
             * (select/input with min-width:180px inside a wide row was
             * keeping its ~127px content width) */
            for (size_t i = 0; i < row.sizes.size(); ++i) {
                size_t gi = kid_idx(row.items[i]);
                if (row.sizes[i] < min_main[gi]) {
                    row.sizes[i] = static_cast<float>(min_main[gi]);
                }
            }
            /* justify-content works on the space left after grow/shrink */
            float free_left = static_cast<float>(container_main);
            for (size_t i = 0; i < row.sizes.size(); ++i) {
                free_left -= row.sizes[i];
            }
            free_left -= gap_main *
                         (row.sizes.size() > 1
                              ? static_cast<float>(row.sizes.size() - 1)
                              : 0);
            if (free_left < 0) {
                free_left = 0;
            }
            float lead = 0;
            float between = gap_main;
            if (justify == "center") {
                lead = free_left / 2;
            } else if (justify == "flex-end" || justify == "end") {
                lead = free_left;
            } else if (justify == "space-between") {
                between = row.sizes.size() > 1
                              ? free_left / (row.sizes.size() - 1)
                              : 0;
            } else if (justify == "space-around") {
                lead = free_left / (row.sizes.size() * 2);
                between = free_left / row.sizes.size();
            } else if (justify == "space-evenly") {
                lead = free_left / (row.sizes.size() + 1);
                between = free_left / (row.sizes.size() + 1);
            }
            float pos = lead;
            for (size_t i = 0; i < row.items.size(); ++i) {
                whaleui_layout_node_t* k = row.items[i];
                /* round UP: a fractional main size truncating down leaves
                 * the content box 1px narrower than the text ("涓€" in a
                 * gap test wrapped/clipped its last pixel) */
                int sz = static_cast<int>(std::ceil(row.sizes[i]));
                int cy = row.y;
                if (column) {
                    /* column items flow down the main axis (row.y is the
                     * cross start, i.e. the left edge); the width handed to
                     * the child is the cross size (inner_w), the height is
                     * the main-axis share */
                    const int item_y = row.y + static_cast<int>(pos);
                    cy = item_y;
                    layout(k, n->content.x, item_y, inner_w, sz, font_px,
                           &cy);
                    /* the resolved main size is authoritative: a flex:1 pane
                     * takes its share even when the content is shorter (or
                     * taller - it then overflows/scrolls like a browser) */
                    if (k->border.h != sz) {
                        /* nested flex: re-lay with the resolved height as an
                         * explicit height so inner flex:1 panes grow to it
                         * (auto height laid them out at inner_h 0). Uses the
                         * same item_y - cy was advanced by the first pass. */
                        std::string hs = get(k->style, "height");
                        char hbuf[32];
                        std::snprintf(hbuf, sizeof(hbuf), "%dpx", sz);
                        k->style["height"] = hbuf;
                        cy = item_y;
                        layout(k, n->content.x, item_y, inner_w, sz, font_px,
                               &cy);
                        if (hs.empty()) {
                            k->style.erase("height");
                        } else {
                            k->style["height"] = hs;
                        }
                    }
                    k->border.h = sz;
                    k->content.h = sz - k->padding[0] - k->padding[2] -
                                   k->border_w[0] - k->border_w[2];
                    pos = (k->border.y + k->border.h) - n->content.y +
                          between;
                    column_total = (k->border.y + k->border.h) - n->content.y;
                    if (reverse) { /* column-reverse: mirror vertically */
                        int dy = inner_h - (k->border.y - n->content.y) -
                                 k->border.h;
                        shift_subtree(k, 0, dy);
                        pos = (k->border.y + k->border.h) - n->content.y +
                              between;
                        column_total = (k->border.y + k->border.h) -
                                       n->content.y;
                    }
                } else {
                    /* a text-run item gets unlimited wrap width: the flex
                     * main size is its own content width, so passing that
                     * as the wrap width makes est_wrap_lines see a
                     * boundary case and split the run into multiple lines
                     * (a 2-char label became 3 lines tall -> the text
                     * painted below the checkbox). */
                    int lay_cw = k->is_text ? 0x7FFFFFFF : sz;
                    layout(k, n->content.x + static_cast<int>(pos), row.y,
                           lay_cw, inner_h, font_px, &cy);
                    k->border.w = sz;
                    k->content.w = sz - k->padding[1] - k->padding[3] -
                                   k->border_w[1] - k->border_w[3];
                    pos = (k->border.x + k->border.w) - n->content.x +
                          between;
                    if (reverse) { /* row-reverse: mirror horizontally */
                        /* the item was laid out left-to-right at offset
                         * off = (border.x - content.x); mirror it to
                         * offset (inner_w - off - w). shift = new - old =
                         * inner_w - 2*off - w. (The old formula dropped
                         * the second off, so every item mirrored onto the
                         * same right-edge position.) pos keeps the
                         * PRE-mirror advance so the next item lands to
                         * the left. */
                        int off = k->border.x - n->content.x;
                        int dx = inner_w - 2 * off - k->border.w;
                        shift_subtree(k, dx, 0);
                    }
                }
                /* track the row's real cross size (border + margins);
                 * for column the cross axis is horizontal (width) */
                int outer = (column ? k->border.w + k->margin[1] + k->margin[3]
                                    : k->border.h + k->margin[0] + k->margin[2]);
                if (outer > row.real_h) {
                    row.real_h = outer;
                }
            }
        }

        /* a single unwrapped row stretches to the container's definite
         * cross size: align-items:stretch then fills a flex:1 child whose
         * content is empty (an editor pane with only absolute children).
         * After the real heights are known, so the estimate cannot fight
         * the measured row height. */
        if (!column && rows.size() == 1 && inner_h > 0 &&
            rows[0].real_h < inner_h) {
            rows[0].real_h = inner_h;
        }

        /* cross-axis alignment: align-content (between rows) then
         * align-items/align-self (within a row) */
        int cross = column ? inner_w : inner_h;
        int content_cross = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            content_cross += rows[r].real_h;
        }
        content_cross += static_cast<int>(gap_cross) *
                         (rows.size() > 1 ? static_cast<int>(rows.size() - 1)
                                          : 0);
        int free_cross = cross - content_cross;
        if (free_cross < 0) {
            free_cross = 0;
        }
        float lead_c = 0, between_c = gap_cross;
        if (align_c == "center") {
            lead_c = free_cross / 2;
        } else if (align_c == "flex-end" || align_c == "end") {
            lead_c = static_cast<float>(free_cross);
        } else if (align_c == "space-between") {
            between_c = rows.size() > 1
                            ? static_cast<float>(free_cross) /
                                  (rows.size() - 1)
                            : gap_cross;
        } else if (align_c == "space-around") {
            lead_c = free_cross / (rows.size() * 2);
            between_c = free_cross / rows.size();
        } else if (align_c == "space-evenly") {
            lead_c = free_cross / (rows.size() + 1);
            between_c = free_cross / (rows.size() + 1);
        }
        /* shift each row into place (est vs real height drift + align-content) */
        {
            int acc = n->content.y + static_cast<int>(lead_c);
            bool first = true;
            for (size_t r = 0; r < rows.size(); ++r) {
                int target = acc;
                int drift = target - rows[r].y;
                if (!first && drift != 0) {
                    for (size_t i = 0; i < rows[r].items.size(); ++i) {
                        shift_subtree(rows[r].items[i], 0, drift);
                    }
                }
                first = false;
                acc += rows[r].real_h + static_cast<int>(between_c);
            }
            /* realigned total height */
            int bottom = acc - static_cast<int>(between_c);
            if (column) {
                /* column: the main axis is vertical, so the container height
                 * is the last item's bottom (tracked in the layout loop) */
                kid_cursor = column_total;
            } else {
                kid_cursor = bottom - n->content.y;
            }
        }

        /* within-row alignment: align-self wins over align-items */
        for (size_t r = 0; r < rows.size(); ++r) {
            FlexRow& row = rows[r];
            for (size_t i = 0; i < row.items.size(); ++i) {
                whaleui_layout_node_t* k = row.items[i];
                std::string as = get(k->style, "align-self");
                std::string a = as.empty() || as == "auto" ? align : as;
                int outer = (column ? k->border.w + k->margin[1] + k->margin[3]
                                    : k->border.h + k->margin[0] + k->margin[2]);
                int extra = row.real_h - outer;
                if (extra <= 0) {
                    continue;
                }
                if (a == "center") {
                    shift_subtree(k, column ? extra / 2 : 0,
                                  column ? 0 : extra / 2);
                } else if (a == "flex-end" || a == "end") {
                    shift_subtree(k, column ? extra : 0,
                                  column ? 0 : extra);
                } else if (!k->is_text &&
                           (a == "stretch" || a.empty())) {
                    /* stretch: grow the border box, content stays top-left.
                     * Re-lay with the resolved height so absolute children
                     * (inset:0 panes) fill the new size - the first pass
                     * laid them out at inner_h 0. */
                    if (column) {
                        k->border.w += extra;
                        k->content.w += extra;
                    } else if (extra > 0) {
                        std::string hs = get(k->style, "height");
                        int target_h = k->border.h + extra;
                        /* the temp height is the CONTENT height: a
                         * content-box element re-adds padding+border on
                         * layout, so setting the border height directly
                         * would stretch it past the row */
                        std::string bs = get(k->style, "box-sizing");
                        int content_h = target_h;
                        if (bs != "border-box") {
                            content_h = target_h - k->padding[0] -
                                        k->padding[2] - k->border_w[0] -
                                        k->border_w[2];
                        }
                        if (content_h < 0) {
                            content_h = 0;
                        }
                        char hbuf[32];
                        std::snprintf(hbuf, sizeof(hbuf), "%dpx", content_h);
                        k->style["height"] = hbuf;
                        int c1 = k->border.y - k->margin[0];
                        /* keep the laid-out width: cw must be the border
                         * width, not the content width, or a content-box
                         * item re-measures narrower (366 -> 330) and the
                         * flex share is lost */
                        layout(k, k->border.x - k->margin[1], c1,
                               k->border.w, content_h, font_px, &c1);
                        /* force the exact border height (int rounding) */
                        k->border.h = target_h;
                        k->content.h = target_h - k->padding[0] -
                                       k->padding[2] - k->border_w[0] -
                                       k->border_w[2];
                        if (hs.empty()) {
                            k->style.erase("height");
                        } else {
                            k->style["height"] = hs;
                        }
                    } else {
                        k->border.h += extra;
                        k->content.h += extra;
                    }
                }
            }
        }
    }

    /* --- CSS Grid layout ---
     * PureLayout algorithm port: tracks (px/%/fr/auto with repeat()),
     * explicit grid-column/grid-row placement (N, "N / M", "span N",
     * negative lines) with auto-placement filling the gaps, row tracks
     * and stretch/center alignment. Tables (display:table) run through
     * the same engine: tr rows become grid rows, td/th cells become grid
     * items placed in reading order (colspan -> a column span). */

    struct GridTrack
    {
        float len; /* px / fr number / percent number */
        int type;  /* 0=px 1=fr 2=auto 3=percent */
    };

    /* grid item placement: final grid lines (1-based, end exclusive) */
    struct GridCell
    {
        whaleui_layout_node_t* node;
        whaleui_layout_node_t* row_node; /* table: the tr (row background) */
        int cs, ce, rs, re;
    };

    /* parsed "grid-column"/"grid-row" value: start line (0 = auto,
     * negative = line counted from the end) + span (negative = the end is
     * a negative line, e.g. "1 / -1") */
    struct GridPlace
    {
        int start;
        int span;
    };

    static void parse_grid_place(const std::string& v, GridPlace& out)
    {
        out.start = 0;
        out.span = 1;
        if (v.empty() || v == "auto") {
            return;
        }
        size_t slash = v.find('/');
        std::string t1 = slash == std::string::npos ? v : v.substr(0, slash);
        std::string t2 = slash == std::string::npos
                             ? std::string()
                             : v.substr(slash + 1);
        auto trim = [](std::string s) {
            size_t b = s.find_first_not_of(" \t");
            size_t e = s.find_last_not_of(" \t");
            return b == std::string::npos ? std::string()
                                          : s.substr(b, e - b + 1);
        };
        t1 = trim(t1);
        t2 = trim(t2);
        if (t1.compare(0, 4, "span") == 0) {
            out.span = std::atoi(t1.c_str() + 4);
            if (out.span < 1) {
                out.span = 1;
            }
        } else if (!t1.empty() && t1 != "auto") {
            out.start = std::atoi(t1.c_str());
        }
        if (!t2.empty() && t2 != "auto") {
            if (t2.compare(0, 4, "span") == 0) {
                out.span = std::atoi(t2.c_str() + 4);
                if (out.span < 1) {
                    out.span = 1;
                }
            } else {
                int end = std::atoi(t2.c_str());
                if (out.start == 0) {
                    out.start = 1; /* "auto / M" anchors at line 1 */
                }
                if (end > out.start) {
                    out.span = end - out.start;
                } else if (end < 0) {
                    out.span = end; /* negative end line, resolved later */
                }
            }
        }
    }

    /* resolve a placement against a known line count into (start, end) */
    static void resolve_place(const GridPlace& p, int nlines, int& s, int& e)
    {
        s = p.start;
        if (s < 0) {
            s = nlines + 1 + s; /* -1 -> the last line */
        }
        if (s < 1) {
            s = 1;
        }
        if (p.span < 0) {
            e = nlines + 1 + p.span; /* -1 -> line nlines */
        } else {
            e = s + p.span;
        }
        if (e <= s) {
            e = s + 1;
        }
    }

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
            } else if (end != tok.c_str() && *end == '%') {
                out.push_back(GridTrack{num, 3});
            } else if (end != tok.c_str()) {
                out.push_back(GridTrack{len_px(tok, 0, 16), 0});
            } else {
                out.push_back(GridTrack{0, 2}); /* auto / unknown */
            }
        }
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

    /* table helpers: row-group (thead/tbody/tfoot) and row/cell tags */
    static bool is_row_group(whaleui_layout_node_t* k)
    {
        if (!k || !k->el) {
            return false;
        }
        size_t len = 0;
        const lxb_char_t* name = lxb_dom_element_local_name(k->el, &len);
        return (len == 5 && std::memcmp(name, "thead", 5) == 0) ||
               (len == 5 && std::memcmp(name, "tbody", 5) == 0) ||
               (len == 5 && std::memcmp(name, "tfoot", 5) == 0);
    }
    static bool is_table_row(whaleui_layout_node_t* k)
    {
        /* tr tag, or a div/span with display:table-row (the .divtable
         * pattern - table rows built out of divs) */
        return k && (k->tag_id == WUI_TAG_TR ||
                     display_kind(get(k->style, "display")) == 6);
    }
    static bool is_table_cell(whaleui_layout_node_t* k)
    {
        return k && (k->tag_id == WUI_TAG_TD || k->tag_id == WUI_TAG_TH ||
                     display_kind(get(k->style, "display")) == 7);
    }

    /* grid-column / grid-row shorthand, falling back to the start/end
     * longhands ("grid-column-start: 2; grid-column-end: 4") */
    GridPlace grid_place_of(const WhaleUIComputedStyle& s, bool column_axis)
    {
        GridPlace p;
        std::string v = column_axis ? get(s, "grid-column")
                                    : get(s, "grid-row");
        if (v.empty()) {
            std::string st = column_axis ? get(s, "grid-column-start")
                                         : get(s, "grid-row-start");
            std::string en = column_axis ? get(s, "grid-column-end")
                                         : get(s, "grid-row-end");
            v = st.empty() ? std::string() : st;
            if (!en.empty() && en != "auto") {
                v = v.empty() ? en : v + " / " + en;
            }
        }
        parse_grid_place(v, p);
        return p;
    }

    void layout_grid(whaleui_layout_node_t* n, int inner_w, int inner_h,
                     int font_px, int& kid_cursor)
    {
        float em = font_px > 0 ? static_cast<float>(font_px) : 16;
        bool is_table = display_kind(get(n->style, "display")) == 5;

        /* collect grid items (table: tr rows -> reading-order cells) */
        std::vector<GridCell> cells;
        if (is_table) {
            int r = 1;
            for (whaleui_layout_node_t* rc = n->first_child; rc;
                 rc = rc->next) {
                if (!rc->visible) {
                    continue;
                }
                std::vector<whaleui_layout_node_t*> row;
                if (is_table_row(rc)) {
                    row.push_back(rc);
                } else if (is_row_group(rc)) {
                    for (whaleui_layout_node_t* tc = rc->first_child; tc;
                         tc = tc->next) {
                        if (tc->visible && is_table_row(tc)) {
                            row.push_back(tc);
                        }
                    }
                }
                if (row.empty()) {
                    continue;
                }
                for (size_t ri = 0; ri < row.size(); ++ri) {
                    whaleui_layout_node_t* tr = row[ri];
                    int col = 1; /* each row restarts at column 1 */
                    for (whaleui_layout_node_t* cc = tr->first_child; cc;
                         cc = cc->next) {
                        if (!cc->visible || !is_table_cell(cc)) {
                            continue;
                        }
                        GridPlace p = grid_place_of(cc->style, true);
                        int span = p.span;
                        if (span < 0) {
                            span = 1; /* negative end resolved later */
                        }
                        int cs2 = p.start > 0 ? p.start : col;
                        if (p.start == 0) {
                            size_t alen = 0;
                            const lxb_char_t* csattr = lxb_dom_element_get_attribute(
                                cc->el, (const lxb_char_t*)"colspan", 7,
                                &alen);
                            if (csattr && alen > 0) {
                                int s2 = std::atoi(
                                    reinterpret_cast<const char*>(csattr));
                                if (s2 > 1) {
                                    span = s2;
                                }
                            }
                        }
                        GridCell cell;
                        cell.node = cc;
                        cell.row_node = tr;
                        cell.cs = cs2;
                        cell.ce = span;
                        cell.rs = r;
                        cell.re = 1;
                        cells.push_back(cell);
                        col = cs2 + (span > 0 ? span : 1);
                    }
                    ++r;
                }
            }
        } else {
            for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
                if (!c->visible) {
                    continue;
                }
                GridPlace pc = grid_place_of(c->style, true);
                GridPlace pr = grid_place_of(c->style, false);
                GridCell cell;
                cell.node = c;
                cell.row_node = nullptr;
                cell.cs = pc.start; /* 0 = auto placement */
                cell.ce = pc.span;  /* span; < 0 = negative end line */
                cell.rs = pr.start;
                cell.re = pr.span;
                cells.push_back(cell);
            }
        }
        if (cells.empty()) {
            return;
        }

        std::vector<GridTrack> ctracks;
        if (is_table) {
            /* table: one auto column per cell column */
            int ncol = 0;
            for (size_t i = 0; i < cells.size(); ++i) {
                if (cells[i].ce - 1 > ncol) {
                    ncol = cells[i].ce - 1;
                }
            }
            ctracks.assign(static_cast<size_t>(ncol > 0 ? ncol : 1),
                           GridTrack{0, 2});
        } else {
            parse_grid_tracks(get(n->style, "grid-template-columns"), ctracks);
        }
        std::vector<GridTrack> rtracks;
        parse_grid_tracks(get(n->style, "grid-template-rows"), rtracks);

        /* resolve explicit placements; count the grid columns */
        int ncols = static_cast<int>(ctracks.size());
        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i].cs > 0) {
                if (cells[i].ce > 0) {
                    int e = cells[i].cs + cells[i].ce;
                    if (e - 1 > ncols) {
                        ncols = e - 1;
                    }
                } else if (cells[i].cs > ncols) {
                    ncols = cells[i].cs;
                }
            }
        }
        if (ncols < 1) {
            ncols = 1;
        }
        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i].cs > 0) {
                int s = cells[i].cs;
                if (s < 0) {
                    s = ncols + 1 + s; /* -1 -> the last column */
                }
                if (s < 1) {
                    s = 1;
                }
                int e;
                if (cells[i].ce < 0) {
                    /* negative end line: there are ncols+1 lines, so
                     * line = (ncols+1) + 1 + e  (e = -1 -> the last) */
                    e = ncols + 2 + cells[i].ce;
                } else {
                    e = s + cells[i].ce; /* ce holds the span */
                }
                if (e <= s) {
                    e = s + 1;
                }
                cells[i].cs = s;
                cells[i].ce = e;
                if (e - 1 > ncols) {
                    ncols = e - 1;
                }
            }
        }
        /* resolve explicit rows (a negative row line is treated as auto -
         * ponytail: rare, needs the final row count) */
        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i].rs > 0) {
                int s = cells[i].rs;
                int e = s + (cells[i].re > 0 ? cells[i].re : 1);
                cells[i].rs = s;
                cells[i].re = e;
            }
        }

        /* auto placement: cursor scans rows for a free slot (sparse grid) */
        std::vector<std::vector<int> > taken; /* [row-1][col-1] occupied */
        auto taken_at = [&](int r, int c) -> int {
            if (r < 1 || c < 1 ||
                static_cast<size_t>(r) > taken.size() ||
                static_cast<size_t>(c) > taken[static_cast<size_t>(r - 1)].size()) {
                return 0;
            }
            return taken[static_cast<size_t>(r - 1)][static_cast<size_t>(c - 1)];
        };
        auto mark_taken = [&](int r, int c) {
            while (static_cast<int>(taken.size()) < r) {
                taken.push_back(std::vector<int>(static_cast<size_t>(ncols), 0));
            }
            while (static_cast<int>(taken[static_cast<size_t>(r - 1)].size()) < c) {
                taken[static_cast<size_t>(r - 1)].push_back(0);
            }
            taken[static_cast<size_t>(r - 1)][static_cast<size_t>(c - 1)] = 1;
        };
        int cursor_r = 1, cursor_c = 1;
        for (size_t i = 0; i < cells.size(); ++i) {
            int cs = cells[i].cs;
            int rs = cells[i].rs;
            int cspan = cs > 0 ? cells[i].ce - cs : cells[i].ce;
            int rspan = rs > 0 ? cells[i].re - rs
                               : (cells[i].re > 1 ? cells[i].re : 1);
            if (cspan < 1) {
                cspan = 1;
            }
            if (rspan < 1) {
                rspan = 1;
            }
            if (cs > 0 && rs > 0) {
                /* fully explicit: occupy its cells */
                for (int r = rs; r < rs + rspan; ++r) {
                    for (int c = cs; c < cs + cspan; ++c) {
                        mark_taken(r, c);
                    }
                }
                continue;
            }
            /* scan for a free slot */
            int r = rs > 0 ? rs : cursor_r;
            int c = cs > 0 ? cs : cursor_c;
            for (;;) {
                if (c + cspan - 1 > ncols) {
                    c = 1;
                    ++r;
                    continue;
                }
                bool free = true;
                for (int rr = r; rr < r + rspan; ++rr) {
                    for (int cc = c; cc < c + cspan; ++cc) {
                        if (taken_at(rr, cc)) {
                            free = false;
                            break;
                        }
                    }
                }
                if (free) {
                    break;
                }
                ++c;
            }
            cells[i].cs = c;
            cells[i].ce = c + cspan;
            cells[i].rs = r;
            cells[i].re = r + rspan;
            for (int rr = r; rr < r + rspan; ++rr) {
                for (int cc = c; cc < c + cspan; ++cc) {
                    mark_taken(rr, cc);
                }
            }
            cursor_r = r;
            cursor_c = c + cspan;
            if (c + cspan - 1 > ncols) {
                ncols = c + cspan - 1; /* grow the implicit grid */
            }
        }
        int nrows = 1;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i].re - 1 > nrows) {
                nrows = cells[i].re - 1;
            }
        }
        if (static_cast<int>(rtracks.size()) > nrows) {
            nrows = static_cast<int>(rtracks.size());
        }

        std::string gapv = get(n->style, "gap");
        if (gapv.empty()) {
            gapv = get(n->style, "column-gap");
        }
        float col_gap = len_px(gapv, static_cast<float>(inner_w), em);
        std::string rgv = get(n->style, "row-gap");
        float row_gap = rgv.empty() ? col_gap
                                    : len_px(rgv, static_cast<float>(inner_w), em);
        std::string align = get(n->style, "align-items");
        if (align.empty()) {
            /* place-items: <align> <justify> - first value is align */
            align = get(n->style, "place-items");
        }

        /* column widths: px/% fixed, auto = content, fr = free share */
        std::vector<int> col_w(static_cast<size_t>(ncols), 0);
        float fr_sum = 0;
        for (int i = 0; i < ncols; ++i) {
            int t = i < static_cast<int>(ctracks.size())
                        ? ctracks[static_cast<size_t>(i)].type
                        : 2;
            if (t == 0) {
                col_w[static_cast<size_t>(i)] =
                    static_cast<int>(ctracks[static_cast<size_t>(i)].len);
            } else if (t == 3) {
                col_w[static_cast<size_t>(i)] = static_cast<int>(
                    ctracks[static_cast<size_t>(i)].len *
                    static_cast<float>(inner_w) / 100.0f);
            } else if (t == 1) {
                fr_sum += ctracks[static_cast<size_t>(i)].len;
            }
        }
        for (int i = 0; i < ncols; ++i) {
            int t = i < static_cast<int>(ctracks.size())
                        ? ctracks[static_cast<size_t>(i)].type
                        : 2;
            if (t != 2) {
                continue;
            }
            int mx = 0;
            for (size_t j = 0; j < cells.size(); ++j) {
                if (cells[j].cs <= i + 1 && cells[j].ce > i + 1) {
                    int w = static_cast<int>(std::ceil(
                        estimate_content_width(cells[j].node, em, true)));
                    /* the auto track is the cell's margin box: content +
                     * padding (already in the estimate) + border + margin.
                     * Without the border/margin the laid-out content box
                     * comes out narrower than the text and wraps
                     * ("鍐呭鑷€傚簲" in a 2px-border 4px-margin cell). */
                    int b4[4], m4[4];
                    box_border_widths(cells[j].node->style, em, b4);
                    sides(get(cells[j].node->style, "margin"), m4, 0, em, 0);
                    w += b4[1] + b4[3] + m4[1] + m4[3];
                    /* an explicit width on a cell caps the auto track
                     * (a <th style="width:80px"> column is 80px wide) */
                    std::string wv = get(cells[j].node->style, "width");
                    if (!wv.empty() && wv != "auto") {
                        float wpx = len_px(wv, static_cast<float>(inner_w), em);
                        if (wpx > w) {
                            w = static_cast<int>(wpx);
                        }
                    }
                    if (w > mx) {
                        mx = w;
                    }
                }
            }
            col_w[static_cast<size_t>(i)] = mx;
        }
        float gap_sum = col_gap * (ncols > 1 ? static_cast<float>(ncols - 1) : 0);
        float fixed_sum = 0;
        for (int i = 0; i < ncols; ++i) {
            fixed_sum += static_cast<float>(col_w[static_cast<size_t>(i)]);
        }
        float free = static_cast<float>(inner_w) - fixed_sum - gap_sum;
        if (free < 0) {
            free = 0;
        }
        float fr_unit = fr_sum > 0 ? free / fr_sum : 0;
        for (int i = 0; i < ncols; ++i) {
            int t = i < static_cast<int>(ctracks.size())
                        ? ctracks[static_cast<size_t>(i)].type
                        : 2;
            if (t == 1) {
                col_w[static_cast<size_t>(i)] = static_cast<int>(
                    ctracks[static_cast<size_t>(i)].len * fr_unit);
            }
        }
        /* a <table> with a definite width and no fr columns: the browser
         * stretches the auto columns to fill the table (a 3-col name
         * table would otherwise keep its content-width cells and leave
         * the rest of the 100% width empty - "鏍煎瓙鏈夌偣灏?). Distribute
         * the free space over the AUTO tracks (those without an explicit
         * cell width - a th width:80px column must stay 80px) by their
         * content share. */
        if (is_table && fr_sum <= 0 && free > 0) {
            std::vector<bool> fixed_col(static_cast<size_t>(ncols), false);
            for (size_t j = 0; j < cells.size(); ++j) {
                std::string wv = get(cells[j].node->style, "width");
                if (!wv.empty() && wv != "auto" &&
                    cells[j].cs >= 1 && cells[j].cs <= ncols) {
                    fixed_col[static_cast<size_t>(cells[j].cs - 1)] = true;
                }
            }
            float auto_sum = 0;
            int n_auto = 0;
            for (int i = 0; i < ncols; ++i) {
                if (!fixed_col[static_cast<size_t>(i)]) {
                    auto_sum += static_cast<float>(col_w[static_cast<size_t>(i)]);
                    ++n_auto;
                }
            }
            if (auto_sum > 0) {
                for (int i = 0; i < ncols; ++i) {
                    if (!fixed_col[static_cast<size_t>(i)] &&
                        col_w[static_cast<size_t>(i)] > 0) {
                        col_w[static_cast<size_t>(i)] += static_cast<int>(
                            free * static_cast<float>(col_w[static_cast<size_t>(i)]) /
                            auto_sum);
                    }
                }
            } else if (n_auto > 0) {
                int share = static_cast<int>(free / n_auto);
                for (int i = 0; i < ncols; ++i) {
                    if (!fixed_col[static_cast<size_t>(i)]) {
                        col_w[static_cast<size_t>(i)] += share;
                    }
                }
            }
        }

        /* row heights: explicit tracks, auto rows = tallest cell */
        std::vector<int> row_h(static_cast<size_t>(nrows), 0);
        float fr_row_sum = 0;
        for (int i = 0; i < nrows; ++i) {
            int t = i < static_cast<int>(rtracks.size())
                        ? rtracks[static_cast<size_t>(i)].type
                        : 2;
            if (t == 0) {
                row_h[static_cast<size_t>(i)] =
                    static_cast<int>(rtracks[static_cast<size_t>(i)].len);
            } else if (t == 3) {
                row_h[static_cast<size_t>(i)] = static_cast<int>(
                    rtracks[static_cast<size_t>(i)].len *
                    static_cast<float>(inner_h) / 100.0f);
            } else if (t == 1) {
                fr_row_sum += rtracks[static_cast<size_t>(i)].len;
            }
        }
        for (int i = 0; i < nrows; ++i) {
            int t = i < static_cast<int>(rtracks.size())
                        ? rtracks[static_cast<size_t>(i)].type
                        : 2;
            if (t != 2) {
                continue;
            }
            int mx = 1;
            for (size_t j = 0; j < cells.size(); ++j) {
                if (cells[j].rs <= i + 1 && cells[j].re > i + 1) {
                    int h = est_node_height(cells[j].node, inner_w, em);
                    /* auto rows cover the cell's FULL border box: the
                     * estimate is content-only, and a padded/bordered cell
                     * (td padding 4px + 1px border -> 10px taller than its
                     * text) laid out at the content height overflowed the
                     * row - the next row started under the content and the
                     * cells overlapped their border line ("文字在线上") */
                    BoxMetrics cbm = box_metrics(cells[j].node->style,
                                                 static_cast<float>(inner_w),
                                                 em);
                    h += cbm.p[0] + cbm.p[2] + cbm.b[0] + cbm.b[2];
                    if (h > mx) {
                        mx = h;
                    }
                }
            }
            row_h[static_cast<size_t>(i)] = mx;
        }
        if (fr_row_sum > 0 && inner_h > 0) {
            float hgap = row_gap * (nrows > 1 ? static_cast<float>(nrows - 1) : 0);
            float hfixed = 0;
            for (int i = 0; i < nrows; ++i) {
                int t = i < static_cast<int>(rtracks.size())
                            ? rtracks[static_cast<size_t>(i)].type
                            : 2;
                if (t != 1) {
                    hfixed += static_cast<float>(row_h[static_cast<size_t>(i)]);
                }
            }
            float hfree = static_cast<float>(inner_h) - hfixed - hgap;
            if (hfree < 0) {
                hfree = 0;
            }
            float hunit = hfree / fr_row_sum;
            for (int i = 0; i < nrows; ++i) {
                int t = i < static_cast<int>(rtracks.size())
                            ? rtracks[static_cast<size_t>(i)].type
                            : 2;
                if (t == 1) {
                    row_h[static_cast<size_t>(i)] =
                        static_cast<int>(rtracks[static_cast<size_t>(i)].len *
                                         hunit);
                }
            }
        }

        /* place cells; a row box (table) spans the full grid width */
        int y = n->content.y;
        int max_cell_bottom = 0; /* real laid-out bottom, not the est rows */
        for (int r = 0; r < nrows; ++r) {
            for (size_t i = 0; i < cells.size(); ++i) {
                if (cells[i].rs - 1 != r) {
                    continue;
                }
                /* offsets from the grid origin */
                int ox = 0;
                for (int c = 1; c < cells[i].cs; ++c) {
                    ox += col_w[static_cast<size_t>(c - 1)] +
                          static_cast<int>(col_gap);
                }
                int oy = 0;
                for (int rr = 1; rr < cells[i].rs; ++rr) {
                    oy += row_h[static_cast<size_t>(rr - 1)] +
                          static_cast<int>(row_gap);
                }
                int w = 0;
                for (int c = cells[i].cs; c < cells[i].ce; ++c) {
                    w += col_w[static_cast<size_t>(c - 1)];
                    if (c > cells[i].cs) {
                        w += static_cast<int>(col_gap);
                    }
                }
                int h = 0;
                for (int rr = cells[i].rs; rr < cells[i].re; ++rr) {
                    h += row_h[static_cast<size_t>(rr - 1)];
                    if (rr > cells[i].rs) {
                        h += static_cast<int>(row_gap);
                    }
                }
                int cy = n->content.y + oy;
                layout(cells[i].node, n->content.x + ox, cy, w, h, font_px,
                       &cy);
                /* stretch: auto-height cells fill the row (default).
                 * Cells with an explicit align-self (center/start/end)
                 * keep their content height - the align block below
                 * positions them (previously the stretch ran first and
                 * "align-self: center" cells filled the whole row). */
                std::string aself =
                    get(cells[i].node->style, "align-self");
                bool no_stretch = aself == "center" || aself == "start" ||
                                  aself == "end" || aself == "flex-start" ||
                                  aself == "flex-end";
                bool h_auto = get(cells[i].node->style, "height").empty() ||
                              get(cells[i].node->style, "height") == "auto";
                if (h_auto && !no_stretch && h > cells[i].node->border.h) {
                    cells[i].node->border.h = h;
                    cells[i].node->content.h =
                        h - cells[i].node->padding[0] -
                        cells[i].node->padding[2] -
                        cells[i].node->border_w[0] - cells[i].node->border_w[2];
                }
                /* align-items: center / flex-end vertically center shorter
                 * cells (stretch already filled the row). align-self on
                 * the cell wins over the container's align-items. */
                std::string ca = (aself.empty() || aself == "auto")
                                     ? align
                                     : aself;
                if (ca == "center" || ca == "flex-end" || ca == "end") {
                    int extra = h - cells[i].node->border.h;
                    if (extra > 0) {
                        int dy = ca == "center" ? extra / 2 : extra;
                        shift_subtree(cells[i].node, 0, dy);
                    }
                }
                if (cells[i].row_node) {
                    cells[i].row_node->border.x = n->content.x;
                    cells[i].row_node->border.y = n->content.y + oy;
                    cells[i].row_node->border.w = inner_w;
                    cells[i].row_node->border.h = h;
                    cells[i].row_node->content = cells[i].row_node->border;
                }
                /* track the REAL bottom: an auto row estimated shorter
                 * than the laid-out cell (a 2-line cell whose est height
                 * came out 3px short) would otherwise size the container
                 * smaller than its content - the "grid content runs past
                 * the bottom" reports. */
                int cb = cells[i].node->border.y + cells[i].node->border.h;
                if (cb > max_cell_bottom) {
                    max_cell_bottom = cb;
                }
            }
            y += row_h[static_cast<size_t>(r)] + static_cast<int>(row_gap);
        }
        kid_cursor = max_cell_bottom > 0
                         ? max_cell_bottom - n->content.y
                         : y - n->content.y - static_cast<int>(row_gap);
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
    /* CSS vars collected once per tree; relayout passes reuse them */
    if (!tree->vars_collected && root) {
        whaleui_style_collect_vars_full(root, rules, count, tree->vars);
        tree->vars_collected = true;
    }
    for (auto& kv : tree->vars) {
        b.vars[kv.first] = kv.second;
    }

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

static int relayout_impl(
    whaleui_layout_tree_t* tree,
    lxb_dom_element* el,
    const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>* theme_vars,
    const whaleui_style_state* st,
    const std::map<lxb_dom_element*, int>* scrolls,
    struct whaleui_anim* anim, float text_scale, int skip_box)
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
    if (!tree->vars_collected && tree->root && tree->root->el) {
        whaleui_style_collect_vars_full(tree->root->el, rules, count,
                                        tree->vars);
        tree->vars_collected = true;
    }
    for (auto& kv : tree->vars) {
        b.vars[kv.first] = kv.second;
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
     * and are only re-positioned. Same tail as whaleui_layout_compute.
     * skip_box (style-only relayout): geometry is unchanged, copy it from
     * the old subtree instead of re-boxing the whole tree - the dominant
     * relayout cost on a 34k-node page (~1.5s) for a paint-only hover. */
    bool boxed = !skip_box;
    if (skip_box) {
        /* old nodes stay alive in the arena (unmap only dropped the by_el
         * entries), so walk both trees in the same DOM order and copy */
        std::function<bool(whaleui_layout_node_t*, whaleui_layout_node_t*)>
            copy_geo = [&](whaleui_layout_node_t* a,
                           whaleui_layout_node_t* b) -> bool {
                b->border = a->border;
                b->content = a->content;
                for (int i = 0; i < 4; ++i) {
                    b->margin[i] = a->margin[i];
                    b->padding[i] = a->padding[i];
                    b->border_w[i] = a->border_w[i];
                }
                b->scroll_y = a->scroll_y;
                b->scroll_max = a->scroll_max;
                b->z = a->z;
                whaleui_layout_node_t* ca = a->first_child;
                whaleui_layout_node_t* cb = b->first_child;
                while (ca || cb) {
                    if (!ca || !cb) {
                        return false; /* structure mismatch: full box pass */
                    }
                    if (!copy_geo(ca, cb)) {
                        return false;
                    }
                    ca = ca->next;
                    cb = cb->next;
                }
                return true;
            };
        boxed = !copy_geo(old, fresh);
    }
    if (boxed) {
        int cursor = 0;
        b.layout(tree->root, 0, 0, tree->viewport_w, tree->viewport_h, 16,
                 &cursor);
        tree->root->border.h = tree->viewport_h;
        tree->root->content.h = tree->viewport_h;
        {
            int cmax = cursor - tree->viewport_h;
            tree->root->scroll_max = cmax > 0 ? cmax : 0;
        }
    }
    return 0;
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
    return relayout_impl(tree, el, rules, count, theme_vars, st, scrolls,
                         anim, text_scale, 0);
}

extern "C" int whaleui_layout_relayout_style(
    whaleui_layout_tree_t* tree,
    lxb_dom_element* el,
    const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>* theme_vars,
    const whaleui_style_state* st,
    const std::map<lxb_dom_element*, int>* scrolls,
    struct whaleui_anim* anim, float text_scale)
{
    return relayout_impl(tree, el, rules, count, theme_vars, st, scrolls,
                         anim, text_scale, 1);
}

/* batch relayout for a layout-affecting animation: rebuild each animated
 * element's subtree with the tick's styles, but run the box pass ONCE for
 * all of them. Per-element relayout ran the whole-tree box pass per element
 * (the demo's bar anim has 6 animated elements -> 6 full box passes a
 * frame); one pass keeps an animation frame proportional to a single
 * box pass instead of N. */
int whaleui_layout_relayout_multi(
    whaleui_layout_tree_t* tree, lxb_dom_element* const* els, size_t nel,
    const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>* theme_vars,
    const whaleui_style_state* st,
    const std::map<lxb_dom_element*, int>* scrolls,
    struct whaleui_anim* anim, float text_scale)
{
    if (!tree || !els || nel == 0) {
        return 1;
    }
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
    /* CSS vars collected once per tree (relayout reuses them - the page's
     * vars don't change between DOM mutations; render.cpp clears tree->vars
     * on DOM changes to force a re-collect) */
    if (!tree->vars_collected && tree->root && tree->root->el) {
        whaleui_style_collect_vars_full(tree->root->el, rules, count,
                                        tree->vars);
        tree->vars_collected = true;
    }
    for (auto& kv : tree->vars) {
        b.vars[kv.first] = kv.second;
    }
    std::vector<whaleui_layout_node_t*> freshNodes;
    for (size_t e = 0; e < nel; ++e) {
        lxb_dom_element* el = els[e];
        auto found = tree->by_el.find(el);
        if (found == tree->by_el.end()) {
            return 1;
        }
        whaleui_layout_node_t* old = found->second;
        whaleui_layout_node_t* parent = old->parent;
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
        whaleui_layout_node_t* fresh = b.build(el, parent);
        if (!fresh) {
            return -1;
        }
        freshNodes.push_back(fresh);
        fresh->next = old->next;
        if (parent) {
            whaleui_layout_node_t** link = &parent->first_child;
            while (*link && *link != old) {
                link = &(*link)->next;
            }
            if (*link != old) {
                return -1;
            }
            *link = fresh;
        } else {
            tree->root = fresh;
        }
    }
    /* localize the box pass: a layout animation (bar width) reflows the
     * animated subtree but the sibling flex rows do not move, so re-running
     * the whole-tree box pass every frame is the cost. Re-layout only the
     * topmost flex/grid ancestor (deduped) of each animated element,
     * positioned inside its parent's content box (NOT its own border - the
     * ancestor chain sets that, and using the element's own border double-
     * applies margins and collapses it to the top). */
    std::vector<whaleui_layout_node_t*> rroots;
    for (whaleui_layout_node_t* fn : freshNodes) {
        whaleui_layout_node_t* a = fn->parent;
        while (a && a != tree->root) {
            int adk = display_kind(get(a->style, "display"));
            if (adk == 1 || adk == 4) {
                break;
            }
            a = a->parent;
        }
        if (!a) {
            a = tree->root;
        }
        if (std::find(rroots.begin(), rroots.end(), a) == rroots.end()) {
            rroots.push_back(a);
        }
    }
    for (whaleui_layout_node_t* rr : rroots) {
        int cx = 0, cy = 0, cw = tree->viewport_w, ch = tree->viewport_h;
        int cursor = 0;
        if (rr->parent) {
            cx = rr->parent->content.x;
            cy = rr->parent->content.y;
            cw = rr->parent->content.w;
            ch = rr->parent->content.h;
            /* static (in-flow) position: b.layout advances cursor_y to place
             * children, so seed it with the element's existing flow offset
             * (border.y minus its own margin-top) instead of 0 - 0 would
             * rebuild the subtree at the page top instead of in flow after
             * the header. */
            cursor = rr->border.y - rr->margin[0];
        }
        b.layout(rr, cx, cy, cw, ch, 16, &cursor);
    }
    tree->root->border.h = tree->viewport_h;
    tree->root->content.h = tree->viewport_h;
    {
        int cmax = 0;
        std::function<void(whaleui_layout_node_t*)> deep =
            [&](whaleui_layout_node_t* nd) {
                int bot = nd->border.y + nd->border.h;
                if (bot > cmax) {
                    cmax = bot;
                }
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    deep(c);
                }
            };
        deep(tree->root);
        tree->root->scroll_max =
            cmax > tree->viewport_h ? cmax - tree->viewport_h : 0;
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

extern "C" void whaleui_layout_set_wrap_lines_metric(whaleui_wrap_lines_fn fn)
{
    g_wrap_lines = fn;
}






