/* Renderer core: per-window render context, frame loop and interaction
 * (hit-test / click / wheel / editing / FSR decision).
 *
 * Paint primitives, colors, text and controls live in the split sources
 * (render_color.cpp / render_paint.cpp / render_text.cpp /
 * render_control.cpp / render_fsr.cpp); shared helpers are declared in
 * render_internal.h. */

#include "render/render.h"
#include "render/render_internal.h"
#include "render/gpu.h"
#include "animate/animate.h"
#include "dom/dom.h"
#include "font/font.h"
#include "style/style.h"

#ifdef WHALEUI_BUILD_FULL
/* Unicode categories for accurate word/separator classification in every
 * script (full build only; lite/minimal keep the dependency-free table) */
#include <utf8proc.h>
#endif
/* runtime CJK dictionary (res/whaleui_dict.bin): full + lite builds use it
 * for word segmentation; minimal builds skip segmentation entirely */
#if defined(WHALEUI_BUILD_FULL) || defined(WHALEUI_BUILD_LITE)
#include "render/cjk_dict.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#ifdef WHALEUI_BUILD_FULL
#include <SDL3_ttf/SDL_ttf.h>
#endif
#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

/* paint-pass globals shared with the split render sources */
whaleui_gpu_t* g_gpu = nullptr;
int g_backdrop_active = 0;

/* last layout tree rendered by any window (DOM geometry queries);
 * NULL until the first frame. The tree itself is owned by the render
 * context that produced it. */
whaleui_layout_tree_t* g_last_tree = nullptr;

/* the render context currently laying out (real text metric hook) */
whaleui_render_t* g_metric_render = nullptr;


#ifdef WHALEUI_BUILD_FULL
/* real text width for the layout pass: measure with the actual TTF font
 * (fallback chain included) so inline-line x positions and wrap points
 * match the painted glyphs. Returns 0 to fall back to the estimate. */
/* 64-bit FNV-1a key for the whole-string width cache (text + size +
 * family + weight + letter-spacing). Hashed so the lookup never copies
 * the UTF-8 string, which is O(len) per call on a text-heavy page. */
static uint64_t textw_key(const char* utf8, size_t len, int fs,
                          const char* family, bool bold, float lsp)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(utf8[i]);
        h *= 1099511628211ULL;
    }
    if (family) {
        for (const char* s = family; *s; ++s) {
            h ^= static_cast<unsigned char>(*s);
            h *= 1099511628211ULL;
        }
    }
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(fs));
    h *= 1099511628211ULL;
    h ^= bold ? 1ULL : 0ULL;
    h *= 1099511628211ULL;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(lsp * 16.0f));
    h *= 1099511628211ULL;
    return h;
}

float render_text_metric(const char* utf8, size_t len, float font_px,
                         bool bold, const char* family, float lsp_px)
{
    whaleui_render_t* r = g_metric_render;
    if (!r || !utf8 || len == 0) {
        return 0;
    }
    int fs = static_cast<int>(font_px);
    if (fs <= 0) {
        fs = 16;
    }
    TTF_Font* font = render_get_font(r, family ? family : "", fs,
                                     bold ? kFontBold : 0);
    if (!font) {
        return 0;
    }
    /* whole-string cache: the box pass and fix_run_heights re-measure the
     * SAME stable text every frame (an animation only repositions), so the
     * per-char glyph loop below is the hot cost. A hit returns in O(1). */
    uint64_t tkey = textw_key(utf8, len, fs, family, bold, lsp_px);
    {
        auto it = r->text_w_cache.find(tkey);
        if (it != r->text_w_cache.end()) {
            return it->second;
        }
    }
    /* measure per glyph (advance) instead of TTF_MeasureString: the layout
     * pass calls this for EVERY run EVERY frame (a layout animation), and
     * TTF_MeasureString re-measures the whole string with no cache - a big
     * chunk of the demo's per-frame cost. Per-glyph advances hit the
     * size-keyed glyph_w_cache / ascii_w, matching the paint path. */
    float total = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        size_t clen = 1;
        unsigned int cp = c;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0) {
                clen = 2;
                cp = c & 0x1F;
            } else if ((c & 0xF0) == 0xE0) {
                clen = 3;
                cp = c & 0x0F;
            } else {
                clen = 4;
                cp = c & 0x07;
            }
            for (size_t k = 1; k < clen && i + k < len; ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
            }
        }
        int adv = 0;
        if (cp < 0x80) {
            if (r->ascii_font != font || r->ascii_fs != fs) {
                r->ascii_font = font;
                r->ascii_fs = fs;
                TTF_SetFontSize(font, static_cast<float>(fs));
                for (int a = 0; a < 128; ++a) {
                    int m0 = 0, m1 = 0, m2 = 0, m3 = 0, aw = 0;
                    TTF_GetGlyphMetrics(font, static_cast<Uint32>(a),
                                        &m0, &m1, &m2, &m3, &aw);
                    r->ascii_w[a] = aw;
                }
            }
            adv = r->ascii_w[cp];
        } else {
            std::tuple<TTF_Font*, int, unsigned int> key(font, fs, cp);
            auto it = r->glyph_w_cache.find(key);
            if (it != r->glyph_w_cache.end()) {
                adv = it->second;
            } else {
                TTF_SetFontSize(font, static_cast<float>(fs));
                int m0 = 0, m1 = 0, m2 = 0, m3 = 0;
                if (TTF_GetGlyphMetrics(font, cp, &m0, &m1, &m2, &m3, &adv)) {
                    r->glyph_w_cache[key] = adv;
                } else {
                    adv = 0;
                }
            }
        }
        total += static_cast<float>(adv);
        i += clen;
    }
    if (lsp_px > 0) {
        size_t ch = 0;
        for (size_t k = 0; k < len; ++k) {
            if ((static_cast<unsigned char>(utf8[k]) & 0xC0) != 0x80) {
                ++ch;
            }
        }
        if (ch > 1) {
            total += lsp_px * static_cast<float>(ch - 1);
        }
    }
    r->text_w_cache[tkey] = total;
    return total;
}

/* real line height for the layout pass (TTF font metrics), so textarea
 * heights / scroll_max match the painted glyph line boxes */
float render_line_height(float font_px, bool bold, const char* family)
{
    whaleui_render_t* r = g_metric_render;
    if (!r) {
        return 0;
    }
    int fs = static_cast<int>(font_px);
    if (fs <= 0) {
        fs = 16;
    }
    TTF_Font* font = render_get_font(r, family ? family : "", fs,
                                     bold ? kFontBold : 0);
    if (!font) {
        return 0;
    }
    int h = TTF_GetFontHeight(font);
    return h > 0 ? static_cast<float>(h) : 0;
}

/* exact wrapped line count for the layout pass: the same per-glyph wrap
 * the paint pass uses (via text_size), so run heights/positions and the
 * page bottom agree to the pixel instead of drifting by an estimated
 * line. Runs only while g_metric_render is set (render layout). */
static size_t render_wrap_lines(const char* utf8, size_t len, int avail,
                                int font_px, bool bold, const char* family,
                                float letter_spacing_px)
{
    whaleui_render_t* r = g_metric_render;
    (void)letter_spacing_px;
    if (!r || !utf8 || len == 0) {
        return 0;
    }
    std::string text(utf8, len);
    int fs = font_px > 0 ? font_px : 16;
    std::string fam = family ? family : "";
    int lh = text_line_h(r, fs, fam, bold);
    if (lh <= 0) {
        return 0;
    }
    int tw = 0, th = 0;
    text_size(r, text, fs, fam, bold, &tw, &th, avail);
    if (th <= 0) {
        return 0;
    }
    /* L.th accumulates lh per line (including a trailing empty line) */
    return static_cast<size_t>((th + lh - 1) / lh);
}
#endif

/* --- color --- */




/* --- framebuffer helpers (RGBA8, 0xAARRGGBB) --- */


/* intersect [x0,x1)x[y0,y1) with clip (NULL = no clip) */
/* grow a clip rect to also contain (x0,y0)-(x1,y1); when out is empty it
 * takes the rect */



/* --- CSS gradients (linear/radial) --- */

/* split on top-level commas (function calls keep their own) */

/* parse one gradient from a background value; returns 1 when found */


/* paint a gradient over (x,y,w,h); alpha-blends over existing pixels */

/* is (px,py) inside a rounded rect (x,y,w,h,radius)? */

/* rounded rect: radius clipped to half the smaller side */

/* rounded border ring: area between the outer rounded rect and the same rect
 * inset by `bw`, so borders follow the corner arcs instead of going square */


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

/* is this element a text-editing target? text-ish input/textarea, or any
 * element with contenteditable != "false". text-ish = any input type that
 * holds text: text, password, number, email, search, tel, url, ... (not
 * button/checkbox/radio/file/color/submit/reset/hidden/image/range) */
bool is_non_text_input(const lxb_char_t* t, size_t alen)
{
    static const char* kSpecial[] = {
        "button", "checkbox", "radio", "file", "color", "submit",
        "reset", "hidden", "image", "range", "select",
    };
    for (size_t i = 0; i < sizeof(kSpecial) / sizeof(kSpecial[0]); ++i) {
        size_t n = std::strlen(kSpecial[i]);
        if (alen == n && std::memcmp(t, kSpecial[i], n) == 0) {
            return true;
        }
    }
    return false;
}

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
            /* button/checkbox/radio/... are not text-editable */
            if (t && alen > 0 && is_non_text_input(t, alen)) {
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
            if (t && alen > 0 && is_non_text_input(t, alen)) {
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


/* window -> framebuffer coordinate scale (FSR); defined in the C API area */
static void fb_coords(whaleui_render_t* r, int& x, int& y);



/* --- fonts --- */

/* split a font-family value ("Segoe UI, \"MS YaHei\", sans-serif") into a
 * list of family names (quotes and whitespace stripped) */


/* --- painting --- */


/* border shorthand "1px solid #rrggbb" / "2px dashed red": scan tokens for a
 * parseable color instead of requiring a plain color value */

/* --- text measuring / hit-testing ---
 * Shared by selection and caret placement. Must match what draw_text_at
 * paints (same font, same wrapping). One program-side layout (UTF-8
 * decode + soft wrap) drives both backends; glyph advance comes from
 * SDL3_ttf (full) or stb_truetype (lite/minimal) through a shared
 * per-glyph callback, so wrapping/caret/selection agree everywhere. */



/* total text size in px (0 when empty) */

/* byte offset of the text under (px,py): defined in render_text.cpp */
size_t byte_at_text(whaleui_render_t* r, const std::string& text, int fs,
                    const std::string& family, bool bold, int px, int py);

/* per-line height in px for the caret/highlight rectangles */

/* highlight rectangles for the byte range [a,b), relative to text top-left */

/* caret (cursor) position for byte offset `off`, relative to text top-left */


/* byte length of the UTF-8 sequence starting with c (ASCII = 1) */

/* ASCII-only text-transform; multi-byte UTF-8 passes through untouched and
 * never changes the byte length, so selection/caret offsets stay valid */

/* render one text string inside the box (bx,by,bw,bh). align: 0=left,
 * 1=center, 2=right. Shared by text runs and <select> controls.
 * style: TTF_STYLE_* bits (bold/italic).
 * ckey: element to cache the rasterized buffer against (NULL = no caching).
 * lsp: letter-spacing in px (added to every glyph advance).
 * wrap: wrap long text to bw (multi-line; height grows with line count). */

/* --- text selection + editing overlay --- */

/* selection byte range [a,b) for el's text; false when el is outside the
 * selection (or the range is empty). Cross-element selections highlight the
 * endpoint elements partially and everything in between fully.
 * seq/sel_lo/sel_hi are pre-order layout-tree sequence numbers computed once
 * per frame (O(1) membership test instead of a per-run document walk). */

/* paint-time scroll offset of a scrollable box (defined below) */
int scroll_delta(whaleui_render_t* r, whaleui_layout_node_t* n);

/* partial-repaint subtree culler (defined with paint_node below) */
bool paint_cull(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
                int off_y, const Clip* clip);

/* pre-order sequence numbers of the anchor/focus layout nodes, computed by
 * walking the tree EXACTLY like paint_node does (same visibility skip, same
 * clipped-subtree skip, same scroll offsets), so the sequence assigned
 * during painting matches. -1 when same-element or collapsed. */


/* paint-time scroll offset of a scrollable box: children keep DOCUMENT
 * coordinates (the layout no longer bakes scroll_y), so painting shifts
 * the content up by the CURRENT scroll (bigger scroll moves it up, hence
 * negative). */
int scroll_delta(whaleui_render_t* r, whaleui_layout_node_t* n)
{
    if (n->scroll_max > 0 && n->el) {
        auto it = r->scrolls.find(n->el);
        if (it != r->scrolls.end()) {
            return -it->second;
        }
    }
    return 0;
}

/* paint-time vertical offset applied to a node: the sum of every ancestor's
 * scroll delta. Hit-testing and painting add this to laid-out (scrolled)
 * coordinates to reach window space; caret/selection math must do the same
 * or clicks land at the wrong character after a scroll. */
int node_scroll_off(whaleui_render_t* r, whaleui_layout_node_t* n)
{
    int off = 0;
    for (whaleui_layout_node_t* p = n->parent; p; p = p->parent) {
        off += scroll_delta(r, p);
    }
    return off;
}

/* the layout estimated wrapped lines; the per-glyph render layout can
 * differ. Correct every text run's height to text_size so the caret,
 * selection, scrolling and the painted glyphs all agree. */
void fix_run_heights(whaleui_render_t* r)
{
    std::function<void(whaleui_layout_node_t*)> fix_run_h =
        [&](whaleui_layout_node_t* nd) {
            if (nd->is_text && !nd->text.empty()) {
                int fs;
                std::string fam;
                bool bold;
                node_font(r, nd, &fs, &fam, &bold);
                int tw = 0, th = 0;
                text_size(r, nd->text, fs, fam, bold, &tw, &th,
                          run_wrap_w(nd));
                if (th > 0) {
                    nd->border.h = th;
                }
            }
            for (whaleui_layout_node_t* c = nd->first_child; c;
                 c = c->next) {
                fix_run_h(c);
            }
        };
    fix_run_h(r->tree->root);
}

/* single post-order pass: compute each subtree's REAL bottom (the run
 * heights were just corrected above, but the ancestor boxes keep their
 * layout-estimated heights). The recursion carries `scomp` = the
 * accumulated scroll_y of every scrollable ancestor: descendants are laid
 * out against baked content.y (shifted up by -scroll_y), so adding the
 * compensation back yields UNBAKED coordinates. Every result - scroll_max,
 * grown auto-height boxes, the returned bottom - must be independent of
 * the live scroll, otherwise a mid-scroll relayout (hover) shrinks the
 * range, the thumb grows and the wheel stops early. */
void fix_scroll_max(whaleui_render_t* r)
{
    std::function<int(whaleui_layout_node_t*, int)> fix_sm =
        [&](whaleui_layout_node_t* nd, int scomp) -> int {
        bool scrollable = false;
        if (nd->el && !nd->is_text && !nd->pseudo) {
            std::string ov = sget(nd->style, "overflow");
            scrollable = ov == "auto" || ov == "scroll" ||
                         nd == r->tree->root;
        }
        int child_comp = scomp;
        /* a scrollable box's own border box is the VIEWPORT, not content:
         * its range is child-bottom minus content.h. Starting from the
         * border box added (border.h - content.h) of phantom scroll
         * (2 lines that fit reported a 6px range). */
        int inner_bottom = scrollable
                               ? 0
                               : nd->border.y + nd->border.h + scomp;
        for (whaleui_layout_node_t* c = nd->first_child; c;
             c = c->next) {
            int cb = fix_sm(c, child_comp);
            if (cb > inner_bottom) {
                inner_bottom = cb;
            }
        }
        if (scrollable) {
            /* inner_bottom is unbaked (children got child_comp): the range
             * is the true content height, independent of the live scroll.
             * The earlier max-with-cmax2 double-counted scroll_y when the
             * box had scroll_y > 0 (or a scrolled ancestor), inflating the
             * range (a 2-line textarea that reported ~70 lines after a
             * scroll/FSR relayout). The reference point must be unbaked
             * TOO: content.y carries the ancestor scrolls (a page scrolled
             * 200px under a textarea baked its y to 100), so subtracting
             * it raw added the ancestor scroll to the range (the textarea
             * reported 217px of scroll for 45px of content). */
            int cmax = (inner_bottom - (nd->content.y + scomp)) -
                       nd->content.h;
            if (cmax < 0) {
                cmax = 0;
            }
            nd->scroll_max = cmax;
            auto it = r->scrolls.find(nd->el);
            if (it != r->scrolls.end()) {
                if (it->second > nd->scroll_max) {
                    it->second = nd->scroll_max;
                }
                if (it->second < 0) {
                    it->second = 0;
                }
            }
            /* the scrollable box's subtree bottom in the same unbaked
             * frame as the parent's inner_bottom (children carry scomp;
             * without it a scrolled-away scrollable reported a baked
             * bottom and the parent's auto-height under-counted it) */
            return nd->border.y + nd->border.h + scomp;
        }
        if (nd->el && !nd->is_text &&
            inner_bottom > nd->border.y + nd->border.h + scomp) {
            /* auto-height box whose content (run heights corrected to the
             * real line height) is taller than the layout estimate: grow it
             * with the UNBAKED height (stable across scrolls). Explicit
             * heights stay fixed. */
            std::string hh = sget(nd->style, "height");
            bool h_auto = hh.empty() || hh == "auto";
            if (h_auto) {
                nd->border.h = inner_bottom - nd->border.y - scomp;
                int ch = nd->border.h - nd->padding[0] -
                         nd->padding[2] - nd->border_w[0] -
                         nd->border_w[2];
                if (ch < 0) {
                    ch = 0;
                }
                nd->content.h = ch;
            }
        }
        return inner_bottom;
    };
    fix_sm(r->tree->root, 0);
}

/* grow glyph-level highlight rects up to the line box so the selection wash
 * covers ascenders/descenders with a bit of padding (TTF substring rects
 * only bound the glyphs) */

/* highlight the selected part of a text run (drawn before the glyphs) */
/* text-selection highlight color: ::selection background if set, else the
 * theme accent */
unsigned int sel_hl_color(whaleui_render_t* r, whaleui_layout_node_t* n,
                          unsigned int alpha);


/* text-selection highlight color: ::selection background if set, else the
 * theme accent */

/* blinking caret at byte offset `off` (text-relative coordinates tx,ty) */

/* move the IME text-input area to the caret so the candidate window
 * follows the cursor (SDL_SetTextInputArea; fb coords scaled to window) */

/* checkbox/radio native control (no field chrome; 16px box from layout) */

/* <progress>/<meter>: a track with a filled portion (value/max, clamped) */

/* editable controls: input paints its value text (always); when focused, a
 * selection highlight + caret + IME composition overlay are drawn on top.
 * textarea/contenteditable glyphs come from the layout text run - this only
 * adds the interaction layer. */

/* vertical scrollbar for scrollable containers and the page (html root).
 * Painted over the content on the container's right edge; thumb height
 * tracks the visible fraction, position tracks scroll_y/scroll_max. */


/* --- <select> support --- */

/* draw the current value + arrow (always, from paint_node) */

/* the expanded option list. Painted LAST (highest z), after the whole
 * document, so later siblings cannot cover it. */

/* depth-first hit test; coordinates are absolute (layout boxes are absolute).
 * Children are checked BEFORE the parent box: a child may stick out of the
 * parent (e.g. space-between overflow), and must still be clickable.
 * off_y carries the paint-time scroll offset (see scroll_delta). */
whaleui_layout_node_t* hit_test(whaleui_render_t* r, whaleui_layout_node_t* n,
                                int x, int y, int off_y)
{
    /* pseudo-element boxes (::before/::after state layers, focus
     * underlines) are pointer-events:none: they must not become the
     * hover/click target, or the button under the layer loses hover.
     * Same for real pointer-events:none elements - a full-viewport
     * decorative overlay (q21k's .noise, fixed inset:0 opacity:.05)
     * would otherwise swallow every hover/click, and a fixed header
     * overlay would steal the pointer from the links below it. */
    if (!n || !n->visible || n->pseudo) {
        return nullptr;
    }
    if (!n->is_text && n->el &&
        sget(n->style, "pointer-events") == "none") {
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
    /* checkbox/radio: the painted control is 16x16 but the layout box can
     * be smaller (flex main-size before the UA default -> 11px wide) or
     * taller (line-box height -> 32px). Hit the 16x16 paint area so the
     * whole visible control is clickable, not just the narrow layout box. */
    if (n->el && is_check_radio(n->el) &&
        x >= n->border.x && x < n->border.x + 16 &&
        y >= n->border.y + off_y && y < n->border.y + off_y + 16) {
        return n;
    }
    return nullptr;
}

/* parse "ox oy blur color" (one shadow; color may be rgba(), written
 * without spaces). Returns 0 when no usable shadow is present. */

/* parse_shadow variant that accepts blur == 0 (text-shadow without a
 * blur radius) */

/* whitespace tokenizer */

/* soft shadow under a box: concentric rounded rects expanding up to `blur`
 * px, alpha fading outwards. Painted BEFORE the background so the element
 * body covers the inner layers. */

/* decode an <img> src into a surface. Only local sources load (file:// or
 * a plain path); http(s) and data: URIs have no network stack here and fall
 * back to the placeholder box. Results cached per src (NULL = not
 * loadable). */

/* paint an <img> element: decoded bitmap honoring object-fit, or a
 * placeholder box when the source is missing/remote/undecodable. The bitmap
 * blends into the CPU text layer (GPU path) / framebuffer (CPU path) - the
 * layer composites above geometry, which suits page content images
 * (ponytail: z-order above the parent border is a known corner). */


/* subtree paint bounds: union of this node's border box and every visible
 * descendant (absolute viewport coords - the layout pass already positions
 * nodes absolutely, so no offsets are needed). Computed once per layout
 * pass; partial repaints then cull whole subtrees instead of walking them. */

/* cull a subtree from a repaint when its paint bounds (shifted by the
 * off_x/off_y offset chain: transform translation + scroll delta) are
 * entirely outside the clip. The clip is the dirty region for partial
 * repaints and the whole viewport for full repaints - on a scrolled page
 * most of the document is off-screen either way, so skipping their subtrees
 * is the biggest paint win. A margin covers box-shadow bleed and selection
 * padding. Full-viewport clips cull too; transformed / fixed subtrees are
 * handled by paint_node and sel_seq through the `tc` flag instead (their
 * painted box differs from the laid-out bounds). */



/* --- editing interaction helpers --- */

/* font params of a layout node (matches the paint path) */
/* font-size -> px. The layout pass resolves clamp()/vw against the
 * viewport, but the paint/fix-run side used atoi (clamp -> 0 -> 16px),
 * so a clamp() font-size rendered at the wrong size AND the run heights
 * were overwritten with the 16px metrics (q21k h3 title 22px instead of
 * 33). Resolve the same way the layout does: clamp(MIN,VAL,MAX) /
 * min() / max() with px/em/%/vw/vh terms. */
int font_size_px(whaleui_render_t* r, const std::string& v)
{
    std::string s = v;
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b != std::string::npos && e >= b) {
        s = s.substr(b, e - b + 1);
    }
    auto term_px = [r](const std::string& t, float em) -> float {
        char* end = nullptr;
        float n = std::strtof(t.c_str(), &end);
        if (end == t.c_str()) {
            return 0;
        }
        if (*end == '%') {
            return n * 16.0f / 100.0f; /* % of parent font (approx 16) */
        }
        if (end[0] == 'e' && end[1] == 'm') {
            return n * em;
        }
        if (end[0] == 'v' && end[1] == 'w') {
            return n * static_cast<float>(r->fb_w) / 100.0f;
        }
        if (end[0] == 'v' && end[1] == 'h') {
            return n * static_cast<float>(r->fb_h) / 100.0f;
        }
        return n; /* px / unitless */
    };
    auto split_args = [](const std::string& t, size_t open,
                         std::vector<std::string>& out) {
        size_t depth = 0;
        size_t start = open;
        for (size_t i = open; i < t.size(); ++i) {
            char c = t[i];
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (depth == 0) {
                    break;
                }
                --depth;
            } else if (c == ',' && depth == 0) {
                std::string arg = t.substr(start, i - start);
                size_t ab = arg.find_first_not_of(" \t");
                size_t ae = arg.find_last_not_of(" \t");
                if (ab != std::string::npos) {
                    out.push_back(arg.substr(ab, ae - ab + 1));
                }
                start = i + 1;
            }
        }
        std::string arg = t.substr(start);
        size_t ab = arg.find_first_not_of(" \t");
        size_t ae = arg.find_last_not_of(" \t\r\n");
        if (ab != std::string::npos) {
            std::string last = arg.substr(ab, ae - ab + 1);
            if (!last.empty() && last.back() == ')') {
                last.pop_back();
            }
            out.push_back(last);
        }
    };
    auto fn_val = [&](const std::string& t, float em) -> float {
        if (t.compare(0, 6, "clamp(") == 0) {
            std::vector<std::string> args;
            split_args(t, 6, args);
            if (args.size() != 3) {
                return 0;
            }
            float mn = term_px(args[0], em);
            float val = term_px(args[1], em);
            float mx = term_px(args[2], em);
            if (val < mn) {
                return mn;
            }
            if (val > mx) {
                return mx;
            }
            return val;
        }
        if (t.compare(0, 4, "min(") == 0) {
            std::vector<std::string> args;
            split_args(t, 4, args);
            if (args.size() != 2) {
                return 0;
            }
            float a = term_px(args[0], em);
            float b = term_px(args[1], em);
            return a < b ? a : b;
        }
        if (t.compare(0, 4, "max(") == 0) {
            std::vector<std::string> args;
            split_args(t, 4, args);
            if (args.size() != 2) {
                return 0;
            }
            float a = term_px(args[0], em);
            float b = term_px(args[1], em);
            return a > b ? a : b;
        }
        return term_px(t, em);
    };
    float em = 16.0f;
    float px = fn_val(s, em);
    if (px <= 0 || px > 2048.0f) {
        return 0;
    }
    return static_cast<int>(px);
}

void node_font(whaleui_render_t* r, whaleui_layout_node_t* n, int* fs,
               std::string* family, bool* bold)
{
    *fs = 16;
    std::string fsv = sget(n->style, "font-size");
    if (!fsv.empty()) {
        int f = font_size_px(r, fsv);
        if (f > 0) {
            *fs = f;
        }
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

/* byte offset in a text-run node at (x,y) */
size_t byte_at_node(whaleui_render_t* r, whaleui_layout_node_t* hit, int x, int y)
{
    int fs;
    std::string family;
    bool bold;
    node_font(r, hit, &fs, &family, &bold);
    int tx = 0, ty = 0;
    int ww = run_wrap_w(hit);
    text_origin(r, hit, hit->text, fs, family, bold, &tx, &ty, ww);
    int off = node_scroll_off(r, hit);
    return byte_at_text(r, hit->text, fs, family, bold, x - tx, y - ty - off,
                        ww);
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
    node_font(r, box, &fs, &family, &bold);
    int tx = 0, ty = 0;
    int ww = tag_eq(el, "input") ? 0 : run_wrap_w(geo);
    text_origin(r, geo, val, fs, family, bold, &tx, &ty, ww);
    int off = node_scroll_off(r, hit);
    /* single-line input content scrolls horizontally with the caret */
    int hx = 0;
    if (tag_eq(el, "input")) {
        auto hi = r->hscrolls.find(el);
        hx = hi == r->hscrolls.end() ? 0 : hi->second;
    }
    return byte_at_text(r, val, fs, family, bold, x - tx + hx, y - ty - off,
                        ww);
}

/* word/line helpers are defined below (editing section) */
size_t line_start(const std::string& s, size_t b);
size_t line_end(const std::string& s, size_t b);
static void word_range(const std::string& s, size_t off, size_t* ws, size_t* we);

/* drag: move the selection focus end to the element under the mouse.
 * sel_mode extends by word (double-click) or line (triple-click) so the
 * drag continues in the same unit the click started. The fixed end stays
 * on the boundary AWAY from the drag direction: dragging left keeps the
 * right boundary (and vice versa), so the originally selected word/line
 * never shrinks away from the anchor side. */
void update_selection_focus(whaleui_render_t* r, whaleui_layout_node_t* hit,
                            int x, int y)
{
    if (!hit) {
        return;
    }
    auto extend = [r](const std::string& text, size_t off, bool editable,
                      size_t* lo, size_t* hi) {
        if (r->sel_mode == 1) {
            size_t ws = 0, we = 0;
            word_range(text, off, &ws, &we);
            if (off <= static_cast<size_t>(r->sel_drag_anchor)) {
                /* dragging left of the click anchor: extend left, keep the
                 * original right edge fixed */
                *lo = ws;
                *hi = static_cast<size_t>(r->sel_drag_focus);
            } else {
                /* dragging right (or back into the original word): the left
                 * edge stays at the click anchor, the right edge follows -
                 * dragging back collapses the added words again */
                *lo = static_cast<size_t>(r->sel_drag_anchor);
                *hi = we;
            }
        } else if (r->sel_mode == 2) {
            if (off <= static_cast<size_t>(r->sel_drag_anchor)) {
                *lo = line_start(text, off);
                *hi = static_cast<size_t>(r->sel_drag_focus);
            } else {
                *lo = static_cast<size_t>(r->sel_drag_anchor);
                *hi = line_end(text, off);
            }
        } else {
            int a = r->sel_anchor < r->sel_focus ? r->sel_anchor : r->sel_focus;
            *lo = static_cast<size_t>(a);
            *hi = off;
        }
        (void)editable;
    };
    if (hit->is_text) {
        r->sel_focus_el = hit->el;
        size_t off = static_cast<size_t>(byte_at_node(r, hit, x, y));
        size_t lo = 0, hi = 0;
        extend(hit->text, off, false, &lo, &hi);
        if (r->sel_mode == 0) {
            r->sel_focus = static_cast<int>(off);
        } else {
            r->sel_anchor = static_cast<int>(lo);
            r->sel_focus = static_cast<int>(hi);
        }
        /* repaint only: the highlight reads r->sel_*, the layout tree is
         * unchanged, so dragging must not relayout every mouse move */
        r->scroll_dirty = 1;
    } else if (is_editable(hit->el)) {
        r->sel_focus_el = hit->el;
        std::string val = edit_value(hit->el);
        size_t off = static_cast<size_t>(caret_from_point(r, hit->el, hit, x, y));
        size_t lo = 0, hi = 0;
        extend(val, off, true, &lo, &hi);
        if (r->sel_mode == 0) {
            r->sel_focus = static_cast<int>(off);
        } else {
            r->sel_anchor = static_cast<int>(lo);
            r->sel_focus = static_cast<int>(hi);
        }
        r->scroll_dirty = 1;
    }
}

/* keep the caret visible after an edit; defined below */
static void edit_ensure_visible(whaleui_render_t* r);
whaleui_layout_node_t* find_node_by_el(whaleui_layout_tree_t* tree,
                                       lxb_dom_element* el);
bool is_contenteditable(lxb_dom_element* el); /* defined below */

/* split a string into lines; a trailing \n keeps an empty last line
 * (matches the text layout's line model) */
static std::vector<std::string> split_lines(const std::string& s)
{
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        size_t q = s.find('\n', p);
        if (q == std::string::npos) {
            out.push_back(s.substr(p));
            break;
        }
        out.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    return out;
}

static std::string join_lines(const std::vector<std::string>& ls)
{
    std::string out;
    for (size_t i = 0; i < ls.size(); ++i) {
        if (i) {
            out += '\n';
        }
        out += ls[i];
    }
    return out;
}

/* line-level replace of [a,b) of el's value with `ins`: operates on the
 * cached line array (built from the DOM on first use), touches only the
 * affected lines, and writes the result back to the DOM (which stays the
 * single source of truth for measurement/rendering). */
static std::string edit_lines_replace(whaleui_render_t* r,
                                      lxb_dom_element* el, size_t a,
                                      size_t b, const std::string& ins)
{
    auto& lines = r->edit_lines[el];
    if (lines.empty()) {
        lines = split_lines(edit_value(el));
    }
    /* byte offset -> (row, col): each line contributes len+1 (the \n) */
    size_t pos = 0;
    size_t r1 = 0, c1 = 0, r2 = 0, c2 = 0;
    bool f1 = false, f2 = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!f1 && a <= pos + lines[i].size()) {
            r1 = i;
            c1 = a - pos;
            f1 = true;
        }
        if (!f2 && b <= pos + lines[i].size()) {
            r2 = i;
            c2 = b - pos;
            f2 = true;
        }
        if (f1 && f2) {
            break;
        }
        pos += lines[i].size() + 1;
    }
    if (!f1) {
        r1 = lines.size() - 1;
        c1 = lines[r1].size();
    }
    if (!f2) {
        r2 = lines.size() - 1;
        c2 = lines[r2].size();
    }
    std::string pre = lines[r1].substr(0, c1);
    std::string suf = lines[r2].substr(c2);
    std::string mid = pre + ins + suf;
    lines.erase(lines.begin() + r1, lines.begin() + r2 + 1);
    std::vector<std::string> ml = split_lines(mid);
    lines.insert(lines.begin() + r1, ml.begin(), ml.end());
    std::string nv = join_lines(lines);
    edit_set_value(el, nv);
    return nv;
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
    /* push an undo record (replace [a,b) with insertion, dropping del);
     * typed keystrokes are coalesced by resetting redo. No record when the
     * edit is a no-op. */
    if (!(insertion.empty() && a == b)) {
        whaleui_render_t::EditOp op = {el, a, b, insertion,
                                       val.substr(a, b - a)};
        r->undo_stack.push_back(op);
        r->redo_stack.clear();
    }
    std::string nv;
    if (is_contenteditable(el)) {
        /* line storage: only the affected lines are rebuilt */
        nv = edit_lines_replace(r, el, a, b, insertion);
    } else {
        nv = val.substr(0, a) + insertion + val.substr(b);
        edit_set_value(el, nv);
    }
    size_t caret = a + insertion.size();
    r->sel_anchor_el = r->sel_focus_el = el;
    r->sel_anchor = r->sel_focus = static_cast<int>(caret);
    r->compose.clear();
    r->nav_col = -1; /* the text changed: the remembered column is stale */
    r->has_dirty = 1;
    r->edit_scroll_need = 1;
    edit_ensure_visible(r);
}

/* Ctrl-Z: apply the last edit in reverse and move the caret to the start
 * of the undone range; the record moves to the redo stack. */
static void edit_undo(whaleui_render_t* r)
{
    if (r->undo_stack.empty() || !r->tree) {
        return;
    }
    whaleui_render_t::EditOp op = r->undo_stack.back();
    r->undo_stack.pop_back();
    std::string val = edit_value(op.el);
    if (op.a > val.size()) {
        op.a = val.size();
    }
    size_t e = op.a + op.ins.size();
    if (e > val.size()) {
        e = val.size();
    }
    std::string nv = val.substr(0, op.a) + op.del + val.substr(e);
    edit_set_value(op.el, nv);
    r->edit_lines.erase(op.el); /* line cache is stale after a DOM edit */
    r->edit_el = op.el;
    r->sel_anchor_el = r->sel_focus_el = op.el;
    r->sel_anchor = r->sel_focus = static_cast<int>(op.a);
    r->nav_col = -1;
    r->compose.clear();
    r->has_dirty = 1;
    r->redo_stack.push_back(op);
    r->edit_scroll_need = 1;
    edit_ensure_visible(r);
}

/* Ctrl-Y / Ctrl-Shift-Z: re-apply the last undone edit; the record moves
 * back to the undo stack. */
static void edit_redo(whaleui_render_t* r)
{
    if (r->redo_stack.empty() || !r->tree) {
        return;
    }
    whaleui_render_t::EditOp op = r->redo_stack.back();
    r->redo_stack.pop_back();
    std::string val = edit_value(op.el);
    if (op.a > val.size()) {
        op.a = val.size();
    }
    size_t e = op.a + op.del.size();
    if (e > val.size()) {
        e = val.size();
    }
    std::string nv = val.substr(0, op.a) + op.ins + val.substr(e);
    edit_set_value(op.el, nv);
    r->edit_lines.erase(op.el); /* line cache is stale after a DOM edit */
    r->edit_el = op.el;
    r->sel_anchor_el = r->sel_focus_el = op.el;
    r->sel_anchor = r->sel_focus = static_cast<int>(op.a + op.ins.size());
    r->nav_col = -1;
    r->compose.clear();
    r->has_dirty = 1;
    r->undo_stack.push_back(op);
    r->edit_scroll_need = 1;
    edit_ensure_visible(r);
}

/* after an edit (or caret move), scroll the editable's box so the caret
 * stays visible: textarea/contenteditable scroll vertically, single-line
 * inputs scroll their content horizontally - the control never grows */
static void edit_ensure_visible(whaleui_render_t* r)
{
    if (!r->edit_el || !r->tree) {
        return;
    }
    whaleui_layout_node_t* n = find_node_by_el(r->tree, r->edit_el);
    if (!n) {
        return;
    }
    std::string val = edit_value(r->edit_el);
    int fs;
    std::string family;
    bool bold;
    node_font(r, n, &fs, &family, &bold);
    whaleui_layout_node_t* geo = editable_geo(n);
    int wrap_w = tag_eq(r->edit_el, "input") ? 0 : run_wrap_w(geo);
    int cx = 0, cy = 0, ch = 16;
    caret_pos(r, val, fs, family, bold,
              static_cast<size_t>(r->sel_focus), &cx, &cy, &ch, wrap_w);
    int tx = 0, ty = 0;
    text_origin(r, geo, val, fs, family, bold, &tx, &ty, wrap_w);
    int off = node_scroll_off(r, n);
    if (tag_eq(r->edit_el, "input")) {
        /* horizontal: keep the caret inside the content box. The text
         * paints at tx - hscroll, so the caret sits at tx - hscroll + cx. */
        int& hcur = r->hscrolls[r->edit_el];
        int hw = n->content.w > 0 ? n->content.w : 0;
        int nv = hcur;
        if (nv < cx + ch - hw) {
            nv = cx + ch - hw; /* caret right edge past the box */
        }
        if (nv > cx) {
            nv = cx; /* caret left of the box */
        }
        if (nv < 0) {
            nv = 0;
        }
        if (nv != hcur) {
            hcur = nv;
            r->scroll_dirty = 1;
        }
        return;
    }
    if (n->scroll_max <= 0) {
        return;
    }
    /* vertical: keep the caret line inside the visible area */
    long base = static_cast<long>(ty) + cy + off + n->scroll_y;
    long top = static_cast<long>(n->content.y) + off;
    long bot = top + n->content.h;
    int& cur = r->scrolls[r->edit_el];
    int nv = cur;
    long need_lo = base + ch - bot;
    long need_hi = base - top;
    if (nv < need_lo) {
        nv = static_cast<int>(need_lo);
    }
    if (nv > need_hi) {
        nv = static_cast<int>(need_hi);
    }
    if (nv > n->scroll_max) {
        nv = n->scroll_max;
    }
    if (nv < 0) {
        nv = 0;
    }
    if (nv != cur) {
        cur = nv;
        r->scroll_dirty = 1;
    }
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
    if (b == 0) {
        return 0;
    }
    size_t p = s.rfind('\n', b - 1);
    return p == std::string::npos ? 0 : p + 1;
}
size_t line_end(const std::string& s, size_t b)
{
    size_t p = s.find('\n', b);
    return p == std::string::npos ? s.size() : p;
}

/* character column of byte b within its line (for up/down navigation) */
static size_t col_of(const std::string& s, size_t b)
{
    size_t ls = line_start(s, b);
    size_t col = 0;
    for (size_t p = ls; p < b && p < s.size(); p = utf8_next(s, p)) {
        ++col;
    }
    return col;
}
/* byte offset of the col-th character in the line starting at line_off
 * (clamped to the line end) */
static size_t col_at(const std::string& s, size_t line_off, size_t col)
{
    size_t le = line_end(s, line_off);
    size_t p = line_off;
    size_t c = 0;
    while (p < le && c < col) {
        p = utf8_next(s, p);
        ++c;
    }
    return p;
}

/* --- word navigation (ctrl+arrows, double-click, drag continuation) --- */

/* decode one UTF-8 codepoint at byte b (b must be a char boundary) */
static unsigned int cp_at(const std::string& s, size_t b, size_t* len)
{
    const unsigned char* u = reinterpret_cast<const unsigned char*>(s.c_str());
    unsigned char c = u[b];
    if (c < 0x80) {
        if (len) { *len = 1; }
        return c;
    }
    if ((c & 0xE0) == 0xC0 && b + 1 < s.size()) {
        if (len) { *len = 2; }
        return ((c & 0x1F) << 6) | (u[b + 1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && b + 2 < s.size()) {
        if (len) { *len = 3; }
        return ((c & 0x0F) << 12) | ((u[b + 1] & 0x3F) << 6) |
               (u[b + 2] & 0x3F);
    }
    if (len) { *len = 4; }
    return ((c & 0x07) << 18) | ((u[b + 1] & 0x3F) << 12) |
           ((u[b + 2] & 0x3F) << 6) | (u[b + 3] & 0x3F);
}

/* CJK unified ideographs (incl. ext A + compatibility) */
static bool is_cjk_cp(unsigned int cp)
{
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

/* whitespace + punctuation (ASCII and CJK); word separators.
 * lite/minimal only - the full build classifies via utf8proc. */
#ifndef WHALEUI_BUILD_FULL
static bool is_sep_cp(unsigned int cp)
{
    if (cp <= 0x7F) {
        return !((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                 (cp >= '0' && cp <= '9') || cp == '_');
    }
    if (cp == 0x3000 || cp == 0x00A0) {
        return true;
    }
    if (cp >= 0x3001 && cp <= 0x303F) {   /* CJK punctuation */
        return true;
    }
    if (cp >= 0xFF01 && cp <= 0xFF65 &&   /* full-width punct (excl. kana) */
        !(cp >= 0xFF10 && cp <= 0xFF19) &&   /* full-width digits */
        !(cp >= 0xFF21 && cp <= 0xFF3A) &&   /* full-width A-Z */
        !(cp >= 0xFF41 && cp <= 0xFF5A)) {   /* full-width a-z */
        return true;
    }
    if (cp >= 0x2000 && cp <= 0x206F) {   /* general punctuation */
        return true;
    }
    if (cp >= 0x2010 && cp <= 0x2027 || cp >= 0x2030 && cp <= 0x205E) {
        return true;
    }
    if (cp >= 0xFE30 && cp <= 0xFE4F) {   /* CJK compat forms */
        return true;
    }
    return false;
}
#endif /* !WHALEUI_BUILD_FULL */

/* word class: 0 = separator, 1 = word char, 2 = CJK ideograph.
 * Full build classifies via utf8proc's Unicode categories (correct for
 * every script: Greek/Cyrillic/Arabic punctuation, full-width forms, ...);
 * lite/minimal use the compact hand-rolled table below (no storage cost). */
#ifdef WHALEUI_BUILD_FULL
static int word_class(unsigned int cp)
{
    if (is_cjk_cp(cp)) {
        return 2;
    }
    switch (utf8proc_category(cp)) {
    case UTF8PROC_CATEGORY_PC:
    case UTF8PROC_CATEGORY_PD:
    case UTF8PROC_CATEGORY_PS:
    case UTF8PROC_CATEGORY_PE:
    case UTF8PROC_CATEGORY_PI:
    case UTF8PROC_CATEGORY_PF:
    case UTF8PROC_CATEGORY_PO:
    case UTF8PROC_CATEGORY_ZS:
    case UTF8PROC_CATEGORY_ZL:
    case UTF8PROC_CATEGORY_ZP:
    case UTF8PROC_CATEGORY_CC:
    case UTF8PROC_CATEGORY_CF:
        return 0;
    default:
        return 1;
    }
}
#else
static int word_class(unsigned int cp)
{
    if (is_sep_cp(cp)) {
        return 0;
    }
    return is_cjk_cp(cp) ? 2 : 1;
}
#endif

/* minimal builds: each hanzi is its own word (no segmentation, no
 * dictionary). Full (~20k words) and lite (~1k words) use the compact
 * dictionary for real word segmentation. */
#ifdef WHALEUI_BUILD_MINIMAL
static bool cjk_chars_split(void) { return true; }
#else
static bool cjk_chars_split(void) { return false; }
#endif

#if defined(WHALEUI_BUILD_FULL) || defined(WHALEUI_BUILD_LITE)
/* --- dictionary segmentation (full/lite builds) --- */

/* longest-match lookup over the loaded dictionary (res/whaleui_dict.bin
 * with a built-in fallback when the file is missing) */
static bool cjk_dict_has(const std::string& w)
{
    return whaleui_cjk_dict_has(w.c_str(), static_cast<int>(w.size())) != 0;
}

/* collect up to 4 consecutive CJK chars starting at byte b */
static int cjk_chars_at(const std::string& s, size_t b, std::string* chars,
                        size_t* pos)
{
    int n = 0;
    pos[0] = b;
    size_t p = b;
    while (n < 4 && p < s.size()) {
        size_t l = 0;
        unsigned int cp = cp_at(s, p, &l);
        if (!is_cjk_cp(cp)) {
            break;
        }
        chars[n] = s.substr(p, l);
        p += l;
        pos[n + 1] = p;
        ++n;
    }
    return n;
}

/* dict word end: the longest dict word starting at b (else single char) */
static size_t cjk_word_end(const std::string& s, size_t b)
{
    std::string ch[4];
    size_t pos[5];
    int n = cjk_chars_at(s, b, ch, pos);
    if (n == 0) {
        return utf8_next(s, b);
    }
    for (int k = n >= 4 ? 4 : n; k >= 2; --k) {
        std::string w;
        for (int i = 0; i < k; ++i) {
            w += ch[i];
        }
        if (cjk_dict_has(w)) {
            return pos[k];
        }
    }
    return pos[1];
}

/* dict word start: the longest dict word ending at p (else single char) */
static size_t cjk_word_start(const std::string& s, size_t p)
{
    std::vector<std::string> ch;
    std::vector<size_t> st;
    size_t q = p;
    while (ch.size() < 4) {
        size_t l = 0;
        unsigned int cp = cp_at(s, q, &l);
        if (!is_cjk_cp(cp)) {
            break;
        }
        ch.insert(ch.begin(), s.substr(q, l));
        st.insert(st.begin(), q);
        if (q == 0) {
            break;
        }
        q = utf8_prev(s, q);
    }
    if (ch.empty()) {
        return p;
    }
    for (int k = static_cast<int>(ch.size()); k >= 2; --k) {
        std::string w;
        for (int i = static_cast<int>(ch.size()) - k;
             i < static_cast<int>(ch.size()); ++i) {
            w += ch[i];
        }
        if (cjk_dict_has(w)) {
            return st[static_cast<int>(ch.size()) - k];
        }
    }
    return p;
}

/* dict word [ws,we) containing byte off (off on a CJK char) */
static void cjk_word_range(const std::string& s, size_t off, size_t* ws,
                           size_t* we)
{
    size_t starts[4];
    int nstart = 0;
    starts[nstart++] = off;
    size_t q = off;
    for (int i = 0; i < 3 && q > 0; ++i) {
        q = utf8_prev(s, q);
        size_t l = 0;
        if (!is_cjk_cp(cp_at(s, q, &l))) {
            break;
        }
        starts[nstart++] = q;
    }
    size_t best_s = off, best_e = utf8_next(s, off);
    int best_len = 0;
    for (int si = 0; si < nstart; ++si) {
        std::string ch[4];
        size_t pos[5];
        int n = cjk_chars_at(s, starts[si], ch, pos);
        for (int k = 2; k <= (n >= 4 ? 4 : n); ++k) {
            std::string w;
            for (int i = 0; i < k; ++i) {
                w += ch[i];
            }
            if (starts[si] <= off && pos[k] > off && cjk_dict_has(w) &&
                k > best_len) {
                best_len = k;
                best_s = starts[si];
                best_e = pos[k];
            }
        }
    }
    *ws = best_s;
    *we = best_e;
}
#endif /* WHALEUI_BUILD_FULL */

/* byte offset of the end of the word at/after b (ctrl+right) */
static size_t word_next(const std::string& s, size_t b)
{
    const size_t n = s.size();
    if (b >= n) {
        return n;
    }
    size_t l = 0;
    int cls = word_class(cp_at(s, b, &l));
    if (cls == 0) {
        /* skip separators to the next word */
        while (b < n) {
            int c2 = word_class(cp_at(s, b, &l));
            if (c2 != 0) {
                cls = c2;
                break;
            }
            b += l;
        }
        if (b >= n) {
            return n;
        }
    }
    cp_at(s, b, &l);
    if (cls == 2 && cjk_chars_split()) {
        return b + l; /* minimal: single hanzi */
    }
#if defined(WHALEUI_BUILD_FULL) || defined(WHALEUI_BUILD_LITE)
    if (cls == 2) {
        return cjk_word_end(s, b); /* dict: word end */
    }
#endif
    /* walk to the end of the same-class run */
    while (b < n) {
        int c2 = word_class(cp_at(s, b, &l));
        if (c2 != cls) {
            break;
        }
        b += l;
    }
    return b;
}

/* byte offset of the start of the word before/at b (ctrl+left) */
static size_t word_prev(const std::string& s, size_t b)
{
    const size_t n = s.size();
    if (b == 0) {
        return 0;
    }
    if (b > n) {
        b = n;
    }
    size_t p = utf8_prev(s, b);
    size_t l = 0;
    int cls = word_class(cp_at(s, p, &l));
    if (cls == 0) {
        /* skip the separator run leftwards */
        while (p > 0 && word_class(cp_at(s, utf8_prev(s, p), nullptr)) == 0) {
            p = utf8_prev(s, p);
        }
        if (p == 0) {
            return 0;
        }
        p = utf8_prev(s, p); /* last char of the previous word */
        cls = word_class(cp_at(s, p, &l));
    }
    if (cls == 2 && cjk_chars_split()) {
        return p; /* minimal: single hanzi */
    }
#if defined(WHALEUI_BUILD_FULL) || defined(WHALEUI_BUILD_LITE)
    if (cls == 2) {
        return cjk_word_start(s, p); /* dict: word start */
    }
#endif
    while (p > 0) {
        size_t q = utf8_prev(s, p);
        if (word_class(cp_at(s, q, nullptr)) != cls) {
            break;
        }
        p = q;
    }
    return p;
}

/* [ws,we) = the word containing byte off (double-click) */
static void word_range(const std::string& s, size_t off, size_t* ws, size_t* we)
{
    const size_t n = s.size();
    *ws = *we = 0;
    if (n == 0) {
        return;
    }
    if (off > n) {
        off = n;
    }
    size_t p = off < n ? off : utf8_prev(s, n);
    size_t l = 0;
    int cls = word_class(cp_at(s, p, &l));
    if (cls == 0) {
        /* on a separator: use the word before it (or the first word) */
        if (p == 0) {
            size_t q = 0;
            while (q < n) {
                if (word_class(cp_at(s, q, &l)) != 0) {
                    p = q;
                    cls = word_class(cp_at(s, p, &l));
                    break;
                }
                q += l;
            }
            if (cls == 0) {
                return; /* all separators: empty word */
            }
        } else {
            p = utf8_prev(s, p);
            cls = word_class(cp_at(s, p, &l));
            if (cls == 0) {
                /* separator run: select the word after it */
                size_t q = off;
                while (q < n) {
                    if (word_class(cp_at(s, q, &l)) != 0) {
                        p = q;
                        cls = word_class(cp_at(s, p, &l));
                        break;
                    }
                    q += l;
                }
                if (cls == 0) {
                    *ws = *we = n;
                    return;
                }
            }
        }
    }
    size_t w0 = p;
#ifdef WHALEUI_BUILD_FULL
    if (cls == 2) {
        /* dict: the dictionary word containing the click point */
        cjk_word_range(s, p, &w0, we);
        *ws = w0;
        return;
    }
#endif
    while (w0 > 0) {
        size_t q = utf8_prev(s, w0);
        if (word_class(cp_at(s, q, nullptr)) != cls) {
            break;
        }
        if (cls == 2 && cjk_chars_split()) {
            break;
        }
        w0 = q;
    }
    size_t w1 = p;
    while (w1 < n) {
        size_t q = utf8_next(s, w1);
        if (q >= n || word_class(cp_at(s, q, nullptr)) != cls) {
            break;
        }
        if (cls == 2 && cjk_chars_split()) {
            break;
        }
        w1 = q;
    }
    *ws = w0;
    *we = utf8_next(s, w1);
}

void edit_key(whaleui_render_t* r, int keycode, int mods)
{
    lxb_dom_element* el = r->edit_el;
    if (!el) {
        return;
    }
    std::string val = edit_value(el);
    int anchor = r->sel_anchor, focus = r->sel_focus;
    int a = anchor < focus ? anchor : focus;
    int b = anchor < focus ? focus : anchor;
    bool ctrl = (mods & SDL_KMOD_CTRL) != 0;
    bool shift = (mods & SDL_KMOD_SHIFT) != 0;
    if (ctrl) {
        switch (keycode) {
        case 'a': /* select all */
            r->sel_anchor = 0;
            r->sel_focus = static_cast<int>(val.size());
            r->has_dirty = 1;
            return;
        case 'c': /* copy the selection to the system clipboard */
            if (a != b) {
                SDL_SetClipboardText(val.substr(static_cast<size_t>(a),
                                                static_cast<size_t>(b - a)).c_str());
            }
            return;
        case 'x': /* cut: copy + delete */
            if (a != b) {
                SDL_SetClipboardText(val.substr(static_cast<size_t>(a),
                                                static_cast<size_t>(b - a)).c_str());
                edit_replace(r, el, static_cast<size_t>(a), static_cast<size_t>(b), "");
            }
            return;
        case 'v': { /* paste from the system clipboard */
            char* cl = SDL_GetClipboardText();
            if (cl) {
                edit_replace(r, el, static_cast<size_t>(a), static_cast<size_t>(b),
                             cl);
                SDL_free(cl);
            }
            return;
        }
        case 'z':
            if ((mods & SDL_KMOD_SHIFT) != 0) {
                edit_redo(r); /* ctrl+shift+z */
            } else {
                edit_undo(r);
            }
            return;
        case 'y':
            edit_redo(r);
            return;
        default:
            break; /* ctrl+arrows/others fall through to movement */
        }
    }
    /* movement keys (arrows/home/end): collapse the selection to its
     * leading end first when shift is not held */
    bool moving = keycode == WHALEUI_KEY_LEFT || keycode == WHALEUI_KEY_RIGHT ||
                  keycode == WHALEUI_KEY_UP || keycode == WHALEUI_KEY_DOWN ||
                  keycode == WHALEUI_KEY_HOME || keycode == WHALEUI_KEY_END;
    if (moving && !shift && anchor != focus) {
        bool left_ward = keycode == WHALEUI_KEY_LEFT ||
                         keycode == WHALEUI_KEY_HOME ||
                         keycode == WHALEUI_KEY_UP;
        r->sel_anchor = r->sel_focus = left_ward ? a : b;
        r->has_dirty = 1;
        return;
    }
    /* apply a new focus position: shift keeps the anchor (extend the
     * selection), otherwise the caret moves alone */
    auto set = [&](size_t nf) {
        if (shift) {
            r->sel_focus = static_cast<int>(nf);
        } else {
            r->sel_anchor = r->sel_focus = static_cast<int>(nf);
        }
        r->has_dirty = 1;
        r->edit_scroll_need = 1;
        edit_ensure_visible(r); /* keep the caret in view while navigating */
    };
    size_t cur = static_cast<size_t>(focus);
    switch (keycode) {
    case WHALEUI_KEY_LEFT:
        r->nav_col = -1;
        set(ctrl ? word_prev(val, cur) : utf8_prev(val, cur));
        break;
    case WHALEUI_KEY_RIGHT:
        r->nav_col = -1;
        set(ctrl ? word_next(val, cur) : utf8_next(val, cur));
        break;
    case WHALEUI_KEY_HOME:
        r->nav_col = -1;
        set(ctrl ? 0 : line_start(val, cur));
        break;
    case WHALEUI_KEY_END:
        r->nav_col = -1;
        set(ctrl ? val.size() : line_end(val, cur));
        break;
    case WHALEUI_KEY_UP: { /* keep the character column across lines */
        if (r->nav_col < 0) {
            r->nav_col = static_cast<int>(col_of(val, cur));
        }
        size_t ls = line_start(val, cur);
        if (ls > 0) {
            set(col_at(val, line_start(val, ls - 1),
                       static_cast<size_t>(r->nav_col)));
        }
        break;
    }
    case WHALEUI_KEY_DOWN: {
        if (r->nav_col < 0) {
            r->nav_col = static_cast<int>(col_of(val, cur));
        }
        size_t le = line_end(val, cur);
        if (le < val.size()) {
            set(col_at(val, le + 1, static_cast<size_t>(r->nav_col)));
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


/* scrollbar drag helpers (defined with the wheel/click handling below) */
whaleui_layout_node_t* scrollbar_under(whaleui_render_t* r,
                                       whaleui_layout_node_t* hit, int x,
                                       int y);
void update_drag_scroll(whaleui_render_t* r, int y);
SDL_Cursor* render_cursor(whaleui_render_t* r, SDL_SystemCursor id);

/* --- FSR 1.0 (GPU compute) resources --- */

/* build compute pipelines + textures for the current window size. Returns 1
 * when everything is usable (FSR can run), 0 if any resource failed. */


/* should the current frame use the FSR path? mode 0 = auto. */

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
    r->async_layout = 0;
    r->layout_thread = nullptr;
    r->layout_done.store(0, std::memory_order_relaxed);
    r->layout_pending = nullptr;
    r->tc_tick = 0;
    r->tc_bytes = 0;
#ifdef WHALEUI_BUILD_FULL
    /* real glyph widths for the layout pass (inline x, wrap points) */
    whaleui_layout_set_text_metric(render_text_metric);
    whaleui_layout_set_line_height_metric(render_line_height);
    whaleui_layout_set_wrap_lines_metric(render_wrap_lines);
#endif
    r->cursor_arrow = nullptr;
    r->cursor_text = nullptr;
    r->cursor_pointer = nullptr;
    r->ascii_font = nullptr;
    r->ascii_fs = 0;
    r->anim = whaleui_anim_create();
    r->text_scale = 1.0f;
    r->nav_col = -1;
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
    /* FSR resources are NOT created up front: the fsr_up/fsr_out textures
     * are window-sized (14MB+ at 2k each) and the pipelines only matter
     * while FSR is on - the frame's fsr_want_active branch creates them on
     * the first frame that actually needs FSR. */

    /* default font: register the platform UI fonts (Latin/CJK/emoji
     * fallback chain), then open the first one that actually loads as the
     * default (no assumption that a specific family exists - a stripped
     * Win8 without Segoe UI still gets Arial/SimSun). Fallbacks load
     * lazily on the first missing glyph (registered-but-unused fonts pin
     * no file data), so startup memory is one Latin font, not 3 font
     * files. */
#ifdef WHALEUI_BUILD_FULL
    whaleui_font_register_system_defaults();
    whaleui_font_registry* reg = whaleui_font_registry_get();
    for (size_t i = 0; i < reg->count; ++i) {
        if (!reg->fonts[i].path) {
            continue;
        }
        SDL_IOStream* io = SDL_IOFromFile(reg->fonts[i].path, "rb");
        if (io) {
            r->font_default = TTF_OpenFontIO(io, true, 16.0f);
            if (r->font_default) {
                r->default_family = reg->fonts[i].family;
                break;
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
    /* join a running async layout worker before freeing anything it reads
     * (rules, DOM, vars). The worker only reads; join is safe. */
    if (r->layout_thread) {
        r->layout_thread->join();
        delete r->layout_thread;
        r->layout_thread = nullptr;
    }
    if (r->layout_pending) {
        whaleui_layout_destroy(r->layout_pending);
        r->layout_pending = nullptr;
    }
    whaleui_layout_destroy(r->tree);
    if (g_last_tree == r->tree) {
        g_last_tree = nullptr; /* the tree this pointer referenced is gone */
    }
    if (g_metric_render == r) {
        g_metric_render = nullptr; /* metric hook must not dangle */
    }
    whaleui_anim_destroy(r->anim);
    if (r->rules) {
        whaleui_css_rules_destroy(r->rules, r->rule_count);
    }
    whaleui_css_keyframes_destroy(&r->keyframes);
#ifdef WHALEUI_BUILD_FULL
    /* fonts are shared across cache keys (one TTF_Font per family, reused
     * by every generic/missing family), so close each unique font once */
    std::set<TTF_Font*> to_close;
    for (auto& f : r->fonts) {
        to_close.insert(f.second);
    }
    for (TTF_Font* fb : r->fallback_open) {
        to_close.insert(fb);
    }
    to_close.insert(r->font_default);
    for (TTF_Font* f : to_close) {
        if (f) {
            TTF_CloseFont(f);
        }
    }
    /* text_cache 鐨勬爡鏍煎寲缂撳啿鏄?std::vector,闅?map 鏋愭瀯鑷姩閲婃斁 */
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
    /* the cascade/vars caches depend on the old rules + theme vars */
    if (r->tree) {
        r->tree->style_cache.clear();
        r->tree->vars.clear();
        r->tree->vars_collected = false;
    }
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
    /* text_cache 缂撳啿涓?std::vector,鐩存帴娓呯┖鍗冲彲 */
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
    r->hscrolls.clear();
    r->last_scrolls.clear();
    r->hover_el = nullptr;
    r->hover_old_el = nullptr;
    r->edit_scroll_need = 0;
    r->focus_el = nullptr;
    r->pressed_el = nullptr;
    r->sel_anchor_el = nullptr;
    r->sel_focus_el = nullptr;
    r->sel_anchor = r->sel_focus = 0;
    r->selecting = 0;
    r->edit_el = nullptr;
    r->compose.clear();
    r->undo_stack.clear();
    r->redo_stack.clear();
    r->edit_lines.clear();
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
    /* GPU render targets (geometry ping-pong + composite) must follow the
     * new size. Only the size-bound textures are rebuilt - the compiled
     * shaders/pipelines survive (a full recreate recompiles every shader
     * with DXC, which froze the window mid-drag). */
    if (whaleui_gpu_resize(r->gpu, width, height) != 0) {
        return -2;
    }
    /* FSR textures are window-sized: drop them; they are recreated lazily
     * the next time FSR activates (see frame's fsr_want_active branch). */
    render_fsr_destroy(r);
    r->fsr_active = 0;
    r->has_dirty = 1;
    /* stale scroll deltas from the old size would make the first scroll
     * after a resize shift by the accumulated difference */
    r->last_scrolls.clear();
    return 0;
}

/* find the layout node for a DOM element in the current tree (O(1): the
 * tree keeps an element -> node map built during the layout pass) */
whaleui_layout_node_t* find_node_by_el(whaleui_layout_tree_t* tree,
                                       lxb_dom_element* el)
{
    if (!tree || !el) {
        return nullptr;
    }
    auto it = tree->by_el.find(el);
    return it == tree->by_el.end() ? nullptr : it->second;
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
        whaleui_layout_node_t* s = find_node_by_el(r->tree, r->open_select);
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
     * within its name group, matching browser behavior). A <label> click
     * forwards to its associated input (the wrapped one, or for="id"). */
    lxb_dom_element* toggle_el = nullptr;
    if (hit && hit->el && is_check_radio(hit->el)) {
        toggle_el = hit->el;
    } else if (hit && hit->el && tag_eq(hit->el, "label")) {
        lxb_dom_element* lab = hit->el;
        /* wrapped input: first input descendant of the label (the common
         * <label><input>...</label> form). for="id" association is not yet
         * wired (no document handle on the render context). */
        for (lxb_dom_node* nd = lab->node.first_child; nd; nd = nd->next) {
            if (nd->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element* e = lxb_dom_interface_element(nd);
                size_t elen = 0;
                const lxb_char_t* en = lxb_dom_element_local_name(e, &elen);
                if (en && elen == 5 && std::memcmp(en, "input", 5) == 0) {
                    toggle_el = e;
                    break;
                }
            }
        }
    }
    if (toggle_el && is_check_radio(toggle_el)) {
        lxb_dom_element* hit2 = toggle_el;
        if (lxb_dom_element_has_attribute(hit2, (const lxb_char_t*)"checked", 7)) {
            lxb_dom_element_remove_attribute(hit2, (const lxb_char_t*)"checked", 7);
        } else {
            lxb_dom_element_set_attribute(hit2, (const lxb_char_t*)"checked", 7,
                                          (const lxb_char_t*)"", 0);
            size_t nlen = 0;
            const lxb_char_t* nm = lxb_dom_element_get_attribute(
                hit2, (const lxb_char_t*)"name", 4, &nlen);
            size_t tlen = 0;
            const lxb_char_t* tname = lxb_dom_element_get_attribute(
                hit2, (const lxb_char_t*)"type", 4, &tlen);
            bool radio = tname && tlen == 5 &&
                         std::memcmp(tname, "radio", 5) == 0;
            if (radio && nm && nlen > 0) {
                /* clear checked on every other radio with the same name */
                std::function<void(lxb_dom_node*)> uncheck =
                    [&](lxb_dom_node* nd) {
                        while (nd) {
                            if (nd->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                                lxb_dom_element* e = lxb_dom_interface_element(nd);
                                if (e != hit2 &&
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
                lxb_dom_document* doc2 = hit2->node.owner_document;
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
        whaleui_layout_node_t* s = find_node_by_el(r->tree, r->open_select);
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
        /* the popup is a modal overlay: hover styles must NOT fall through
         * to the elements underneath it (an input below the list would
         * switch to an I-beam / apply its :hover). But the cursor is not
         * frozen: it follows the element under the pointer - the select
         * itself stays pointer (its UA cursor), the popup options are
         * default arrow (unless they set their own), and anything else on
         * the page shows its own cursor ("涓嬫媺鐐瑰紑鍚庨紶鏍囧湪鍝兘鏄?pointer"
         * was the select's pointer leaking to the whole page because
         * hover_el stayed pinned to the select). */
        whaleui_layout_node_t* s2 = find_node_by_el(r->tree, r->open_select);
        int lx = 0, ly = 0, lw = 0, lh = 0;
        if (s2) {
            int soff = 0;
            for (whaleui_layout_node_t* p = s2->parent; p; p = p->parent) {
                soff += scroll_delta(r, p);
            }
            std::vector<std::string> texts2, values2;
            select_options(s2->el, texts2, values2);
            lx = s2->border.x;
            ly = s2->border.y + soff + s2->border.h;
            lw = s2->border.w;
            lh = kSelectItemH * static_cast<int>(values2.size());
        }
        bool in_list = x >= lx && x < lx + lw && y >= ly && y < ly + lh;
        whaleui_layout_node_t* hit2 =
            in_list ? nullptr : hit_test(r, r->tree->root, x, y, 0);
        SDL_Cursor* want = render_cursor(r, SDL_SYSTEM_CURSOR_DEFAULT);
        lxb_dom_element* cur_el =
            in_list ? (s2 ? s2->el : nullptr) : (hit2 ? hit2->el : nullptr);
        if (cur_el) {
            std::string c = in_list ? std::string()
                                    : sget(hit2->style, "cursor");
            bool pointer = c == "pointer";
            if (!pointer) {
                size_t tlen = 0;
                const lxb_char_t* tname =
                    lxb_dom_element_local_name(cur_el, &tlen);
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
        if (want) {
            SDL_SetCursor(want);
        }
        return;
    }
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    lxb_dom_element* el = hit ? hit->el : nullptr;
    if (el != r->hover_el) {
        r->hover_old_el = r->hover_el;
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
     * mouse button - never selects. A press inside an existing selection
     * instead readies a drag-to-move/copy: the selection stays put and the
     * drop (move/copy) happens on mouse-up. */
    if (r->pressed_el && r->sel_anchor_el && hit) {
        if (r->drag_sel) {
            int dx = x - r->press_x;
            int dy2 = y - r->press_y;
            if (dx * dx + dy2 * dy2 >= 36) {
                r->drag_sel_active = 1;
            }
            return;
        }
        if (!r->selecting) {
            int dx = x - r->press_x;
            int dy2 = y - r->press_y;
            if (dx * dx + dy2 * dy2 < 36) {
                return;
            }
            r->selecting = 1;
        }
        update_selection_focus(r, hit, x, y);
        /* dragging past the visible edge must scroll the editable box
         * (single-line inputs scroll horizontally, textareas vertically) */r->edit_scroll_need = 1;
        edit_ensure_visible(r);
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
        sc = find_node_by_el(r->tree, r->drag_scroll_el);
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

/* drop the dragged selection at (x, y); defined after set_pressed_ex */
static void drag_drop_selection(whaleui_render_t* r, int x, int y, int copy);

extern "C" void whaleui_render_set_pressed(whaleui_render_t* r, int x, int y,
                                           int down)
{
    whaleui_render_set_pressed_ex(r, x, y, down, 1, 0);
}

extern "C" void whaleui_render_set_pressed_ex(whaleui_render_t* r, int x,
                                              int y, int down, int clicks,
                                              int mods)
{
    if (!r || !r->tree) {
        return;
    }
    fb_coords(r, x, y);
    if (down) {
        /* the expanded list is a modal overlay: a press picks an option
         * (handle_click) or closes the list - it must not press/focus the
         * element underneath the popup */
        if (r->open_select) {
            r->has_dirty = 1;
            return;
        }
        whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
        lxb_dom_element* el = hit ? hit->el : nullptr;
        r->press_x = x;
        r->press_y = y;
        r->selecting = 0;
        r->drag_sel = 0;
        r->drag_sel_active = 0;
        r->drag_copy = 0;
        r->press_clicks = clicks < 1 ? 1 : (clicks > 3 ? 3 : clicks);
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
        /* pressing inside an existing editable selection readies a
         * drag-to-move/copy: the selection stays put while dragging.
         * Only single clicks (clicks == 1): a double/triple click re-selects
         * the word/line under the pointer instead. A click without a drag
         * collapses the selection to the press point on mouse-up. */
        if (hit && is_editable(hit->el) && r->press_clicks == 1 &&
            r->sel_anchor_el == hit->el &&
            r->sel_anchor_el == r->sel_focus_el && r->sel_anchor != r->sel_focus) {
            size_t off = caret_from_point(r, hit->el, hit, x, y);
            int a = r->sel_anchor < r->sel_focus ? r->sel_anchor : r->sel_focus;
            int b = r->sel_anchor < r->sel_focus ? r->sel_focus : r->sel_anchor;
            if (off > static_cast<size_t>(a) && off < static_cast<size_t>(b)) {
                r->drag_sel = 1;
                r->press_caret = static_cast<int>(off);
                r->pressed_el = el;
                r->focus_el = el;
                r->has_dirty = 1;
                return;
            }
        }
        if (el && is_editable(el)) {
            /* focus the editable control and place the caret / word / line */
            r->edit_el = el;
            r->sel_anchor_el = r->sel_focus_el = el;
            r->nav_col = -1;
            std::string val = edit_value(el);
            size_t off = caret_from_point(r, el, hit, x, y);
            if (r->press_clicks >= 3) {
                int ls = static_cast<int>(line_start(val, off));
                int le = static_cast<int>(line_end(val, off));
                r->sel_anchor = ls;
                r->sel_focus = le;
                r->sel_drag_anchor = ls;
                r->sel_drag_focus = le;
            } else if (r->press_clicks == 2) {
                size_t ws = 0, we = 0;
                word_range(val, off, &ws, &we);
                r->sel_anchor = static_cast<int>(ws);
                r->sel_focus = static_cast<int>(we);
                r->sel_drag_anchor = static_cast<int>(ws);
                r->sel_drag_focus = static_cast<int>(we);
            } else {
                r->sel_anchor = r->sel_focus = static_cast<int>(off);
            }
            r->sel_mode = r->press_clicks >= 3 ? 2 : (r->press_clicks == 2 ? 1 : 0);
            /* mouse click (not a drag yet) can move the caret past the
             * visible edge of a horizontally scrolled input: bring it back */r->edit_scroll_need = 1;
            edit_ensure_visible(r);
            SDL_StartTextInput(r->window);
        } else if (hit && hit->is_text) {
            /* anchor a potential selection (only drags extend it) */
            if (r->edit_el) {
                SDL_StopTextInput(r->window);
                r->edit_el = nullptr;
            }
            r->compose.clear();
            r->sel_anchor_el = r->sel_focus_el = hit->el;
            size_t off = byte_at_node(r, hit, x, y);
            if (r->press_clicks >= 3) {
                int ls = static_cast<int>(line_start(hit->text, off));
                int le = static_cast<int>(line_end(hit->text, off));
                r->sel_anchor = ls;
                r->sel_focus = le;
                r->sel_drag_anchor = ls;
                r->sel_drag_focus = le;
            } else if (r->press_clicks == 2) {
                size_t ws = 0, we = 0;
                word_range(hit->text, off, &ws, &we);
                r->sel_anchor = static_cast<int>(ws);
                r->sel_focus = static_cast<int>(we);
                r->sel_drag_anchor = static_cast<int>(ws);
                r->sel_drag_focus = static_cast<int>(we);
            } else {
                r->sel_anchor = r->sel_focus = static_cast<int>(off);
            }
            r->sel_mode = r->press_clicks >= 3 ? 2 : (r->press_clicks == 2 ? 1 : 0);
        } else {
            /* click elsewhere: drop the selection + editing focus */
            if (r->edit_el) {
                SDL_StopTextInput(r->window);
                r->edit_el = nullptr;
            }
            r->compose.clear();
            r->sel_anchor_el = r->sel_focus_el = nullptr;
            r->sel_anchor = r->sel_focus = 0;
            r->sel_mode = 0;
        }
        r->pressed_el = el;
        r->focus_el = el;
    } else {
        /* mouse up: a selection survives only when it was actually dragged
         * (or started as a double/triple click). A plain click (press+
         * release without crossing the threshold, even with incidental
         * micro-motion) leaves nothing selected. The caret of a focused
         * editable control is kept as-is. */
        r->pressed_el = nullptr;
        r->drag_scroll_el = nullptr;
        r->drag_scroll_node = nullptr;
        if (r->drag_sel && !r->drag_sel_active) {
            /* plain click inside the selection: collapse it and move the
             * caret to the press point (drag-to-move needs an actual drag) */
            r->sel_anchor = r->sel_focus = r->press_caret;
            r->sel_mode = 0;
        } else if (r->drag_sel_active) {
            r->drag_copy = (mods & SDL_KMOD_CTRL) != 0;
            drag_drop_selection(r, x, y, r->drag_copy);
        }
        r->drag_sel = 0;
        r->drag_sel_active = 0;
        if (!r->selecting && r->sel_mode == 0 &&
            !(r->edit_el && r->sel_anchor_el == r->edit_el)) {
            r->sel_anchor_el = r->sel_focus_el = nullptr;
            r->sel_anchor = r->sel_focus = 0;
        }
        r->selecting = 0;
    }
    r->has_dirty = 1;
}

/* drop the dragged selection at (x, y): move it (ctrl: copy) into the
 * editable element under the pointer. Only same-element selections inside
 * an editable control are supported as sources. */
static void drag_drop_selection(whaleui_render_t* r, int x, int y, int copy)
{
    lxb_dom_element* src = r->sel_anchor_el;
    if (!src || r->sel_anchor_el != r->sel_focus_el || !is_editable(src) ||
        r->sel_anchor == r->sel_focus || !r->tree) {
        return;
    }
    int a = r->sel_anchor < r->sel_focus ? r->sel_anchor : r->sel_focus;
    int b = r->sel_anchor < r->sel_focus ? r->sel_focus : r->sel_anchor;
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    lxb_dom_element* dst = hit ? hit->el : nullptr;
    if (!dst || !is_editable(dst)) {
        return; /* only editable targets accept drops */
    }
    std::string val = edit_value(src);
    if (static_cast<size_t>(a) >= val.size() || static_cast<size_t>(b) > val.size()) {
        return;
    }
    std::string txt = val.substr(static_cast<size_t>(a),
                                 static_cast<size_t>(b - a));
    if (txt.empty()) {
        return;
    }
    size_t t = caret_from_point(r, dst, hit, x, y);
    if (dst == src) {
        if (t >= static_cast<size_t>(a) && t <= static_cast<size_t>(b)) {
            return; /* dropped on itself */
        }
        if (!copy && t > static_cast<size_t>(b)) {
            t -= static_cast<size_t>(b - a); /* shift after removal */
        }
        if (copy) {
            edit_replace(r, dst, t, t, txt);
        } else {
            edit_replace(r, dst, static_cast<size_t>(a), static_cast<size_t>(b), "");
            edit_replace(r, dst, t, t, txt);
        }
    } else {
        if (!copy) {
            edit_replace(r, src, static_cast<size_t>(a), static_cast<size_t>(b), "");
        }
        edit_replace(r, dst, t, t, txt);
    }
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
            r->scroll_el = sc->el; /* embedded scroll: partial repaint */
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
        /* only boxes that can actually scroll claim the wheel: a box whose
         * estimate says "no overflow" (scroll_max == 0) must NOT swallow
         * the event, or the wheel over a small textarea/input is lost and
         * the page underneath never scrolls. Single-line inputs never
         * scroll vertically - instead, when their value overflows the box
         * horizontally the wheel scrolls the value sideways (like the
         * caret-driven hscroll); with no horizontal overflow they fall
         * through to the page. */
        bool is_input = false;
        if (n->el) {
            size_t ilen = 0;
            const lxb_char_t* iname =
                lxb_dom_element_local_name(n->el, &ilen);
            is_input = ilen == 5 && std::memcmp(iname, "input", 5) == 0;
        }
        if (is_input) {
            std::string val = edit_value(n->el);
            int hmax = 0;
            if (!val.empty()) {
                int fs = 0;
                std::string fam;
                bool bold = false;
                node_font(r, n, &fs, &fam, &bold);
                int tw = 0, th = 0;
                text_size(r, val, fs, fam, bold, &tw, &th, 0);
                hmax = tw - n->content.w;
                if (hmax < 0) {
                    hmax = 0;
                }
            }
            if (hmax > 0) {
                /* value overflows: scroll it horizontally, swallow the
                 * wheel (the page does not move) */
                int& hcur = r->hscrolls[n->el];
                int nv = hcur + delta;
                if (nv > hmax) {
                    nv = hmax;
                }
                if (nv < 0) {
                    nv = 0;
                }
                if (nv != hcur) {
                    hcur = nv;
                    r->scroll_dirty = 1;
                }
                return;
            }
            continue; /* no horizontal overflow: page scrolls */
        }
        if ((ov == "auto" || ov == "scroll") && n->scroll_max > 0) {
            int before = r->scrolls[n->el];
            do_scroll(n);
            /* a container at its edge must NOT swallow the wheel forever:
             * once it cannot move (already at top/bottom), fall through to
             * the next scrollable ancestor so the page keeps scrolling
             * ("榧犳爣鍦?card/textarea 涓婃棤娉曟粴鍔ㄥ埌搴? - the container
             * claimed every wheel event and never bubbled). */
            if (r->scrolls[n->el] != before) {
                return;
            }
            continue;
        }
    }
    /* nothing explicitly scrollable: scroll the page (html root) */
    do_scroll(r->tree->root);
}

/* Page-internal anchor: scroll the document so the element with this id is
 * visible near the top (leaving room for a fixed header). The laid-out y is
 * baked (content shifted up by the live scroll), so the raw position is
 * border.y + current scroll; subtract the header allowance. No-op when the
 * id is missing. */
extern "C" int whaleui_render_scroll_to_id(whaleui_render_t* r,
                                           whaleui_dom_document_t* doc,
                                           const char* id)
{
    if (!r || !r->tree || !doc || !id || !*id) {
        return -1;
    }
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    lxb_dom_element* root =
        hd ? lxb_dom_document_element(&hd->dom_document) : nullptr;
    if (!root) {
        return -1;
    }
    /* find the element with this id (document walk; ids are rare) */
    lxb_dom_element* el = nullptr;
    std::function<lxb_dom_element*(lxb_dom_node*)> find_id =
        [&](lxb_dom_node* node) -> lxb_dom_element* {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element* e = lxb_dom_interface_element(node);
            size_t alen = 0;
            const lxb_char_t* a = lxb_dom_element_get_attribute(
                e, (const lxb_char_t*)"id", 2, &alen);
            if (a && alen == std::strlen(id) &&
                std::memcmp(a, id, alen) == 0) {
                return e;
            }
        }
        for (lxb_dom_node* c = node->first_child; c; c = c->next) {
            lxb_dom_element* found = find_id(c);
            if (found) {
                return found;
            }
        }
        return nullptr;
    };
    el = find_id(&root->node);
    if (!el) {
        return -1;
    }
    whaleui_layout_node_t* n = find_node_by_el(r->tree, el);
    if (!n) {
        return -1;
    }
    lxb_dom_element* root_el = r->tree->root->el;
    auto sit = r->scrolls.find(root_el);
    int cur = sit != r->scrolls.end() ? sit->second : 0;
    int target = n->border.y + cur - 80; /* 80px for the fixed header */
    if (target < 0) {
        target = 0;
    }
    int max = r->tree->root->scroll_max;
    if (target > max) {
        target = max;
    }
    if (target != cur) {
        r->scrolls[root_el] = target;
        r->scroll_dirty = 1;
        r->scroll_el = root_el;
    }
    return 0;
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
        whaleui_layout_node_t* n = find_node_by_el(r->tree, el);
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

extern "C" int whaleui_scroll_smooth_fn(whaleui_render_t* r,
                                        lxb_dom_element* el, int delta,
                                        void* userdata)
{
    (void)userdata;
    if (!el || !r->tree) {
        return 0;
    }
    whaleui_layout_node_t* n = find_node_by_el(r->tree, el);
    if (!n) {
        return 0;
    }
    int max = n->scroll_max > 0 ? n->scroll_max : 0;
    int& tgt = r->scroll_tgt[el];
    int nv = tgt + delta;
    if (nv > max) {
        nv = max;
    }
    if (nv < 0) {
        nv = 0;
    }
    if (nv != tgt) {
        tgt = nv;
        return 1; /* the frame loop eases scrolls toward tgt */
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
    /* smooth scroll easing: move each live position toward its wheel
     * target in small per-frame steps. This is what decouples wheel
     * events from frame rendering - the event only accumulates the target
     * (whaleui_scroll_smooth_fn), the position here glides toward it, so
     * discrete mouse-wheel notches render like touchpad deltas. */
    if (!r->scroll_tgt.empty() && r->tree) {
        for (auto it = r->scroll_tgt.begin(); it != r->scroll_tgt.end();) {
            lxb_dom_element* el = it->first;
            whaleui_layout_node_t* scn = find_node_by_el(r->tree, el);
            if (!scn) {
                it = r->scroll_tgt.erase(it);
                continue;
            }
            int max = scn->scroll_max > 0 ? scn->scroll_max : 0;
            int tgt = it->second;
            if (tgt > max) {
                tgt = max;
            }
            if (tgt < 0) {
                tgt = 0;
            }
            int& cur = r->scrolls[el];
            if (cur > max) {
                cur = max;
            }
            if (cur < 0) {
                cur = 0;
            }
            int diff = tgt - cur;
            if (diff == 0) {
                it = r->scroll_tgt.erase(it);
                continue;
            }
            /* exponential approach: 40% of the gap per frame (~40ms time
             * constant at 60fps) - fast enough to feel immediate, smooth
             * enough to hide the notch quantization */
            int step = static_cast<int>(diff * 0.4f);
            if (step == 0) {
                step = diff > 0 ? 1 : -1;
            }
            cur += step;
            it->second = tgt; /* persist the clamped target */
            r->scroll_dirty = 1;
            r->scroll_el = el; /* embedded containers repaint partially */
            ++it;
        }
    }
    /* consume this document's pending DOM mutations up front: a non-empty
     * set must keep the frame alive so the incremental relayout below runs */
    std::vector<lxb_dom_element*> dom_dirty;
    whaleui_dom_take_dirty(doc, dom_dirty);

    /* async first layout (opt-in): no tree yet -> build on a worker so a
     * large page does not freeze the window while it lays out; the frame
     * loop picks up the finished tree. Only the FIRST layout is async -
     * later full rebuilds stay synchronous. The worker reads the DOM
     * (no user interaction exists before the first paint, so no concurrent
     * DOM write) and an anim=NULL layout (animation state is created by
     * the following synchronous frames). */
    if (r->async_layout && !r->tree && !r->layout_thread && doc) {
        r->layout_done.store(0, std::memory_order_relaxed);
        r->layout_pending = nullptr;
        r->layout_w = r->fb_w;
        r->layout_h = r->fb_h;
        r->layout_text_scale = r->text_scale;
        r->layout_vars = r->theme_vars;
        r->layout_thread = new std::thread([r, doc]() {
            whaleui_style_state st0;
            r->layout_pending = whaleui_layout_compute(
                doc, r->rules, r->rule_count, &r->layout_vars,
                r->layout_w, r->layout_h, &st0, nullptr, nullptr,
                r->layout_text_scale);
            r->layout_done.store(1, std::memory_order_release);
        });
    }
    /* worker finished: take the tree (the worker has exited, join is
     * immediate) and force a full first paint. scroll_dirty keeps the
     * frame alive WITHOUT the relayout pass - the tree is already built. */
    if (r->layout_thread &&
        r->layout_done.load(std::memory_order_acquire)) {
        r->tree = r->layout_pending;
        r->layout_pending = nullptr;
        r->layout_thread->join();
        delete r->layout_thread;
        r->layout_thread = nullptr;
        r->layout_done.store(0, std::memory_order_relaxed);
        if (r->tree) {
            g_last_tree = r->tree;
            r->bounds_valid = 0;
            r->scroll_dirty = 1;
            fix_run_heights(r);  /* real glyph heights + ranges */
            fix_scroll_max(r);
        }
    }
    if (r->layout_thread) {
        /* worker still running: keep the event loop responsive; there is
         * no tree to paint yet, the window keeps its clear color */
        r->alive = 1; /* stay in the frame loop until the layout lands */
        return 0;
    }
    /* skip the whole frame when nothing changed: idle frames cost ~0.
     * Repaint when the layout/state is dirty, a wheel scroll happened, an
     * animation/transition is running, an editable caret is blinking, or
     * the DOM was mutated. */
    if (!r->has_dirty && r->tree && !r->scroll_dirty &&
        !animating && !r->edit_el && dom_dirty.empty()) {
        r->alive = 0; /* static page: the app loop can park idle */
        return 0;
    }
    r->scroll_dirty = 0;
#ifdef WHALEUI_BUILD_FULL
    /* the layout pass measures real glyph widths through this context */
    g_metric_render = r;
#endif
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
    /* layout rebuild needed when the document is dirty (or no tree yet).
     * Layout-affecting animations rebuild only the animated elements'
     * subtrees (incremental relayout below), NOT the whole tree - a width
     * animation previously rebuilt every box every frame. Paint-only
     * animations (opacity/transform/colors) skip layout entirely: the
     * tick's values are applied straight onto the tree and only the
     * opacity chain is recomputed - the bulk of the per-frame cost
     * (style cascade + box layout) is gone. */
    const bool need_layout = r->has_dirty || !r->tree;
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
        /* NOTE: no clamp here. The layout's scroll_max is an estimate; a
         * clamp against it snaps the live scroll to 0 before fix_sm below
         * corrects the range (a dragged thumb "jumps back" and the bottom
         * bounces). fix_sm clamps against the corrected values instead. */
        /* the layout estimated wrapped lines; the per-glyph render layout
         * can differ. Correct every text run's height to text_size and
         * recompute scroll ranges, so the caret, selection, scrolling and
         * the painted glyphs all agree. */
        fix_run_heights(r);
        /* corrected scroll ranges + grown auto-height boxes (see
         * fix_scroll_max above; shared with the animation relayout path) */
        fix_scroll_max(r);        /* after a DOM edit the layout tree is fresh here: re-run the
         * caret-visible scroll so a caret just typed past the visible
         * area (or on a wrapped line) scrolls the box. edit_replace's
         * earlier call ran against the stale tree (scroll_max was 0),
         * so this is the pass that actually scrolls textareas. Only an
         * EDIT sets edit_scroll_need: a user wheel/drag must never be
         * yanked back to the caret line (the "caret locks the scroll"). */
        if (r->edit_el && r->edit_scroll_need) {
            edit_ensure_visible(r);
            r->edit_scroll_need = 0;
        }
#ifdef WHALEUI_BUILD_FULL
        g_metric_render = nullptr; /* layout done; paint re-rasterizes */
#endif
    } else if (animating && whaleui_anim_needs_layout(r->anim)) {
        /* layout-affecting animation (width/margin/...): rebuild only the
         * animated elements' subtrees with the tick's values, re-position
         * the rest (box pass). A width bar previously rebuilt the WHOLE
         * tree every frame - the demo's bar animation is what made any
         * concurrent scrolling/animating feel like a crawl. relayout's
         * build applies the animated styles itself, so the apply_ov pass
         * below is skipped on these frames. */
        whaleui_style_state st;
        st.hover = r->hover_el;
        st.focus = r->focus_el;
        st.pressed = r->pressed_el;
        std::vector<lxb_dom_element*> anim_els;
        std::function<void(whaleui_layout_node_t*)> collect =
            [&](whaleui_layout_node_t* nd) {
                if (nd->el && whaleui_anim_has_el(r->anim, nd->el)) {
                    anim_els.push_back(nd->el);
                }
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    collect(c);
                }
            };
        collect(r->tree->root);
        bool ok = false;
        int m_ok = whaleui_layout_relayout_multi(
            r->tree, anim_els.data(), anim_els.size(), r->rules,
            r->rule_count, &r->theme_vars, &st, &r->scrolls, r->anim,
            r->text_scale);
        if (m_ok == 0) {
            ok = true;
        } else if (m_ok < 0) {
            ok = false; /* builder failed; full rebuild below */
        } else {
            ok = true;  /* every element already mapped to nothing: nothing
                         * changed, stay on the current tree */
        }
        if (ok) {
            g_last_tree = r->tree;
            r->bounds_valid = 0; /* subtree paint bounds are stale */
            r->drag_scroll_node = nullptr; /* nodes were recreated */
            r->scroll_max_el = nullptr;    /* scroll_max may have changed */
            r->wheel_node = nullptr;       /* hit cache is stale */
            /* ponytail: skip the full-tree fix_scroll_max walk on layout-
             * animation frames. A width/height animation inside a scroll
             * container is rare, and the range corrects on the next full
             * relayout (scroll/hover/DOM/edit); the walk is per-frame
             * baseline cost otherwise. */
        } else {
            r->has_dirty = 1; /* tree inconsistent: full rebuild */
        }
#ifdef WHALEUI_BUILD_FULL
        g_metric_render = nullptr; /* layout done; paint re-rasterizes */
#endif
    } else if (animating) {
        /* paint-only animation: apply the tick's values to the animated
         * elements' styles and refresh the cascaded opacity ONLY along
         * their subtrees (opacity inheritance cannot leave them). The old
         * version walked the whole tree every frame doing a style lookup
         * per node - the fixed per-frame cost that made even a tiny pulse
         * animation cost ms at 2k. */
        std::function<void(whaleui_layout_node_t*, float)> apply_sub =
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
                    apply_sub(c, nd->opacity);
                }
            };
        std::function<void(whaleui_layout_node_t*)> find_anim =
            [&](whaleui_layout_node_t* nd) {
                /* text runs share their element's animatable style but
                 * must not have the animation values applied to their own
                 * style (a translateY would offset the glyphs inside the
                 * already-transformed box - "ghost text" regression). Their
                 * opacity is handled by apply_sub above. */
                if (nd->el && !nd->is_text &&
                    whaleui_anim_has_el(r->anim, nd->el)) {
                    whaleui_anim_apply_ov(r->anim, nd->el, nd->style);
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
                    nd->opacity = o;
                    for (whaleui_layout_node_t* c = nd->first_child; c;
                         c = c->next) {
                        apply_sub(c, nd->opacity);
                    }
                }
                /* an ancestor being animated must not stop the walk: the
                 * children may carry their own animated values (e.g. a
                 * hovered cell inside a section whose reveal transition is
                 * still running). Without this the child's apply_ov never
                 * runs and its transition stays pinned at the start value
                 * ("hover changes and snaps straight back"). */
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    find_anim(c);
                }
            };
        find_anim(r->tree->root);
    }
    /* DOM mutations: incremental relayout. Only the affected subtrees get
     * a fresh style cascade + build; the box pass re-positions the tree
     * while untouched branches keep their computed styles. Falls back to a
     * full rebuild (has_dirty) when the tree is inconsistent. */
    if (!need_layout && !dom_dirty.empty()) {
        /* DOM mutated: CSS custom properties may have changed, so the
         * cached var collection is stale - force a re-collect */
        if (r->tree) {
            r->tree->vars.clear();
            r->tree->vars_collected = false;
            r->tree->style_cache.clear();
        }
        bool ok = true;
        whaleui_style_state st;
        st.hover = r->hover_el;
        st.focus = r->focus_el;
        st.pressed = r->pressed_el;
        for (lxb_dom_element* el : dom_dirty) {
            if (whaleui_layout_relayout(r->tree, el, r->rules,
                                        r->rule_count, &r->theme_vars,
                                        &st, &r->scrolls, r->anim,
                                        r->text_scale) < 0) {
                ok = false;
                break;
            }
        }
        if (ok) {
            g_last_tree = r->tree;
            r->bounds_valid = 0; /* subtree paint bounds are stale */
            r->drag_scroll_node = nullptr; /* nodes were recreated */
            r->scroll_max_el = nullptr;    /* scroll_max may have changed */
            r->wheel_node = nullptr;       /* hit cache is stale */
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
#ifdef WHALEUI_BUILD_FULL
            g_metric_render = nullptr; /* layout done; paint re-rasterizes */
#endif
        } else {
            r->has_dirty = 1; /* tree inconsistent: full rebuild */
        }
    }
    if (!r->tree) {
        return -2;
    }

    /* pure scroll: shift the previous image (ping-pong blit on the GPU
     * side + memmove of the text layer) and only repaint the exposed strip.
     * Any other dirty/animations fall back to a full repaint (dy=0).
     *
     * VIEWPORT MODEL: the page is laid out once and the viewport shows a
     * window into it. A scroll only changes the viewport offset - it does
     * NOT shift (reuse) the previous frame's pixels. Reusing the old frame
     * plus repainting the exposed strip is what left stale pixels behind
     * (the smeared fixed header, the giant overlay glyphs, the select
     * dropdown ghost) whenever the shift and the per-element paint offsets
     * disagreed. Repainting the whole viewport at the new scroll offset is
     * the correct, artifact-free model, so scroll_dy is pinned to 0 and
     * every scroll frame repaints the visible region. */
    int scroll_dy = 0;

    /* paint: collect batched GPU draw commands. Text goes to the CPU layer
     * (moved on scroll), uploaded + composited by a compute pass. */
    Clip full = {0, 0, r->fb_w, r->fb_h};
    /* empty strip: dirty_rect GROWS it, so partial branches (hover /
     * animation / embedded scroll) shrink the repaint to their boxes. A
     * strip starting at `full` never shrinks - dirty_rect only unions -
     * and every "partial" frame repainted the whole window (the animation
     * frame-drop report). */
    Clip strip = {0, 0, 0, 0};
    if (scroll_dy != 0) {
        /* the scroll strip spans the full width */
        strip.x = 0;
        strip.w = full.w;
        /* shift the existing text layer rows FIRST (opposite of the
         * scroll): the shift copies from [dy, fb_h) into [0, fb_h-dy), so
         * the old bottom-strip content lands above the strip. Clearing the
         * strip before shifting would wipe exactly those rows. */
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
    /* scroll + paint-only animation: an animating element inside the
     * viewport but OUTSIDE the strip moves (translate), and the strip-only
     * repaint leaves its PREVIOUS position as a ghost (same root cause as
     * the animation partial branch, applied to the scroll strip). Widen the
     * strip to cover every visible animating element. */
    if (scroll_dy != 0 && animating && !whaleui_anim_needs_layout(r->anim)) {
        const int AM = 40; /* ponytail: anim-travel margin (see above) */
        std::function<void(whaleui_layout_node_t*)> acca =
            [&](whaleui_layout_node_t* nd) {
                if (nd->el && !nd->is_text &&
                    whaleui_anim_has_el(r->anim, nd->el)) {
                    int oy = node_scroll_off(r, nd);
                    int x0 = nd->border.x - AM;
                    int y0 = nd->border.y + oy - AM;
                    int x1 = x0 + nd->border.w + AM + AM;
                    int y1 = y0 + nd->border.h + AM + AM;
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
                    acca(c);
                }
            };
        acca(r->tree->root);
    }
    bool partial = false; /* load-only repaint of a dirty region */
    /* partial strips (hover / animation) are computed from node bounds -
     * compute them BEFORE the strip math. After a relayout bounds_valid
     * is 0 and every bound reads (0,0,0,0), so the hover strip came out
     * empty and a hover change repainted nothing (background-color :hover
     * "涓嶅搷搴?; the relayout had applied the style, the paint just never
     * covered the box). */
    if (!r->bounds_valid) {
        compute_paint_bounds(r->tree->root);
        r->bounds_valid = 1;
    }
    /* a wheel scroll happened (scroll_el set by the scroll behavior) or a
     * scrollbar is being dragged: the WHOLE viewport repaints at the new
     * scroll offset. Consume it so it only forces one frame; embedded
     * scroll boxes repaint too (their content shifts, drawn at the new
     * offset during the full paint). The drag path also clears a stale
     * hover_old_el: set_hover returns early while dragging, so a hover
     * change right before the drag leaves hover_old_el set - the partial
     * branch below would then repaint ONLY the two hover boxes and the
     * scrolled content stayed frozen until mouse-up ("half the page does
     * not move while dragging the scrollbar"). */
    if (r->scroll_el || r->drag_scroll_el) {
        r->scroll_el = nullptr;
        r->hover_old_el = nullptr;
    } else if (!animating && !r->edit_el && !r->open_select &&
               r->hover_old_el) {
        /* hover change: repaint only the previous and current hover
         * targets (their :hover style changed). The relayout already
         * re-cascaded the styles; the geometry is stable. */
        std::function<void(whaleui_layout_node_t*)> acc =
            [&](whaleui_layout_node_t* nd) {
                if (nd->el &&
                    (nd->el == r->hover_old_el ||
                     nd->el == r->hover_el)) {
                    /* visual position: the layout bounds are pre-scroll;
                     * the paint pass draws at bounds + ancestor scroll */
                    int off = 0;
                    for (whaleui_layout_node_t* p = nd->parent; p;
                         p = p->parent) {
                        off += scroll_delta(r, p);
                    }
                    int x0 = nd->bounds.x - 2;
                    int y0 = nd->bounds.y + off - 2;
                    int x1 = x0 + nd->bounds.w + 4;
                    int y1 = y0 + nd->bounds.h + 4;
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
        r->hover_old_el = nullptr;
    } else if (animating && !need_layout && !r->has_dirty && !r->edit_el &&
               !r->open_select) {
        /* animation: repaint only the animating elements' bounding boxes
         * (dirty-rect, keeps the rest of the frame). Covers BOTH paint-only
         * animations (opacity/transform) and layout animations (width/
         * height): the relayout pass only rebuilds the animated subtrees,
         * so the damage is limited to the animated element (plus its
         * parent box, whose interior may re-flow) - a full repaint per
         * width-animation frame is what dropped the demo to 24fps at 2k.
         * An open <select> dropdown is drawn OUTSIDE the tree (last, full
         * viewport); a dirty-rect frame covers only the animating boxes, so
         * the dropdown repaints over stale pixels and jitters - repaint
         * fully while a dropdown is open. */
        const bool lay_anim = whaleui_anim_needs_layout(r->anim);
        /* anim-travel margin: per-element, derived from the actual
         * transform - a width/opacity animation (no transform) needs only
         * a couple of pixels of anti-alias room, while a translate needs
         * its travel distance (capped). A fixed 40px margin on every
         * animated element inflated the 2k strip (and the text-layer
         * upload) for bar/opacity animations that never move. */
        const int AM = 40;
        std::function<void(whaleui_layout_node_t*)> acc =
            [&](whaleui_layout_node_t* nd) {
                if (nd->el && whaleui_anim_has_el(r->anim, nd->el)) {
                    int x0 = nd->border.x, y0 = nd->border.y;
                    int x1 = x0 + nd->border.w, y1 = y0 + nd->border.h;
                    int am = 4;
                    /* widen by the transform translation */
                    std::string tv = sget(nd->style, "transform");
                    if (!tv.empty() && tv != "none") {
                        whaleui_transform_t tf;
                        if (whaleui_transform_eval(
                                tv.c_str(), static_cast<float>(nd->border.w),
                                static_cast<float>(nd->border.h), &tf) == 0) {
                            int dtx = static_cast<int>(tf.tx > 0 ? tf.tx
                                                                 : -tf.tx);
                            int dty = static_cast<int>(tf.ty > 0 ? tf.ty
                                                                 : -tf.ty);
                            x0 -= dtx;
                            y0 -= dty;
                            x1 += dtx;
                            y1 += dty;
                            int sm = static_cast<int>(
                                (tf.sx > 1.0f ? tf.sx - 1.0f : 0.0f) *
                                    static_cast<float>(nd->border.w) +
                                (tf.sy > 1.0f ? tf.sy - 1.0f : 0.0f) *
                                    static_cast<float>(nd->border.h) +
                                4.0f);
                            if (sm > am) {
                                am = sm;
                            }
                            if (dtx > am) {
                                am = dtx;
                            }
                            if (dty > am) {
                                am = dty;
                            }
                        }
                    }
                    if (lay_anim && nd->parent && !nd->parent->is_text) {
                        /* layout animation: the parent's interior re-flows
                         * (children move within it), cover the whole box */
                        whaleui_layout_node_t* p = nd->parent;
                        x0 = p->border.x < x0 ? p->border.x : x0;
                        y0 = p->border.y < y0 ? p->border.y : y0;
                        int px1 = p->border.x + p->border.w;
                        int py1 = p->border.y + p->border.h;
                        x1 = px1 > x1 ? px1 : x1;
                        y1 = py1 > y1 ? py1 : y1;
                    }
                    if (am > AM) {
                        am = AM;
                    }
                    x0 -= am;
                    y0 -= am;
                    x1 += am;
                    y1 += am;
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
        /* a partial repaint leaves the scrollbar column stale whenever
         * the strip touches it - the thumb can straddle the strip edge,
         * and a relayout right before this frame (hover) can have moved
         * the thumb so the OLD half-cleaned thumb shows through the new
         * track. Repaint every visible scrollbar column with the strip
         * (cheap: 8px x the box height), so the column always carries
         * freshly-painted content + track + thumb. */
        std::function<void(whaleui_layout_node_t*)> add_bars =
            [&](whaleui_layout_node_t* nd) {
                if (nd->visible && nd->el && !nd->is_text &&
                    nd != r->tree->root && nd->scroll_max > 0 &&
                    nd->border.h >= 24) {
                    int oy = node_scroll_off(r, nd);
                    int bx0 = nd->border.x + nd->border.w - 8;
                    int by0 = nd->border.y + oy;
                    int bx1 = bx0 + 8;
                    int by1 = by0 + nd->border.h;
                    /* only columns the strip already touches: a scrollbar
                     * far from the animated region must not widen the strip
                     * to full-screen (that negated the dirty-rect work) */
                    if (bx1 > strip.x && bx0 < strip.x + strip.w &&
                        by1 > strip.y && by0 < strip.y + strip.h) {
                        dirty_rect(bx0 < 0 ? 0 : bx0, by0 < 0 ? 0 : by0,
                                   bx1 > r->fb_w ? r->fb_w : bx1,
                                   by1 > r->fb_h ? r->fb_h : by1,
                                   &strip);
                    }
                }
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    add_bars(c);
                }
            };
        add_bars(r->tree->root);
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
    /* backdrop-filter runs on every paint (including animation/hover
     * partial frames): a fixed translucent header must stay blurred while
     * content animates under it - gating it on full frames only left the
     * header sharp/unblurred for the whole duration of a reveal
     * animation ("topbar blur not applied"). */
    g_backdrop_active = (scroll_dy == 0) ? 1 : 0;
    int sel_lo = 0, sel_hi = 0;
    const Clip* paint_clip = (partial || scroll_dy != 0) ? &strip : &full;
    /* load-only frames (scroll strip / hover / animation, GPU LOADOP_LOAD)
     * keep stale pixels in areas nothing repaints: the html/body background
     * is usually transparent, so a translated animation leaves the OLD
     * box behind at its previous position. Paint the strip's base color
     * first - every layer (bg, gradients, box shadows, element bodies)
     * draws over it, and transparent areas show the clean window color
     * instead of a ghost. */
    if (partial || scroll_dy != 0) {
        int c[4] = {strip.x, strip.y, strip.w, strip.h};
        whaleui_gpu_rect(g_gpu, static_cast<float>(strip.x),
                         static_cast<float>(strip.y),
                         static_cast<float>(strip.w),
                         static_cast<float>(strip.h), 0.0f, r->bg_color, c);
    }
    sel_seq(r, &sel_lo, &sel_hi, paint_clip);
    int seq = 0;
    paint_node(r, r->tree->root, 0, 0, seq, sel_lo, sel_hi, paint_clip,
               false);

    /* scroll-shift frames: repaint every visible scrollbar column. The
     * image shift moves the OLD thumb with the content; a strip-only
     * repaint only redraws the thumb when it sits inside the strip, so a
     * thumb above the strip keeps the shifted ghost ("scrollbar rendering
     * wrong" after restoring scroll-shift). The 8px column is cheap. */
    if (scroll_dy != 0) {
        std::function<void(whaleui_layout_node_t*, int)> repaint_bars =
            [&](whaleui_layout_node_t* nd, int oy) {
                if (!nd->visible) {
                    return;
                }
                if (nd->el && !nd->is_text && nd->scroll_max > 0) {
                    int by = nd->border.y + oy;
                    if (by < r->fb_h && by + nd->border.h > 0) {
                        paint_scrollbar(r, nd, 0, oy, &full);
                    }
                }
                int noy = oy + scroll_delta(r, nd);
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    repaint_bars(c, noy);
                }
            };
        repaint_bars(r->tree->root, 0);
    }

    /* expanded select list is drawn last (highest z) so later siblings and
     * other content cannot cover it; its position follows the select's
     * scroll-offset ancestors. Like every z-raised layer it clears the old
     * text under its area first, otherwise lower elements' glyphs show
     * through the popup ("涓嬫媺閫夐」琚笅灞傛枃瀛楃┛閫?). */
    if (r->open_select) {
        whaleui_layout_node_t* s = find_node_by_el(r->tree, r->open_select);
        if (s) {
            int soff = 0;
            for (whaleui_layout_node_t* p = s->parent; p; p = p->parent) {
                soff += scroll_delta(r, p);
            }
            std::vector<std::string> texts3, values3;
            select_options(s->el, texts3, values3);
            text_layer_clear(r, s->border.x,
                             s->border.y + soff + s->border.h, s->border.w,
                             kSelectItemH * static_cast<int>(values3.size()));
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
    /* WaitAndAcquire blocks until the GPU finishes the previous frame and
     * the swapchain image is available, so the worker is throttled by the
     * display (vblank) instead of spinning - on the D3D12 backend the
     * VSYNC present mode alone did not throttle (an animation loop ran at
     * ~130fps, ~90% of a core), so this is what keeps an uncapped AC-power
     * animation at the refresh rate. Falls through on failure like the
     * non-blocking acquire. */
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, r->window, &swapchain,
                                               &sw, &sh) ||
        !swapchain) {
        SDL_SubmitGPUCommandBuffer(cmd);
        r->alive = (animating || r->edit_el) ? 1 : 0;
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
    /* keep the frame loop alive while an animation runs or an editable
     * caret blinks; a static page goes idle (the app loop parks). */
    r->alive = (animating || r->edit_el) ? 1 : 0;
    return 0;
}















































