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
#include "dom/events.h"
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

/* properties whose change means geometry moved (a relayout must re-run the
 * box pass / repaint widely); everything else is paint-only. Shared by the
 * interaction-state path (:hover/:focus/:active) and the DOM-dirty path
 * (attribute/style edits) - one layout-key rule for all mutations. */
/* HTML5 drag & drop (defined further down; set_hover/set_pressed_ex use
 * these). The session starts once the pointer passes kDndThreshold on a
 * draggable press candidate. */
static const int kDndThreshold = 6;
static void dnd_begin(whaleui_render_t* r, struct lxb_dom_element* src, int x, int y);
static void dnd_update(whaleui_render_t* r, int x, int y);
static void dnd_finish(whaleui_render_t* r, int x, int y, int cancelled);
static bool is_draggable_el(struct lxb_dom_element* el);

/* Properties whose change invalidates GEOMETRY. An edit that touches one of
 * these must re-run the box pass; anything else is paint-only and keeps the
 * old geometry.
 *
 * This list is the gate for the incremental DOM path: a missing property
 * makes a real layout change take the "geometry unchanged, repaint only"
 * branch, so the element keeps its OLD box and the page is stuck at the
 * wrong position until something else (a hover) forces a full relayout -
 * exactly the reported "after a C++ style change the UI sits at the wrong
 * spot, moving the mouse fixes it". Keep it in sync with the properties
 * layout.cpp actually reads. */
static const char* const kLayoutKeys[] = {
    "display", "position", "width", "height", "min-width", "min-height",
    "max-width", "max-height", "margin", "margin-top", "margin-right",
    "margin-bottom", "margin-left", "padding", "padding-top",
    "padding-right", "padding-bottom", "padding-left", "border-width",
    "border-top-width", "border-right-width", "border-bottom-width",
    "border-left-width", "border-top", "border-right", "border-bottom",
    "border-left", "border", "inset", "top", "right", "bottom", "left",
    "box-sizing", "float", "clear", "flex", "flex-grow", "flex-shrink",
    "flex-basis", "flex-direction", "flex-wrap", "gap", "row-gap",
    "column-gap", "order", "font-size", "font-family", "font-weight",
    "font-style", "line-height", "letter-spacing", "word-spacing",
    "white-space", "overflow", "overflow-x", "overflow-y", "text-align",
    "text-transform", "vertical-align", "word-break", "aspect-ratio",
    /* flex/grid alignment changes move items inside their container */
    "align-items", "align-self", "align-content", "justify-content",
    "justify-items", "justify-self", "place-items", "place-content",
    "grid-template-columns", "grid-template-rows", "grid-auto-flow",
    "grid-auto-columns", "grid-auto-rows", "grid-column", "grid-row",
    "grid-column-start", "grid-column-end", "grid-row-start", "grid-row-end",
    /* a pseudo-element's content changes the boxes it generates */
    "content", "list-style-type",
};

/* Same box KIND for layout, even when the strings differ. layout.cpp's build
 * pass normalizes several inline boxes to inline-block (a <select> gets
 * inline-block, a contenteditable span gets `width:12em; display:inline-
 * block`), while a re-cascade returns the stylesheet's original value. The
 * state/DOM relayout diff compares the new cascade against the NODE's style,
 * so "inline-block" vs "inline" read as a layout change on EVERY hover: the
 * element was pushed to the full relayout + whole-tree box pass each time
 * (slow, and it re-measured every run on the page). layout.cpp display_kind()
 * maps each pair below to one kind - compare on that. */
static bool same_layout_display(const std::string& a, const std::string& b)
{
    if (a == b) {
        return true;
    }
    struct Pair { const char* x; const char* y; };
    static const Pair kSame[] = {
        {"inline", "inline-block"},
        {"flex", "inline-flex"},
        {"grid", "inline-grid"},
        {"table", "inline-table"},
    };
    for (size_t i = 0; i < sizeof(kSame) / sizeof(kSame[0]); ++i) {
        if ((a == kSame[i].x && b == kSame[i].y) ||
            (a == kSame[i].y && b == kSame[i].x)) {
            return true;
        }
    }
    return false;
}

/* one layout-key comparison shared by both relayout diffs */
static bool layout_key_differs(const char* k, const std::string& old_v,
                               const std::string& new_v)
{
    if (std::strcmp(k, "display") == 0 &&
        same_layout_display(old_v, new_v)) {
        return false;
    }
    return old_v != new_v;
}

/* union ONE element's painted box into a dirty strip: bounds + ancestor
 * scroll offsets +-2px, widened along transformed ancestors (same math the
 * interaction-state damage walk applies per node). Used by the hover
 * re-check to repaint the old + new hover targets of the frame. */
whaleui_layout_node_t* find_node_by_el(whaleui_layout_tree_t* tree,
                                       lxb_dom_element* el);
static void strip_element(whaleui_render_t* r, lxb_dom_element* el,
                          Clip* strip)
{
    if (!el || !r->tree || !strip) {
        return;
    }
    whaleui_layout_node_t* nd = find_node_by_el(r->tree, el);
    if (!nd) {
        return;
    }
    int off = 0;
    for (whaleui_layout_node_t* p = nd->parent; p; p = p->parent) {
        off += scroll_delta(r, p);
    }
    int x0 = nd->bounds.x - 2;
    int y0 = nd->bounds.y + off - 2;
    int x1 = x0 + nd->bounds.w + 4;
    int y1 = y0 + nd->bounds.h + 4;
    for (whaleui_layout_node_t* a = nd; a; a = a->parent) {
        if (!a->xf) {
            continue;
        }
        whaleui_transform_t tf;
        if (whaleui_transform_eval(sget(a->style, "transform").c_str(),
                                   static_cast<float>(a->border.w),
                                   static_cast<float>(a->border.h),
                                   &tf) != 0) {
            continue;
        }
        const int ddx = static_cast<int>(tf.tx);
        const int ddy = static_cast<int>(tf.ty);
        if (x0 + ddx < x0) x0 += ddx;
        if (y0 + ddy < y0) y0 += ddy;
        if (x1 + ddx > x1) x1 += ddx;
        if (y1 + ddy > y1) y1 += ddy;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > r->fb_w) x1 = r->fb_w;
    if (y1 > r->fb_h) y1 = r->fb_h;
    if (x1 > x0 && y1 > y0) {
        dirty_rect(x0, y0, x1, y1, strip);
    }
}


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
    /* (buffer, size, text version) instead of hashing the bytes: the box pass
     * and fix_run_heights re-measure the same runs every frame, and hashing a
     * 292k-char run was ~0.5ms of that. Any edit bumps the version (see
     * r->text_ver), so the entry cannot go stale. */
    h ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(utf8));
    h *= 1099511628211ULL;
    h ^= static_cast<uint64_t>(len);
    h *= 1099511628211ULL;
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
            /* CJK/emoji: resolve through the paint path's fallback chain
             * (text_glyph_adv) - measuring the PRIMARY font directly
             * returns .notdef advances (a label with "å·²é€? measured 20px
             * vs the painted 29px and wrapped its text). */
            adv = text_glyph_adv(r, fs, family ? family : "",
                                 bold ? true : false, cp);
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

/* input[type=checkbox/radio]: native control drawn + toggled by the engine.
 * Single writer of the check/radio question (layout's whaleui_input_kind
 * answers the same via tag_id + type): keep both in sync. */
bool is_check_radio(lxb_dom_element* el)
{
    if (!tag_eq(el, "input")) {
        return false;
    }
    return whaleui_input_kind(el) == 2;
}

/* radio type test (single writer): kind==2 narrows to check/radio, this
 * picks the radio half. The toggle + peer walk ask this, never a hand
 * type-attribute compare. */
bool is_radio_type(lxb_dom_element* el)
{
    size_t tlen = 0;
    const lxb_char_t* tv = lxb_dom_element_get_attribute(
        el, (const lxb_char_t*)"type", 4, &tlen);
    return tv && tlen == 5 && std::memcmp(tv, "radio", 5) == 0;
}

/* Single-line text input: <input> whose kind is text-like (0), password
 * (3) or number (4). textarea and contenteditable wrap, so they are
 * multi-line and return false. This is the only place that interprets
 * the kind code for geometry; every caret / wrap / scroll / selection /
 * click / wheel branch asks this function. */
bool is_single_line_input(const whaleui_layout_node_t* n)
{
    if (!n || n->is_text || n->tag_id != WUI_TAG_INPUT) {
        return false;
    }
    int k = whaleui_input_kind(n->el);
    return k == 0 || k == 3 || k == 4;
}

bool is_single_line_input_el(lxb_dom_element* el)
{
    if (!tag_eq(el, "input")) {
        return false;
    }
    int k = whaleui_input_kind(el);
    return k == 0 || k == 3 || k == 4;
}

/* <input type=password>: value shows as mask chars, caret/selection still
 * use the plaintext. Single writer of the password question (reads the
 * kind code, never the attribute directly). */
bool is_password_box_el(lxb_dom_element* el)
{
    return tag_eq(el, "input") && whaleui_input_kind(el) == 3;
}

/* <input type=number>: only digits + number-syntax chars pass the insert
 * filter, and the whole value keeps at most one dot. Single writer (reads
 * the kind code, never the attribute directly). */
bool is_number_box_el(lxb_dom_element* el)
{
    return tag_eq(el, "input") && whaleui_input_kind(el) == 4;
}

/* radio-group peer walk, rooted at the picked element's owner document:
 * clears `checked` on same-name radios when one is picked (browser
 * exclusivity). Single writer of the peer question (the click path asks
 * this, never a hand recursive walk). */
void radio_uncheck_peers(lxb_dom_element* picked)
{
    size_t nlen = 0;
    const lxb_char_t* nm = lxb_dom_element_get_attribute(
        picked, (const lxb_char_t*)"name", 4, &nlen);
    if (!nm || nlen == 0) {
        return;
    }
    lxb_dom_document* doc2 = picked->node.owner_document;
    lxb_dom_element* docroot =
        doc2 ? lxb_dom_document_element(doc2) : nullptr;
    if (!docroot) {
        return;
    }
    /* iterative stack walk (no recursion: deep pages blew the C++ stack
     * through the old recursive lambda) */
    std::vector<lxb_dom_node*> stack;
    stack.push_back(&docroot->node);
    while (!stack.empty()) {
        lxb_dom_node* nd = stack.back();
        stack.pop_back();
        while (nd) {
            if (nd->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element* e = lxb_dom_interface_element(nd);
                if (e != picked &&
                    lxb_dom_element_has_attribute(
                        e, (const lxb_char_t*)"checked", 7) &&
                    tag_eq(e, "input") &&
                    whaleui_input_kind(e) ==
                        2 /* radio (checkbox never reaches here) */) {
                    size_t nn = 0;
                    const lxb_char_t* en = lxb_dom_element_get_attribute(
                        e, (const lxb_char_t*)"name", 4, &nn);
                    if (en && nn == nlen && std::memcmp(en, nm, nlen) == 0) {
                        size_t tl = 0;
                        const lxb_char_t* tv = lxb_dom_element_get_attribute(
                            e, (const lxb_char_t*)"type", 4, &tl);
                        if (tv && tl == 5 && std::memcmp(tv, "radio", 5) == 0) {
                            lxb_dom_element_remove_attribute(
                                e, (const lxb_char_t*)"checked", 7);
                            whaleui_dom_mark_dirty(e);
                        }
                    }
                }
                if (nd->first_child) {
                    stack.push_back(nd->first_child);
                }
            }
            nd = nd->next;
        }
    }
}

/* contenteditable test (single writer): non-false contenteditable, and
 * never an input/textarea (those are editable by tag+kind, not by the
 * attribute - an input contenteditable=false stays a single-line field). */
bool is_contenteditable(lxb_dom_element* el)
{
    if (!el || tag_eq(el, "input") || tag_eq(el, "textarea")) {
        return false;
    }
    size_t alen = 0;
    const lxb_char_t* ce = lxb_dom_element_get_attribute(
        el, (const lxb_char_t*)"contenteditable", 15, &alen);
    return ce && alen > 0 &&
           !(alen == 5 && std::memcmp(ce, "false", 5) == 0);
}

bool is_editable(lxb_dom_element* el)
{
    if (!el) {
        return false;
    }
    if (tag_eq(el, "input")) {
        int k = whaleui_input_kind(el);
        return k == 0 || k == 3 || k == 4;
    }
    if (tag_eq(el, "textarea")) {
        return true;
    }
    return is_contenteditable(el);
}

/* editable check via the layout node's tag component (O(1), skips the
 * local_name call for every painted node) */
bool is_editable_node(whaleui_layout_node_t* n)
{
    if (!n || !n->el) {
        return false;
    }
    if (n->tag_id == WUI_TAG_INPUT) {
        int k = whaleui_input_kind(n->el);
        return k == 0 || k == 3 || k == 4;
    }
    if (n->tag_id == WUI_TAG_TEXTAREA) {
        return true;
    }
    return is_contenteditable(n->el);
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
                /* reserve the CSS line height the layout pass used, not the
                 * font's own height: fix_run_heights overwrote a
                 * `line-height: 40px` run's box with the ~22px font height,
                 * so the text overflowed its box and the following content
                 * moved up into it. */
                int lh = text_line_h_css(r, fs, fam, bold,
                                         sget(nd->style, "line-height"));
                text_size(r, nd->text, fs, fam, bold, &tw, &th,
                          run_wrap_w(nd), lh);
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
    r->scroll_boxes.clear();
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
        /* A plain `display:inline` box's vertical padding/border does NOT
         * grow its ancestors (CSS: it overflows the line box). Counting the
         * padded box made `<span style="padding:10px">` double its parent
         * line's height. Its glyph content still contributes - via the text
         * runs inside it, whose own boxes are clamped to the line height
         * below. */
        bool plain_inline = false;
        if (nd->el && !nd->is_text && !nd->pseudo) {
            plain_inline = sget(nd->style, "display") == "inline";
        }
        if (plain_inline) {
            /* start from the CONTENT top (the line top, not the padded
             * border top): the padding overflows, it does not push */
            inner_bottom = nd->content.y + scomp;
        }
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
            /* the root is the page scroller on demand: its scroll position
             * only matters when content overflows (cmax == 0 means the
             * whole page fits - no wheel/scroll frames, no scrollbar
             * columns to extend partial strips with). Registering it
             * unconditionally made EVERY scroll_el assignment (including
             * the select open/close repaint signal) look like a page
             * scroll: hover/focus/pressed markers were cleared and the
             * whole viewport repainted, so :hover/:active changes painted
             * nothing - "clicking flashes / hover sticks until the next
             * move". */
            if (cmax > 0) {
                r->scroll_boxes.push_back(nd->el);
            }
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
                int grown = inner_bottom - nd->border.y - scomp;
                /* max-height still caps an auto-height box: the growth above
                 * is "content is taller than the estimate", not a license to
                 * exceed the CSS clamp (a max-height:40 overflow:hidden box
                 * grew to its content height and ignored the limit). */
                std::string mxh = sget(nd->style, "max-height");
                if (!mxh.empty()) {
                    float m = 0.0f;
                    int unit = 0;
                    if (whaleui_style_parse_len(mxh.c_str(), &m, &unit) == 0 &&
                        unit == 0 && m > 0.0f) {
                        int cap = static_cast<int>(m);
                        if (grown > cap) {
                            grown = cap;
                        }
                    }
                }
                nd->border.h = grown;
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
    /* position:fixed paints at the viewport origin (paint_node zeroes the
     * ancestor scroll offsets at the fx node): mirror that here or a
     * scrolled page hit-tests the fixed header N px ABOVE where it is
     * drawn - the "fixed header hover/click misses after scrolling" bug. */
    if (n->fx) {
        off_y = 0;
    }
    /* translate/scale: mirror paint_node's offsets (nox = off + t +
     * origin*(1-scale); the element's own box is scaled around the origin,
     * children shift but are NOT scaled). Without this the hit region of a
     * transformed element and its whole subtree stays at the laid-out spot
     * while the paint moved - hover/click misses on lifted cards ("hover
     * only works while the mouse keeps moving"). rot/skew keep their
     * inverse-transform fallback below (unchanged approximation). */
    int dxs = 0, dys = 0;
    int sw = n->border.w, sh = n->border.h;
    /* the transform string parses once per visited node: the translate/scale
     * branch below and the rot/skew inverse branch share the result (the
     * old code ran whaleui_transform_eval twice per node per hit query). */
    whaleui_transform_t htf;
    bool has_htf = false;
    if (n->xf && n->el && !n->is_text) {
        if (whaleui_transform_eval(sget(n->style, "transform").c_str(),
                                   static_cast<float>(n->border.w),
                                   static_cast<float>(n->border.h),
                                   &htf) == 0) {
            has_htf = true;
        }
        if (has_htf && (htf.tx != 0.0f || htf.ty != 0.0f ||
                        htf.sx != 1.0f || htf.sy != 1.0f)) {
            float toox = static_cast<float>(n->border.w) * 0.5f;
            float tooy = static_cast<float>(n->border.h) * 0.5f;
            whaleui_transform_origin_eval(
                sget(n->style, "transform-origin").c_str(),
                static_cast<float>(n->border.w),
                static_cast<float>(n->border.h), &toox, &tooy);
            dxs = static_cast<int>(htf.tx) +
                  static_cast<int>(toox * (1.0f - htf.sx));
            dys = static_cast<int>(htf.ty) +
                  static_cast<int>(tooy * (1.0f - htf.sy));
            sw = static_cast<int>(static_cast<float>(n->border.w) * htf.sx);
            sh = static_cast<int>(static_cast<float>(n->border.h) * htf.sy);
        }
    }
    const int hx = x - dxs; /* the point in this node's PAINTED frame */
    const int hy = y - dys;
    int child_off = off_y + scroll_delta(r, n);
    /* chunked child walk (see paint_node): a chunk whose merged box does not
     * contain the point cannot contain it in any member, so skip it whole.
     * No cull margin here - hit testing must stay exact.
     * NO early return on a hit: paint draws siblings in document order, so
     * a LATER sibling covers an earlier one - the hit must follow the same
     * priority (the last matching sibling wins) or an overlapped card under
     * a later one steals the hover/click ("hover/click lands on the wrong
     * element where boxes overlap"). Non-overlapping children cost the same
     * as before (the chunk box still bounds the walk). */
    whaleui_layout_node_t* child_hit = nullptr;
    whaleui_child_chunk* ck = n->chunks;
    for (whaleui_layout_node_t* c = n->first_child; c;) {
        if (ck && c->chunk == ck) {
            const whaleui_child_chunk* cur = ck;
            ck = ck->next;
            const bool has_pt =
                hx >= cur->box.x && hx < cur->box.x + cur->box.w &&
                hy >= cur->box.y + child_off &&
                hy < cur->box.y + cur->box.h + child_off;
            if (!has_pt) {
                /* the chunk box is in LAYOUT space but the point is in
                 * this node's PAINTED frame: a transformed member paints
                 * outside its laid-out box, so a miss against the merged
                 * box is only conclusive when no member can move paint
                 * outside layout (no xf/fx/sk in the chunk). Otherwise
                 * walk the members (<= WUI_CHUNK probes, miss path only).
                 * Without this a lifted card's painted position
                 * hit-tested body ("hover dies once the lift paints"). */
                bool movable = false;
                for (whaleui_layout_node_t* m = cur->first;
                     m && m->chunk == cur; m = m->next) {
                    if (!m->is_text && (m->xf || m->fx || m->sk)) {
                        movable = true;
                        break;
                    }
                }
                if (!movable) {
                    c = cur->last ? cur->last->next : nullptr;
                    continue;
                }
            }
        }
        whaleui_layout_node_t* hit = hit_test(r, c, hx, hy, child_off);
        if (hit) {
            child_hit = hit;
        }
        c = c->next;
    }
    if (child_hit) {
        return child_hit;
    }
    if (hx >= n->border.x && hx < n->border.x + sw &&
        hy >= n->border.y + off_y && hy < n->border.y + off_y + sh) {
        return n;
    }
    /* a transformed box's own background/border follows the transform
     * around the transform-origin anchor (whaleui_gpu_rect_rot), so the
     * clickable region must follow too: inverse-transform the point into
     * the box's local frame and retest. Only rotation/skew are handled
     * here (translate/scale were already inverted exactly above); the
     * anchor mirrors paint (border + translation + origin). Inverse order
     * mirrors paint (un-rotate, then un-skew around the same anchor). */
    if (has_htf &&
        (htf.rot != 0.0f || htf.skx != 0.0f || htf.sky != 0.0f)) {
        float toox = static_cast<float>(n->border.w) * 0.5f;
        float toooy = static_cast<float>(n->border.h) * 0.5f;
        whaleui_transform_origin_eval(
            sget(n->style, "transform-origin").c_str(),
            static_cast<float>(n->border.w),
            static_cast<float>(n->border.h), &toox, &toooy);
        float cx = static_cast<float>(n->border.x) + toox;
        float cy = static_cast<float>(n->border.y + off_y) + toooy;
        const float rad = -htf.rot * 3.14159265f / 180.0f;
        float dx = static_cast<float>(hx) - cx;
        float dy = static_cast<float>(hy) - cy;
        float ux = dx * std::cos(rad) - dy * std::sin(rad);
        float uy = dx * std::sin(rad) + dy * std::cos(rad);
        /* un-skew around the anchor (inverse shear) */
        if (htf.skx != 0.0f || htf.sky != 0.0f) {
            float tx = 0, ty = 0;
            if (htf.skx != 0.0f) {
                tx = std::tan(htf.skx * 3.14159265f / 180.0f);
            }
            if (htf.sky != 0.0f) {
                ty = std::tan(htf.sky * 3.14159265f / 180.0f);
            }
            float det = 1.0f - tx * ty;
            if (det != 0.0f) {
                float ix = (ux - tx * uy) / det;
                float iy = (uy - ty * ux) / det;
                ux = ix;
                uy = iy;
            }
        }
        const float rx = cx + ux;
        const float ry = cy + uy;
        if (rx >= n->border.x && rx < n->border.x + n->border.w &&
            ry >= n->border.y + off_y &&
            ry < n->border.y + off_y + n->border.h) {
            return n;
        }
    }
    /* checkbox/radio: the painted control follows the layout box (UA 16x16,
     * 12-24px clamp in paint_checkbox), but the layout box can be taller
     * (line-box height) while the paint centers inside it. Hit the painted
     * control area so the whole visible control is clickable. */
    if (n->el && is_check_radio(n->el)) {
        int cbw = n->border.w > 0 ? n->border.w : 16;
        int cbh = n->border.h > 0 ? n->border.h : 16;
        if (cbw < 12) {
            cbw = 12;
        }
        if (cbh < 12) {
            cbh = 12;
        }
        if (cbw > 24) {
            cbw = 24;
        }
        if (cbh > 24) {
            cbh = 24;
        }
        int cbx = n->border.x;
        int cby = n->border.y + off_y;
        if (n->border.w > cbw) {
            cbx += (n->border.w - cbw) / 2;
        }
        if (n->border.h > cbh) {
            cby += (n->border.h - cbh) / 2;
        }
        if (hx >= cbx && hx < cbx + cbw && hy >= cby && hy < cby + cbh) {
            return n;
        }
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
        if (end[0] == 'r' && end[1] == 'e' && end[2] == 'm') {
            return n * 16.0f; /* root em: fixed 16 (mirrors layout) */
        }
        if (end[0] == 'v' && end[1] == 'w') {
            return n * static_cast<float>(r->fb_w) / 100.0f;
        }
        if (end[0] == 'v' && end[1] == 'h') {
            return n * static_cast<float>(r->fb_h) / 100.0f;
        }
        if (end[0] == 'v' && end[1] == 'm') {
            float m = r->fb_w < r->fb_h ? static_cast<float>(r->fb_w)
                                        : static_cast<float>(r->fb_h);
            if (end[2] == 'a' && end[3] == 'x') {
                m = r->fb_w > r->fb_h ? static_cast<float>(r->fb_w)
                                      : static_cast<float>(r->fb_h);
            }
            return n * m / 100.0f;
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
            if (args.empty()) {
                return 0;
            }
            float m = term_px(args[0], em);
            for (size_t k = 1; k < args.size(); ++k) {
                float b = term_px(args[k], em);
                if (b < m) {
                    m = b;
                }
            }
            return m;
        }
        if (t.compare(0, 4, "max(") == 0) {
            std::vector<std::string> args;
            split_args(t, 4, args);
            if (args.empty()) {
                return 0;
            }
            float m = term_px(args[0], em);
            for (size_t k = 1; k < args.size(); ++k) {
                float b = term_px(args[k], em);
                if (b > m) {
                    m = b;
                }
            }
            return m;
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
    int ww = is_single_line_input_el(el) ? 0 : run_wrap_w(geo);
    text_origin(r, geo, val, fs, family, bold, &tx, &ty, ww);
    int off = node_scroll_off(r, hit);
    /* single-line input content scrolls horizontally with the caret */
    int hx = 0;
    if (is_single_line_input_el(el)) {
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
/* find an element by its id attribute under a layout subtree (the render
 * context carries no document handle, so <label for="id"> resolves through
 * the layout tree instead of lxb_dom_document). Iterative: shares the
 * radio peer walk's no-recursion discipline. Returns nullptr when absent. */
static lxb_dom_element* find_el_by_id(whaleui_layout_node_t* root,
                                      const lxb_char_t* id, size_t idlen)
{
    if (!root || !id || idlen == 0) {
        return nullptr;
    }
    std::vector<whaleui_layout_node_t*> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        whaleui_layout_node_t* n = stack.back();
        stack.pop_back();
        for (whaleui_layout_node_t* c = n; c; c = c->next) {
            if (!c->is_text && c->el) {
                size_t ilen = 0;
                const lxb_char_t* iv = lxb_dom_element_get_attribute(
                    c->el, (const lxb_char_t*)"id", 2, &ilen);
                if (iv && ilen == idlen && std::memcmp(iv, id, idlen) == 0) {
                    return c->el;
                }
            }
            if (c->first_child) {
                stack.push_back(c->first_child);
            }
        }
    }
    return nullptr;
}
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

/* input type differences go through one filter expression at the generic
 * layer, the type is the config:
 * - disabled: rejects all editing (focus/insert/paste/IME)
 * - type=number: only digits + number-syntax chars (0-9 . - + e E), and
 *   the whole value keeps at most one dot (browser behavior: a second dot
 *   rejects the segment). Single writer of the number question: kind code
 *   picks the branch, no per-callsite type reads.
 * - other types (text/password/email/...) pass through unchanged.
 * Page-level custom filter hooks can attach here later (reserved in
 * whaleui.h). Returns the filtered insertable string; empty rejects. */
static std::string input_insert_filter(lxb_dom_element* el,
                                       const std::string& ins,
                                       size_t rep_a, size_t rep_b)
{
    if (ins.empty()) {
        return ins;
    }
    if (lxb_dom_element_has_attribute(
            el, (const lxb_char_t*)"disabled", 8)) {
        return std::string();
    }
    if (is_number_box_el(el)) {
        std::string out;
        out.reserve(ins.size());
        for (size_t i = 0; i < ins.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(ins[i]);
            if ((c >= '0' && c <= '9') || c == '.' || c == '-' ||
                c == '+' || c == 'e' || c == 'E') {
                out.push_back(static_cast<char>(c));
            }
        }
        if (!out.empty()) {
            std::string cur = edit_value(el);
            if (rep_a > cur.size()) {
                rep_a = cur.size();
            }
            if (rep_b > cur.size()) {
                rep_b = cur.size();
            }
            if (rep_a > rep_b) {
                std::swap(rep_a, rep_b);
            }
            std::string nv2 =
                cur.substr(0, rep_a) + out + cur.substr(rep_b);
            size_t dots = 0;
            for (size_t i = 0; i < nv2.size(); ++i) {
                if (nv2[i] == '.') {
                    ++dots;
                }
            }
            if (dots > 1) {
                return std::string();
            }
        }
        return out;
    }
    return ins;
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
    /* DOM input event on every actual value change (bubbles, like the
     * browser); arms the `change` dirty flag (commit on focus-out / Enter). */
    if (nv != val) {
        r->edit_changed_el = el;
        WUI_DOM_FIRE(reinterpret_cast<whaleui_dom_element_t*>(el),
                     "input", 0, 0, 0, 0, 0, 0);
    }
    edit_ensure_visible(r);
}

/* DOM change-event commit: the browser fires `change` when an edited value
 * leaves the control (focus-out, Enter on single-line inputs). Dispatch on
 * the element that armed the flag, if any. */
static void commit_change(whaleui_render_t* r)
{
    if (r->edit_changed_el) {
        lxb_dom_element* e = r->edit_changed_el;
        r->edit_changed_el = nullptr;
        WUI_DOM_FIRE(reinterpret_cast<whaleui_dom_element_t*>(e),
                     "change", 0, 0, 0, 0, 0, 0);
    }
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
    int wrap_w = is_single_line_input_el(r->edit_el) ? 0 : run_wrap_w(geo);
    int cx = 0, cy = 0, ch = 16;
    caret_pos(r, val, fs, family, bold,
              static_cast<size_t>(r->sel_focus), &cx, &cy, &ch, wrap_w);
    int tx = 0, ty = 0;
    text_origin(r, geo, val, fs, family, bold, &tx, &ty, wrap_w);
    int off = node_scroll_off(r, n);
    if (is_single_line_input_el(r->edit_el)) {
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

bool is_contenteditable(lxb_dom_element* el);

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

/* enter text-edit mode on `el`: set the edit target, caret at the end,
 * clear stale IME state and START text input (SDL_StartTextInput) so the
 * OS keyboard/IME is active. Called on Tab focus; clicking an editable
 * runs its own caret placement then the same StartTextInput. */
/* move the engine focus + DOM mirror + focus/blur events (defined next to
 * set_pressed_ex, the other focus-mover) */
static void render_focus_move(whaleui_render_t* r, lxb_dom_element* el,
                              int x, int y);
static void begin_editing(whaleui_render_t* r, lxb_dom_element* el)
{
    if (!r || !el) {
        return;
    }
    if (lxb_dom_element_has_attribute(
            el, (const lxb_char_t*)"disabled", 8)) {
        return; /* disabled controls never take the edit focus */
    }
    /* moving the edit focus commits any pending change on the previous
     * control (the browser fires `change` when the edited field loses
     * focus). */
    if (r->edit_el && r->edit_el != el && r->edit_changed_el == r->edit_el) {
        commit_change(r);
    }
    if (r->edit_el != el) {
        r->edit_changed_el = nullptr; /* fresh control: no change armed yet */
    }
    r->edit_el = el;
    r->sel_anchor_el = r->sel_focus_el = el;
    std::string vv = edit_value(el);
    r->sel_anchor = r->sel_focus = static_cast<int>(vv.size());
    r->nav_col = -1;
    r->compose.clear();
    r->compose_caret = -1;
    r->has_dirty = 1;
    r->caret_phase_painted = -1; /* new focus: repaint unconditionally */
    r->edit_scroll_need = 1;
    /* Tab focus is a real focus move: :focus styles, the DOM
     * activeElement mirror and focus/blur all follow it (single writer) */
    render_focus_move(r, el, 0, 0);
    if (r->window) {
        SDL_Window* ww = r->window;
        whaleui_sdl_on_main(
            [](void* p) { SDL_StartTextInput(static_cast<SDL_Window*>(p)); },
            ww);
    }
    edit_ensure_visible(r);
}

extern "C" void whaleui_render_focus_editable(whaleui_render_t* r, int dir)
{
    if (!r || !r->tree) {
        return;
    }
    std::vector<lxb_dom_element*> eds;
    /* by_el is the live map; the arena also holds the nodes of every subtree
     * an incremental relayout replaced, so an arena scan listed detached
     * duplicates (Tab could focus an element that is no longer laid out). */
    for (std::unordered_map<lxb_dom_element*, whaleui_layout_node_t*>::iterator
             it = r->tree->by_el.begin();
         it != r->tree->by_el.end(); ++it) {
        whaleui_layout_node_t* n = it->second;
        if (!n || !n->visible || !n->el || n->is_text || n->pseudo) {
            continue;
        }
        if (is_editable(n->el) &&
            !lxb_dom_element_has_attribute(
                n->el, (const lxb_char_t*)"disabled", 8)) {
            eds.push_back(n->el);
        }
    }
    if (eds.empty()) {
        return;
    }
    int cur = -1;
    if (r->edit_el) {
        for (size_t i = 0; i < eds.size(); ++i) {
            if (eds[i] == r->edit_el) {
                cur = static_cast<int>(i);
                break;
            }
        }
    }
    int nxt = cur < 0 ? 0
                      : (cur + dir + static_cast<int>(eds.size())) %
                            static_cast<int>(eds.size());
    if (nxt == cur) {
        return; /* single editable: keep the current focus */
    }
    begin_editing(r, eds[static_cast<size_t>(nxt)]);
}

void edit_key(whaleui_render_t* r, int keycode, int mods)
{
    lxb_dom_element* el = r->edit_el;
    if (!el) {
        return;
    }
    /* disabled controls ignore all editing keys (movement included, via
     * this generic gate; checkbox/button disabled is also refused by
     * their own focus/click handlers) */
    if (lxb_dom_element_has_attribute(
            el, (const lxb_char_t*)"disabled", 8)) {
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
                std::string sel = val.substr(static_cast<size_t>(a),
                                             static_cast<size_t>(b - a));
                /* DOM copy event first: a listener may preventDefault (cancel
                 * the engine's write) or supply its own text via
                 * set_clipboard_text */
                whaleui_event_init_t ci;
                std::memset(&ci, 0, sizeof(ci));
                ci.mods = r->last_mods;
                ci.clipboard_text = sel.c_str();
                const char* supplied = nullptr;
                int prevented = 0;
                whaleui_dom_dispatch_input_clipboard(
                    reinterpret_cast<whaleui_dom_element_t*>(el), "copy", &ci,
                    &prevented, &supplied);
                if (!prevented) {
                    SDL_SetClipboardText(supplied ? supplied : sel.c_str());
                }
            }
            return;
        case 'x': /* cut: copy + delete */
            if (a != b) {
                std::string sel = val.substr(static_cast<size_t>(a),
                                             static_cast<size_t>(b - a));
                whaleui_event_init_t ci;
                std::memset(&ci, 0, sizeof(ci));
                ci.mods = r->last_mods;
                ci.clipboard_text = sel.c_str();
                const char* supplied = nullptr;
                int prevented = 0;
                whaleui_dom_dispatch_input_clipboard(
                    reinterpret_cast<whaleui_dom_element_t*>(el), "cut", &ci,
                    &prevented, &supplied);
                if (!prevented) {
                    SDL_SetClipboardText(supplied ? supplied : sel.c_str());
                    edit_replace(r, el, static_cast<size_t>(a),
                                 static_cast<size_t>(b), "");
                }
            }
            return;
        case 'v': { /* paste from the system clipboard */
            char* cl = SDL_GetClipboardText();
            std::string clip = cl ? cl : "";
            SDL_free(cl);
            /* DOM paste event: a listener may preventDefault (cancel the
             * insert) or supply its own text */
            whaleui_event_init_t ci;
            std::memset(&ci, 0, sizeof(ci));
            ci.mods = r->last_mods;
            ci.clipboard_text = clip.c_str();
            const char* supplied = nullptr;
            int prevented = 0;
            whaleui_dom_dispatch_input_clipboard(
                reinterpret_cast<whaleui_dom_element_t*>(el), "paste", &ci,
                &prevented, &supplied);
            if (prevented) {
                return;
            }
            const char* src = supplied ? supplied : clip.c_str();
            if (!src || !*src) {
                return;
            }
            std::string pv = input_insert_filter(
                el, std::string(src), static_cast<size_t>(a),
                static_cast<size_t>(b));
            if (pv.empty()) {
                return;
            }
            edit_replace(r, el, static_cast<size_t>(a),
                         static_cast<size_t>(b), pv);
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
        /* multi-line editable inserts a newline; single-line inputs commit
         * the edit (the browser fires `change` on Enter for text inputs)
         * and, when inside a <form>, dispatch `submit` on the form (bubbles;
         * the engine has no form navigation, preventDefault is the contract
         * hook) */
        if (is_single_line_input_el(el) && r->edit_changed_el == el) {
            commit_change(r);
        }
        if (is_single_line_input_el(el)) {
            for (lxb_dom_node* p = el->node.parent; p; p = p->parent) {
                if (p->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                    continue;
                }
                lxb_dom_element* pe = lxb_dom_interface_element(p);
                if (tag_eq(pe, "form")) {
                    WUI_DOM_FIRE(reinterpret_cast<whaleui_dom_element_t*>(pe),
                                 "submit", 0, 0, 0, 0, 0, 0);
                    break;
                }
            }
        }
        if (!is_single_line_input_el(el) && is_editable(el)) {
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
    r->el_ver_seq = 0;
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
    r->caret_phase_painted = -1;
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
    /* join the background expand-fill thread before freeing anything it
     * reads (rules, DOM, tree, vars) */
    if (r->fill_thread) {
        r->fill_stop.store(1);
        r->fill_thread->join();
        delete r->fill_thread;
        r->fill_thread = nullptr;
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
    /* decoded image surfaces (owned per src, NULL = unloadable) */
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
    /* replacing the stylesheet while the background fill thread is
     * appending rows (it reads r->rules under tree_mx) must take the same
     * lock - otherwise the fill can touch rules that were just freed */
    std::unique_lock<std::recursive_mutex> lk(r->tree_mx);
    if (r->rules) {
        whaleui_css_rules_destroy(r->rules, r->rule_count);
        r->rules = nullptr;
        r->rule_count = 0;
    }
    r->has_interact_rules = 0;
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
            r->has_interact_rules = 0;
            for (size_t i = 0; i < count; ++i) {
                const char* sel = rules[i].selector ? rules[i].selector : "";
                if (std::strstr(sel, ":hover") || std::strstr(sel, ":active") ||
                    std::strstr(sel, ":focus")) {
                    r->has_interact_rules = 1;
                }
            }
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
    /* rules changed: a plain resize must no longer take the geometry-only
     * re-layout (its widths/styles came from the old ruleset) */
    r->geometry_only = 0;
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
    /* serialized against the background fill thread (reads the layout tree) */
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
    fb_coords(r, x, y);
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    return hit ? reinterpret_cast<whaleui_dom_element_t*>(hit->el) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_render_focus_element(whaleui_render_t* r)
{
    return r ? reinterpret_cast<whaleui_dom_element_t*>(r->focus_el) : nullptr;
}

/* --- selection accessors (window.getSelection() backing) --- */

extern "C" whaleui_dom_element_t* whaleui_render_sel_anchor_element(whaleui_render_t* r)
{
    return r ? reinterpret_cast<whaleui_dom_element_t*>(r->sel_anchor_el) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_render_sel_focus_element(whaleui_render_t* r)
{
    return r ? reinterpret_cast<whaleui_dom_element_t*>(r->sel_focus_el) : nullptr;
}

extern "C" int whaleui_render_sel_anchor_offset(whaleui_render_t* r)
{
    return r ? r->sel_anchor : 0;
}

extern "C" int whaleui_render_sel_focus_offset(whaleui_render_t* r)
{
    return r ? r->sel_focus : 0;
}

/* The selected text. Same-element: the byte slice between the offsets.
 * Cross-element: the anchor element's tail, every fully-covered element's
 * text (layout-tree document order between the endpoints), the focus
 * element's head. Mirrors sel_range_for's coverage rules; the middle uses
 * the layout tree's preorder when available, falling back to the DOM walk
 * (an unrendered selection cannot happen in practice: selection only starts
 * from a rendered document). */
extern "C" std::string whaleui_render_selected_text(whaleui_render_t* r)
{
    std::string out;
    if (!r) {
        return out;
    }
    lxb_dom_element* sa = r->sel_anchor_el;
    lxb_dom_element* sf = r->sel_focus_el;
    if (!sa || !sf) {
        return out;
    }
    if (sa == sf) {
        if (r->sel_anchor == r->sel_focus) {
            return out;
        }
        size_t a = static_cast<size_t>(r->sel_anchor);
        size_t b = static_cast<size_t>(r->sel_focus);
        if (a > b) {
            std::swap(a, b);
        }
        std::string v = edit_value(sa);
        if (b <= v.size()) {
            out = v.substr(a, b - a);
        } else if (a < v.size()) {
            out = v.substr(a);
        }
        return out;
    }
    /* cross-element: partial tails/heads + fully-in-between elements. Walk
     * the layout tree in paint order (the same order the highlight uses). */
    bool in_middle = false;
    bool saw_anchor = false;
    bool saw_focus = false;
    std::function<void(whaleui_layout_node_t*)> walk =
        [&](whaleui_layout_node_t* n) {
            if (!n || saw_focus) return;
            if (n->el) {
                if (n->el == sa && !saw_anchor) {
                    saw_anchor = true;
                    in_middle = true;
                    std::string v = edit_value(sa);
                    size_t a = static_cast<size_t>(r->sel_anchor);
                    if (a < v.size()) out += v.substr(a);
                } else if (n->el == sf && saw_anchor) {
                    saw_focus = true;
                    std::string v = edit_value(sf);
                    size_t b = static_cast<size_t>(r->sel_focus);
                    if (b > v.size()) b = v.size();
                    out += v.substr(0, b);
                    return;
                } else if (in_middle && !n->is_text && !n->pseudo) {
                    /* fully-covered element between the endpoints */
                    out += edit_value(n->el);
                }
            }
            for (whaleui_layout_node_t* c = n->first_child; c && !saw_focus;
                 c = c->next) {
                walk(c);
            }
        };
    if (r->tree && r->tree->root) {
        walk(r->tree->root);
        if (saw_anchor && saw_focus) {
            return out;
        }
        out.clear(); /* layout order disagreed (detached endpoints): DOM walk */
    }
    /* DOM-order fallback: preorder over the document element */
    lxb_dom_node* root = sa->node.owner_document
        ? reinterpret_cast<lxb_dom_node*>(
              lxb_dom_document_element(sa->node.owner_document))
        : nullptr;
    in_middle = false;
    saw_anchor = false;
    saw_focus = false;
    std::function<void(lxb_dom_node*)> dom_walk = [&](lxb_dom_node* n) {
        while (n && !saw_focus) {
            if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element* e = lxb_dom_interface_element(n);
                if (e == sa && !saw_anchor) {
                    saw_anchor = true;
                    in_middle = true;
                    std::string v = edit_value(sa);
                    size_t a = static_cast<size_t>(r->sel_anchor);
                    if (a < v.size()) out += v.substr(a);
                } else if (e == sf && saw_anchor) {
                    saw_focus = true;
                    std::string v = edit_value(sf);
                    size_t b = static_cast<size_t>(r->sel_focus);
                    if (b > v.size()) b = v.size();
                    out += v.substr(0, b);
                    return;
                } else if (in_middle) {
                    out += edit_value(e);
                }
                if (n->first_child) {
                    dom_walk(n->first_child);
                }
            }
            n = n->next;
        }
    };
    if (root) {
        dom_walk(root);
    }
    return out;
}

extern "C" void whaleui_render_clear_selection(whaleui_render_t* r)
{
    if (!r) {
        return;
    }
    r->sel_anchor_el = r->sel_focus_el = nullptr;
    r->sel_anchor = r->sel_focus = 0;
    r->sel_mode = 0;
    r->selecting = 0;
    r->drag_sel = 0;
    r->drag_sel_active = 0;
    /* a repaint is needed: the highlight is gone */
    r->scroll_dirty = 1;
    r->scroll_el = nullptr;
}

extern "C" void whaleui_render_select_all_of(whaleui_render_t* r,
                                             whaleui_dom_element_t* elt)
{
    lxb_dom_element* el = reinterpret_cast<lxb_dom_element*>(elt);
    if (!r || !el) {
        return;
    }
    std::string v = edit_value(el);
    r->sel_anchor_el = el;
    r->sel_focus_el = el;
    r->sel_anchor = 0;
    r->sel_focus = static_cast<int>(v.size());
    r->sel_mode = 0;
    r->selecting = 0;
    r->drag_sel = 0;
    r->drag_sel_active = 0;
    r->scroll_dirty = 1;
    r->scroll_el = nullptr;
}

extern "C" void whaleui_render_reset_dom(whaleui_render_t* r)
{
    if (!r) {
        return;
    }
    /* The async first-layout worker walks the document and the rule array.
     * The caller destroys the old document right after this call, so the
     * worker MUST be gone before that - otherwise it keeps reading freed
     * lexbor nodes and publishes a tree built from a dead document, which
     * the next frame adopts and paints (use-after-free: "the tab bar shows
     * but the content never appears, then the app hangs"). Join it here,
     * under the tree lock, and drop whatever it produced. */
    {
        std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
        if (r->layout_thread) {
            /* the worker only reads; wait for it to finish (bounded by one
             * full layout) and discard the result - it belongs to the old
             * document */
            r->layout_thread->join();
            delete r->layout_thread;
            r->layout_thread = nullptr;
            r->layout_done.store(0, std::memory_order_relaxed);
        }
        if (r->layout_pending) {
            whaleui_layout_destroy(r->layout_pending);
            r->layout_pending = nullptr;
        }
    }
#ifdef WHALEUI_BUILD_FULL
    /* run-raster cache: entries are owned vectors keyed by the full string
     * key (see draw_text_at) - clearing drops every raster at once */
    r->text_cache.clear();
    r->tc_bytes = 0;
    for (auto& im : r->images) {
        SDL_DestroySurface(im.second);
    }
    r->images.clear();
#endif
    /* caches keyed on DOM ELEMENTS / TEXT BUFFER POINTERS of the destroyed
     * document: element versions (text rasters), whole-run widths (keyed on
     * the text buffer address), wrapped sizes and the line-layout tables.
     * A later allocation can reuse a freed address with the same length and
     * silently serve stale data - the pointer-keyed caches must die with
     * the document, not linger for the next one. */
    r->el_ver.clear();
    r->text_w_cache.clear();
    r->text_size_cache.clear();
    whaleui_text_caches_reset();
    r->open_select = nullptr;
    r->open_select_hover = 0;
    r->select_index.clear();
    r->scrolls.clear();
    r->hscrolls.clear();
    r->last_scrolls.clear();
    r->hover_el = nullptr;
    r->hover_old_el = nullptr;
    r->hover_prev.clear();
    r->focus_old_el = nullptr;
    r->pressed_old_el = nullptr;
    r->state_pending = 0;
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
    /* the old layout tree holds pointers into the destroyed document (and
     * the animation state keys elements of it): drop both now, not on the
     * next frame - events processed in the same batch (a mouse-up right
     * after the navigating click) hit-test the tree and would walk freed
     * lexbor nodes. */
    if (r->tree) {
        if (g_last_tree == r->tree) {
            g_last_tree = nullptr;
        }
        whaleui_layout_destroy(r->tree);
        r->tree = nullptr;
    }
    if (r->anim) {
        whaleui_anim_reset(r->anim);
    }
    r->has_dirty = 1;
    r->presented_valid = 0;
}

extern "C" void whaleui_render_invalidate(whaleui_render_t* r)
{
    if (r) {
        r->has_dirty = 1;
        r->presented_valid = 0;
    }
}

/* cheap state tick for parked windows (minimized / hidden / occluded):
 * advance running animations' clocks + smooth-scroll easing WITHOUT
 * relayout, paint or present, so an uncover/expose resumes mid-motion
 * instead of jumping. Mutates no geometry: the next real frame repaints
 * from the current values. Keeps r->alive warm so the worker's self-drive
 * does not stall the loop while parked. */
extern "C" void whaleui_render_tick_only(whaleui_render_t* r)
{
    if (!r) {
        return;
    }
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
    const uint64_t now = SDL_GetTicks();
    bool keep = false;
    if (r->anim && whaleui_anim_tick(r->anim, now) != 0) {
        keep = true;
    }
    if (keep) {
        r->alive = 1;
    }
}

/* drop the presented-frame fast-path cache (exposure / document reset):
 * the next present repaints unconditionally. */
extern "C" void whaleui_render_drop_presented(whaleui_render_t* r)
{
    if (!r) {
        return;
    }
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
    r->presented_valid = 0;
}

extern "C" void whaleui_render_invalidate_rect(whaleui_render_t* r, int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
    if (r) {
        r->has_dirty = 1;
        r->presented_valid = 0;
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
    r->presented_valid = 0;
    /* stale scroll deltas from the old size would make the first scroll
     * after a resize shift by the accumulated difference */
    r->last_scrolls.clear();
    /* the stylesheet did not change with the size: the next layout pass
     * re-lays-out the existing tree (geometry only) instead of rebuilding
     * it - keeps a window drag on a 34k-node page responsive */
    r->geometry_only = 1;
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
    /* serialized against the background fill thread (reads the layout tree) */
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
    fb_coords(r, x, y);
    /* the select that was open BEFORE this click decides the toggle below: the
     * click-outside branch clears open_select, so reading it afterwards always
     * sees null and the dropdown would re-open on every click */
    lxb_dom_element* const was_open_select = r->open_select;
    /* 1. clicking inside the expanded list chooses an option */
    if (r->open_select) {
        whaleui_layout_node_t* s = find_node_by_el(r->tree, r->open_select);
        if (!s) {
            r->open_select = nullptr;
            r->open_select_t = 0;
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
                /* capture the index BEFORE the pick overwrites it: the
                 * input/change dispatch below compares old vs new (the
                 * browser fires nothing when re-picking the same option) */
                const int old_idx = select_current_index(r, s->el, texts);
                r->select_index[r->open_select] = idx;
                /* write the pick back into the DOM (`selected` on the
                 * option): the DOM is the source of truth for the value -
                 * get_value/set_value and both paint paths read it through
                 * select_current_index. The click-time select_index stays
                 * as the tiebreak for this frame. */
                {
                    int n = 0;
                    for (lxb_dom_node* c = r->open_select->node.first_child;
                         c; c = c->next) {
                        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                            continue;
                        }
                        lxb_dom_element* opt = lxb_dom_interface_element(c);
                        size_t olen = 0;
                        const lxb_char_t* nm =
                            lxb_dom_element_local_name(opt, &olen);
                        if (!nm || olen != 6 ||
                            std::memcmp(nm, "option", 6) != 0) {
                            continue;
                        }
                        if (n == idx) {
                            lxb_dom_element_set_attribute(
                                opt, (const lxb_char_t*)"selected", 8,
                                (const lxb_char_t*)"", 0);
                        } else {
                            lxb_dom_element_remove_attribute(
                                opt, (const lxb_char_t*)"selected", 8);
                        }
                        ++n;
                    }
                }
                /* the value raster is keyed on (element, geometry, color)
                 * - none of which change with the selection. Bump this
                 * element's version so its value re-rasterizes instead of
                 * serving the old option's glyphs ("select shows the
                 * previous value"). */
                r->el_ver[r->open_select] = ++r->el_ver_seq;
                /* DOM contract: picking a DIFFERENT option fires input then
                 * change on the select (re-picking the same option fires
                 * nothing). */
                if (old_idx != idx) {
                    WUI_DOM_FIRE(
                        reinterpret_cast<whaleui_dom_element_t*>(
                            s->el),
                        "input", 0, 0, 0, 0, 0, 0);
                    WUI_DOM_FIRE(
                        reinterpret_cast<whaleui_dom_element_t*>(
                            s->el),
                        "change", 0, 0, 0, 0, 0, 0);
                }
                r->open_select = nullptr;
                r->open_select_t = 0;
                /* no relayout: opening/closing/picking changes NO style or
                 * geometry (the popup paints outside the tree) - only the
                 * viewport must repaint at the same offsets. A whole-tree
                 * rebuild here recreated every node: tests holding node
                 * pointers saw orphans, and every toggle paid a full
                 * layout. scroll_el (+ scroll_dirty, otherwise the frame's
                 * idle check skips the paint entirely) forces exactly one
                 * full-viewport paint (scroll_frame semantics) and clears
                 * stale hover damage. */
                r->scroll_el = s->el;
                r->scroll_dirty = 1;
                if (out_value) {
                    static std::string chosen;
                    chosen = values[idx];
                    *out_value = chosen.c_str();
                }
                return 1;
            }
        }
        /* click outside the list closes it: the popup region must
         * repaint (it is drawn outside the tree, so no state/dirty path
         * covers it) - a scroll frame repaints the whole viewport once */
        r->open_select = nullptr;
        r->open_select_t = 0;
        r->scroll_el = s->el;
        r->scroll_dirty = 1;
    }
    /* 2. clicking a <select> toggles it open */
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    if (hit && !hit->is_text && hit->tag_id == WUI_TAG_SELECT) {
        /* actually TOGGLE, from the pre-click state (was_open_select): an
         * unconditional assignment re-opened the dropdown in the very click
         * that should have closed it, so it could never be closed ("the click
         * does nothing") and its options stayed drawn beside the control's own
         * value ("the text renders twice") */
        r->open_select = (was_open_select == hit->el) ? nullptr : hit->el;
        r->open_select_hover = 0;
        /* timestamp the open for the CSS open transition (paint fades +
         * rises the popup over transition-duration); a close needs none */
        r->open_select_t =
            r->open_select ? whaleui_anim_now() : 0;
        r->scroll_el = hit->el;
        r->scroll_dirty = 1;
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
                        /* collapse: stop the background fill thread (it
                         * appends rows to a subtree that is about to be
                         * rebuilt away); the frame below reaps it */
                        r->fill_stop.store(1);
                        r->stream_expand.active = false;
                    } else {
                        /* (the whole handler runs under tree_mx, see
                         * handle_click) */
                        lxb_dom_element_set_attribute(
                            det, (const lxb_char_t*)"open", 4,
                            (const lxb_char_t*)"", 0);
                        /* progressive expand: the first relayout below
                         * builds only the viewport's worth of rows (the
                         * Builder consumes tree->pending_budget); the rest
                         * streams in via append_rows on later frames */
                        lxb_dom_element* ul_el = nullptr;
                        for (lxb_dom_node* dn = det->node.first_child; dn;
                             dn = dn->next) {
                            if (dn->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                                continue;
                            }
                            lxb_dom_element* de =
                                lxb_dom_interface_element(dn);
                            size_t ulen = 0;
                            const lxb_char_t* uname =
                                lxb_dom_element_local_name(de, &ulen);
                            if (uname && ulen == 2 && uname[0] == 'u' &&
                                uname[1] == 'l') {
                                ul_el = de;
                                break;
                            }
                        }
                        if (r->tree) {
                            /* one row per viewport line: the first paint
                             * shows the whole viewport filled */
                            size_t rows = 1;
                            if (r->tree->root && r->height > 0) {
                                rows = static_cast<size_t>(r->height / 25) +
                                       8;
                            }
                            r->tree->pending_budget = rows;
                        }
                        r->stream_expand.det = det;
                        r->stream_expand.list = ul_el;
                        r->stream_expand.active = true;
                    }
                    /* incremental relayout, not a full-tree rebuild: the
                     * dirty set below rebuilds only this <details> subtree
                     * + one box pass. A whole-tree rebuild re-cascaded
                     * every element (22k-node details froze the UI ~48s).
                     * Clear a pending full-rebuild request (a mouse-move
                     * hover change queued in the same event batch sets
                     * has_dirty; the incremental pass below re-cascades the
                     * details subtree with the current hover state, so the
                     * full rebuild is redundant). */
                    r->has_dirty = 0;
                    /* expand/collapse changes the subtree shape (the
                     * <ul> rows appear/disappear): structural dirty, so
                     * the expand relayout consumes the progressive budget
                     * (viewport rows first, rest appended per frame) */
                    whaleui_dom_mark_dirty_struct(det);
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
        /* for="id": the labelled input anywhere in the document (needs the
         * renderer's tree root as the document proxy - the render context
         * carries no document handle) */
        size_t flen = 0;
        const lxb_char_t* forv = lxb_dom_element_get_attribute(
            lab, (const lxb_char_t*)"for", 3, &flen);
        if (forv && flen > 0 && r->tree && r->tree->root) {
            toggle_el = find_el_by_id(r->tree->root, forv, flen);
            if (toggle_el && !is_check_radio(toggle_el)) {
                toggle_el = nullptr;
            }
        }
        /* wrapped input: first check/radio descendant of the label (the
         * common <label><input>...</label> form; deep, not just direct
         * children) */
        if (!toggle_el) {
            std::vector<lxb_dom_node*> stack;
            stack.push_back(lab->node.first_child);
            while (!stack.empty() && !toggle_el) {
                lxb_dom_node* nd = stack.back();
                stack.pop_back();
                while (nd) {
                    if (nd->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                        lxb_dom_element* e =
                            lxb_dom_interface_element(nd);
                        if (is_check_radio(e)) {
                            toggle_el = e;
                            break;
                        }
                        if (nd->first_child) {
                            stack.push_back(nd->first_child);
                        }
                    }
                    nd = nd->next;
                }
            }
        }
    }
    if (toggle_el && is_check_radio(toggle_el)) {
        lxb_dom_element* hit2 = toggle_el;
        bool was_checked = lxb_dom_element_has_attribute(
            hit2, (const lxb_char_t*)"checked", 7);
        bool r0 = whaleui_input_kind(hit2) == 2 &&
                  is_radio_type(hit2);
        bool flipped = false;
        if (was_checked) {
            /* a checked radio does NOT un-toggle on a second click (browser
             * behavior: it only switches to another radio in the group). A
             * checkbox does. */
            if (!r0) {
                lxb_dom_element_remove_attribute(
                    hit2, (const lxb_char_t*)"checked", 7);
                flipped = true;
            }
        } else {
            lxb_dom_element_set_attribute(hit2, (const lxb_char_t*)"checked", 7,
                                          (const lxb_char_t*)"", 0);
            if (r0) {
                radio_uncheck_peers(hit2);
            }
            flipped = true;
        }
        /* NO whole-tree rebuild: flipping `checked` is a style-only DOM
         * edit in the common case (the UA :checked rules only recolor;
         * layout-key changes fall back to a rebuild through the normal
         * DOM-dirty layout-key diff). A rebuild here recreated every node
         * per click and raced the repaint - blank-window frames for
         * seconds on control-heavy pages. Mark the edit through the DOM
         * dirty path instead (lexbor set/remove_attribute do not go
         * through whaleui_dom_mark_dirty - they bypass the public API). */
        whaleui_dom_mark_dirty(hit2);
        r->hover_recheck = 1;
        /* DOM input/change on an actual checkedness flip (the browser fires
         * input then change as the activation behavior; the ordering
         * compromise here matches handle_click's existing click-after-
         * default-behavior choice - listeners observe the toggled state). */
        if (flipped) {
            WUI_DOM_FIRE(reinterpret_cast<whaleui_dom_element_t*>(hit2),
                         "input", 0, 0, 0, 0, 0, 0);
            WUI_DOM_FIRE(reinterpret_cast<whaleui_dom_element_t*>(hit2),
                         "change", 0, 0, 0, 0, 0, 0);
        }
    }
    return 0;
}

/* The pointer left the window: no further motion events arrive, so the last
 * hovered element would stay stuck in :hover ("moving away sometimes does not
 * react - especially when the mouse leaves quickly"). Same bookkeeping
 * set_hover does on a miss, but with no hit-test (there is no position). */
extern "C" void whaleui_render_clear_hover(whaleui_render_t* r)
{
    if (!r || !r->tree) {
        return;
    }
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
    /* the pointer is gone: the frame's hover re-check must not re-hit-test
     * the stale position and resurrect :hover after a scroll/relayout */
    r->mouse_has = 0;
    if (!r->hover_el) {
        return;
    }
    /* DOM mouseleave on the element losing the pointer (browser: leaving the
     * window fires mouseleave/mouseout on the hovered chain; the position is
     * the last one seen). Non-bubbling, like the browser's enter/leave. */
    {
        int lx = static_cast<int>(r->mouse_wx);
        int ly = static_cast<int>(r->mouse_wy);
        WUI_DOM_FIRE_NB(reinterpret_cast<whaleui_dom_element_t*>(r->hover_el),
                        "mouseleave", 0, lx, ly, 0, 0, 0);
    }
    if (r->has_interact_rules && r->rule_count > 0) {
        if (r->hover_old_el) {
            r->hover_prev.push_back(r->hover_old_el);
        }
        r->hover_old_el = r->hover_el;
        r->state_pending = 1;
    } else {
        r->hover_old_el = nullptr;
        r->hover_prev.clear();
    }
    r->hover_el = nullptr;
    r->has_dirty = 1;
}

/* pointer cursor for natively clickable elements (links / controls).
 * Single writer: UA CSS already declares cursor:pointer for these tags, so
 * this only covers what the cascade did NOT set (an unstyled UA-less page).
 * Keep in sync with theme_css.cpp's cursor rules. */
static bool natively_clickable(lxb_dom_element* el)
{
    if (!el) {
        return false;
    }
    size_t tlen = 0;
    const lxb_char_t* tname = lxb_dom_element_local_name(el, &tlen);
    return tname &&
           ((tlen == 1 && tname[0] == 'a') ||
            (tlen == 6 &&
             (std::memcmp(tname, "select", 6) == 0 ||
              std::memcmp(tname, "button", 6) == 0)) ||
            (tlen == 7 && std::memcmp(tname, "summary", 7) == 0));
}

/* resolve the system cursor for the hovered element: I-beam over editable
 * text, pointer over links/clickable controls (explicit `cursor` style
 * first, natively-clickable tags as fallback), arrow otherwise */
static SDL_Cursor* cursor_for_hover(whaleui_render_t* r,
                                    whaleui_layout_node_t* hit)
{
    SDL_Cursor* want = render_cursor(r, SDL_SYSTEM_CURSOR_DEFAULT);
    lxb_dom_element* el = hit ? hit->el : nullptr;
    if (!el) {
        return want;
    }
    if (is_editable(el)) {
        return render_cursor(r, SDL_SYSTEM_CURSOR_TEXT);
    }
    std::string c = sget(hit->style, "cursor");
    if (c == "pointer" || (c.empty() && natively_clickable(el))) {
        want = render_cursor(r, SDL_SYSTEM_CURSOR_POINTER);
    }
    return want;
}

extern "C" void whaleui_render_set_hover(whaleui_render_t* r, int x, int y)
{
    if (!r) {
        return;
    }
    /* remember the raw window-space position for the frame's hover
     * re-check (see mouse_wx in render.h): relayouts/scrolls can move a
     * different element under a stationary pointer */
    float prev_x = r->mouse_wx;
    float prev_y = r->mouse_wy;
    int had_prev = r->mouse_has;
    r->mouse_wx = static_cast<float>(x);
    r->mouse_wy = static_cast<float>(y);
    r->mouse_has = 1;
    /* MouseEvent.movementX/Y in window coordinates (before fb scaling) */
    float move_x = had_prev ? static_cast<float>(x) - prev_x : 0.0f;
    float move_y = had_prev ? static_cast<float>(y) - prev_y : 0.0f;
    if (!r->tree) {
        return;
    }
    /* serialized against the background fill thread (reads the layout tree) */
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
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
                /* repaint only (the highlight paints outside the tree):
                 * a full rebuild here recreated every node per mouse move */
                r->scroll_el = s->el;
                r->scroll_dirty = 1;
            }
        }
        /* the popup is a modal overlay: hover styles must NOT fall through
         * to the elements underneath it (an input below the list would
         * switch to an I-beam / apply its :hover). But the cursor is not
         * frozen: it follows the element under the pointer - the popup
         * options use the default arrow (unless they set their own cursor),
         * and anything outside the list shows its own cursor (the select's
         * own pointer must not leak to the whole page while its hover
         * stays pinned to the select). */
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
        /* popup options use the default arrow (unless they set their own
         * cursor); anything below the list follows its own cursor */
        SDL_Cursor* want = render_cursor(r, SDL_SYSTEM_CURSOR_DEFAULT);
        if (!in_list && hit2) {
            want = cursor_for_hover(r, hit2);
        }
        if (want) {
            SDL_SetCursor(want);
        }
        return;
    }
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    lxb_dom_element* el = hit ? hit->el : nullptr;
    if (el != r->hover_el) {
        lxb_dom_element* was = r->hover_el;
        if (r->has_interact_rules && r->rule_count > 0) {
            /* the mouse crossed more than one element since the last
             * frame: every previous target must lose its :hover styles,
             * not just the most recent one - queue the earlier ones so
             * the state relayout reverts them all (a fast move used to
             * leave every intermediate element stuck in :hover). */
            if (r->hover_old_el) {
                r->hover_prev.push_back(r->hover_old_el);
            }
            r->hover_old_el = r->hover_el;
        } else {
            r->hover_old_el = nullptr;
            r->hover_prev.clear(); /* no state rules: nothing to revert */
        }
        r->hover_el = el;
        SDL_SetCursor(cursor_for_hover(r, hit));
        /* interaction-state rules present -> a hover change can alter
         * styles: relayout only the previous + current element (incremental
         * DOM-dirty path in the frame loop). Without such rules hover is
         * only the cursor switch above - nothing to relayout. */
        if (r->has_interact_rules && r->rule_count > 0) {
            r->state_pending = 1;
        }
        /* DOM mouse events on a target change: mouseleave fires on the old
         * element, mouseenter on the new (both non-bubbling, like the
         * browser). relatedTarget names the counterpart element. */
        whaleui_event_init_t mi;
        std::memset(&mi, 0, sizeof(mi));
        mi.mouse_x = x;
        mi.mouse_y = y;
        mi.movement_x = move_x;
        mi.movement_y = move_y;
        mi.mods = r->last_mods;
        if (was) {
            mi.related_target = reinterpret_cast<whaleui_dom_element_t*>(el);
            whaleui_dom_dispatch_input_nonbubble(
                reinterpret_cast<whaleui_dom_element_t*>(was), "mouseleave",
                &mi, nullptr);
        }
        if (el) {
            mi.related_target = reinterpret_cast<whaleui_dom_element_t*>(was);
            whaleui_dom_dispatch_input_nonbubble(
                reinterpret_cast<whaleui_dom_element_t*>(el), "mouseenter",
                &mi, nullptr);
        }
    }
    /* mousemove fires on the element under the pointer every frame the
     * worker applies a coalesced motion position (target may be NULL: no
     * element, no dispatch - the browser sends pointer moves to the window
     * instead, which is the WHALEUI_WINDOW_EVENT path). */
    if (el) {
        whaleui_event_init_t mi;
        std::memset(&mi, 0, sizeof(mi));
        mi.mouse_x = x;
        mi.mouse_y = y;
        mi.movement_x = move_x;
        mi.movement_y = move_y;
        mi.mods = r->last_mods;
        whaleui_dom_dispatch_input(
            reinterpret_cast<whaleui_dom_element_t*>(el), "mousemove", &mi,
            nullptr);
    }
    /* drag to extend a selection: gated by a 6px threshold so a plain
     * click - including the incidental hand micro-motion of pressing a
     * mouse button - never selects. A press inside an existing selection
     * instead readies a drag-to-move/copy: the selection stays put and the
     * drop (move/copy) happens on mouse-up. */
    /* HTML5 drag session: once the pointer passes the threshold on a
     * draggable press candidate, start the session (dragstart) and keep it
     * updated (dragenter/dragover/dragleave). This takes priority over text
     * selection: a draggable element drags, it does not select. */
    if (r->press_hit_el && r->pressed_el) {
        if (!r->dnd_active) {
            int dx = x - r->press_x;
            int dy2 = y - r->press_y;
            if (dx * dx + dy2 * dy2 >= kDndThreshold * kDndThreshold) {
                dnd_begin(r, r->press_hit_el, x, y);
            }
        }
        if (r->dnd_active) {
            dnd_update(r, x, y);
            return;
        }
    }
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

/* --- HTML5 drag & drop (draggable="true") ---
 *
 * A press on a draggable element records the candidate; once the pointer
 * moves past kDndThreshold the session starts: dragstart on the source (the
 * listener may fill the DataTransfer), then dragenter/dragover on whatever is
 * under the pointer. Releasing over a target that called preventDefault in
 * dragover fires drop; otherwise the session is cancelled (dragend always
 * fires on the source). Mirrors the browser's "preventDefault in dragover
 * makes a drop target" contract. */

/* element.draggable: the attribute, plus the UA defaults the theme sets
 * (img/a are draggable in a browser; our UA CSS does not express that, so
 * only the explicit attribute is honored - documented). */
static bool is_draggable_el(lxb_dom_element* el)
{
    if (!el) {
        return false;
    }
    size_t len = 0;
    const lxb_char_t* v = lxb_dom_element_get_attribute(
        el, (const lxb_char_t*)"draggable", 9, &len);
    if (!v || len == 0) {
        return false;
    }
    return (len == 4 && std::memcmp(v, "true", 4) == 0) ? true : false;
}

/* the element's textContent (what a drag carries by default) */
static std::string el_text_content(lxb_dom_element* el)
{
    std::string out;
    if (!el) {
        return out;
    }
    std::function<void(lxb_dom_node*)> walk = [&](lxb_dom_node* n) {
        while (n) {
            if (n->type == LXB_DOM_NODE_TYPE_TEXT ||
                n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
                const lexbor_str_t* s = &lxb_dom_interface_text(n)->char_data.data;
                if (s->data) {
                    out.append(reinterpret_cast<const char*>(s->data), s->length);
                }
            } else if (n->first_child) {
                walk(n->first_child);
            }
            n = n->next;
        }
    };
    walk(el->node.first_child);
    return out;
}

static void dnd_begin(whaleui_render_t* r, lxb_dom_element* src, int x, int y)
{
    whaleui_data_transfer_t* dt = whaleui_data_transfer_create();
    /* pre-fill text/plain with the source's text (the browser uses the
     * selection; textContent is the closest equivalent); a dragstart listener
     * may replace it */
    std::string text = el_text_content(src);
    if (!text.empty()) {
        whaleui_data_transfer_set_text(dt, text.c_str());
    }
    whaleui_data_transfer_set_effect_allowed(dt, 1 /* copy */);
    r->dnd_active = 1;
    r->dnd_source = src;
    r->dnd_over = nullptr;
    r->dnd_dt = dt;
    r->dnd_started_x = x;
    r->dnd_started_y = y;
    int prevented = 0;
    whaleui_dom_dispatch_drag(reinterpret_cast<whaleui_dom_element_t*>(src),
                              "dragstart", dt, x, y, r->last_mods, &prevented);
    if (prevented) {
        /* a prevented dragstart cancels the session (browser contract) */
        whaleui_data_transfer_destroy(dt);
        r->dnd_dt = nullptr;
        r->dnd_active = 0;
        r->dnd_source = nullptr;
    }
}

/* per-motion update: hit-test, then dragenter/dragover on the new target and
 * dragleave on the old one. */
static void dnd_update(whaleui_render_t* r, int x, int y)
{
    if (!r->dnd_active || !r->tree || !r->dnd_dt) {
        return;
    }
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    lxb_dom_element* el = hit ? hit->el : nullptr;
    if (el != r->dnd_over) {
        if (r->dnd_over) {
            whaleui_dom_dispatch_drag(
                reinterpret_cast<whaleui_dom_element_t*>(r->dnd_over),
                "dragleave", r->dnd_dt, x, y, r->last_mods, nullptr);
        }
        r->dnd_over = el;
        if (el) {
            whaleui_dom_dispatch_drag(
                reinterpret_cast<whaleui_dom_element_t*>(el), "dragenter",
                r->dnd_dt, x, y, r->last_mods, nullptr);
        }
    }
    if (el) {
        int prevented = 0;
        whaleui_dom_dispatch_drag(reinterpret_cast<whaleui_dom_element_t*>(el),
                                  "dragover", r->dnd_dt, x, y, r->last_mods,
                                  &prevented);
        /* preventDefault marks the element as accepting the drop */
        if (prevented) {
            whaleui_data_transfer_set_drop_effect(r->dnd_dt, 1 /* copy */);
        } else {
            whaleui_data_transfer_set_drop_effect(r->dnd_dt, 0 /* none */);
        }
    }
}

/* release: drop on the accepting target (if any), then dragend on the source;
 * the transfer is destroyed either way. */
static void dnd_finish(whaleui_render_t* r, int x, int y, int cancelled)
{
    if (!r->dnd_active) {
        return;
    }
    lxb_dom_element* src = r->dnd_source;
    whaleui_data_transfer_t* dt = r->dnd_dt;
    lxb_dom_element* over = r->dnd_over;
    r->dnd_active = 0;
    r->dnd_source = nullptr;
    r->dnd_over = nullptr;
    r->dnd_dt = nullptr;
    if (!cancelled && over && dt) {
        int prevented = 0;
        whaleui_dom_dispatch_drag(reinterpret_cast<whaleui_dom_element_t*>(over),
                                  "drop", dt, x, y, r->last_mods, &prevented);
    }
    if (src) {
        whaleui_dom_dispatch_drag(reinterpret_cast<whaleui_dom_element_t*>(src),
                                  "dragend", dt, x, y, r->last_mods, nullptr);
    }
    whaleui_data_transfer_destroy(dt);
}

extern "C" int whaleui_render_drag_active(whaleui_render_t* r)
{
    return r ? r->dnd_active : 0;
}

extern "C" whaleui_dom_element_t* whaleui_render_drag_source(whaleui_render_t* r)
{
    if (!r || !r->dnd_source) {
        return nullptr;
    }
    return reinterpret_cast<whaleui_dom_element_t*>(r->dnd_source);
}

/* An OS drop (files or text dragged in from another app): hit-test the
 * pointer and dispatch the drag event on the element under it, with a
 * DataTransfer the caller built. The engine has no default action. */
extern "C" int whaleui_render_dispatch_os_drag(whaleui_render_t* r,
                                               const char* type,
                                               void* data_transfer, int x,
                                               int y)
{
    if (!r || !r->tree || !type) {
        return -1;
    }
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
    fb_coords(r, x, y);
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    if (!hit || !hit->el) {
        return -2; /* nothing under the pointer: no target, no dispatch */
    }
    return whaleui_dom_dispatch_drag(
        reinterpret_cast<whaleui_dom_element_t*>(hit->el), type, data_transfer,
        x, y, r->last_mods, nullptr);
}

extern "C" void whaleui_render_set_pressed(whaleui_render_t* r, int x, int y,
                                           int down)
{
    whaleui_render_set_pressed_ex(r, x, y, down, 1, 0);
}

/* Move the engine focus: set focus_el, mirror it into the DOM layer
 * (document.activeElement / public focus()/blur() API), and fire the
 * non-bubbling blur on the element losing focus + focus on the new one.
 * A null target (click outside the tree) clears the mirror, like the
 * browser's activeElement falling back to body. */
static void render_focus_move(whaleui_render_t* r, lxb_dom_element* el,
                              int x, int y)
{
    lxb_dom_element* old = r->focus_el;
    if (old == el) {
        return;
    }
    r->focus_el = el;
    if (el) {
        whaleui_dom_set_active(el);
    } else if (r->tree && r->tree->root && r->tree->root->el) {
        whaleui_dom_clear_active(r->tree->root->el->node.owner_document);
    }
    if (old) {
        WUI_DOM_FIRE_NB(reinterpret_cast<whaleui_dom_element_t*>(old),
                        "blur", 0, x, y, 0, 0, 0);
    }
    if (el) {
        WUI_DOM_FIRE_NB(reinterpret_cast<whaleui_dom_element_t*>(el),
                        "focus", 0, x, y, 0, 0, 0);
    }
}

extern "C" void whaleui_render_set_pressed_ex(whaleui_render_t* r, int x,
                                              int y, int down, int clicks,
                                              int mods)
{
    if (!r || !r->tree) {
        return;
    }
    /* serialized against the background fill thread (reads the layout tree) */
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
    fb_coords(r, x, y);
    lxb_dom_element* old_focus = r->focus_el;
    lxb_dom_element* old_pressed = r->pressed_el;
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
        /* a press on a draggable element arms an HTML5 drag session; it only
         * starts once the pointer passes the threshold (so a plain click on
         * a draggable row still behaves as a click) */
        r->press_hit_el = (el && is_draggable_el(el)) ? el : nullptr;
        /* scrollbar drag takes priority over selection/caret */
        whaleui_layout_node_t* sc = scrollbar_under(r, hit, x, y);
        if (sc) {
            r->drag_scroll_el = sc->el;
            r->drag_scroll_node = sc; /* valid until the next rebuild */
            update_drag_scroll(r, y);
            r->pressed_el = el;
            render_focus_move(r, el, x, y);
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
                render_focus_move(r, el, x, y);
                r->has_dirty = 1;
                return;
            }
        }
        if (el && is_editable(el) &&
            !lxb_dom_element_has_attribute(
                el, (const lxb_char_t*)"disabled", 8)) {
            /* focus the editable control and place the caret / word / line */
            if (r->edit_el != el) {
                /* moving edit focus: commit a pending change on the old one */
                if (r->edit_el && r->edit_changed_el == r->edit_el) {
                    commit_change(r);
                }
                r->edit_changed_el = nullptr;
                r->caret_phase_painted = -1; /* new focus: repaint */
            }
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
            if (r->window) {
                SDL_Window* ww = r->window;
                whaleui_sdl_on_main(
                    [](void* p) { SDL_StartTextInput(static_cast<SDL_Window*>(p)); },
                    ww);
            }
        } else if (hit && hit->is_text) {
            /* anchor a potential selection (only drags extend it) */
            if (r->edit_el) {
                if (r->edit_changed_el == r->edit_el) {
                    commit_change(r);
                }
                if (r->window) {
                    SDL_Window* ww = r->window;
                    whaleui_sdl_on_main(
                        [](void* p) { SDL_StopTextInput(static_cast<SDL_Window*>(p)); },
                        ww);
                }
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
                /* the edited control loses focus: commit its pending change */
                if (r->edit_changed_el == r->edit_el) {
                    commit_change(r);
                }
                if (r->window) {
                    SDL_Window* ww = r->window;
                    whaleui_sdl_on_main(
                        [](void* p) { SDL_StopTextInput(static_cast<SDL_Window*>(p)); },
                        ww);
                }
                r->edit_el = nullptr;
            }
            r->compose.clear();
            r->sel_anchor_el = r->sel_focus_el = nullptr;
            r->sel_anchor = r->sel_focus = 0;
            r->sel_mode = 0;
        }
        r->pressed_el = el;
        render_focus_move(r, el, x, y);
    } else {
        /* mouse up: a selection survives only when it was actually dragged
         * (or started as a double/triple click). A plain click (press+
         * release without crossing the threshold, even with incidental
         * micro-motion) leaves nothing selected. The caret of a focused
         * editable control is kept as-is. */
        r->pressed_el = nullptr;
        r->drag_scroll_el = nullptr;
        r->drag_scroll_node = nullptr;
        /* HTML5 drag session: releasing ends it - drop on the accepting
         * target (dnd_finish dispatches it), then dragend on the source. */
        if (r->dnd_active) {
            dnd_finish(r, x, y, 0);
            r->press_hit_el = nullptr;
            r->has_dirty = 1;
            return;
        }
        r->press_hit_el = nullptr;
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
        /* mouse-up clears :active on `old_pressed` (== the pre-release
         * pressed element): the element below records it as pressed_old
         * so the state pass reverts its :active style through the same
         * path as focus. Without this the up never produced a repaint
         * when focus was unchanged (the common plain-button click):
         * the :active style was applied on press, but the revert never
         * repainted - the button "flashed" and stuck until the next
         * motion. */
        if (old_pressed && old_pressed != r->pressed_el) {
            r->pressed_old_el = old_pressed;
        }
    }
    /* focus/pressed state change on a NON-editing path goes through the
     * unified interaction-state relayout (state_pending): :active / :focus
     * styles apply to the new element and revert on the old one via the
     * same layout-key diff -> style-only path as :hover. Editing (caret /
     * selection / popup) keeps the full repaint - those paths carry their
     * own state and repaint needs. */
    bool st_changed = r->focus_el != old_focus || r->pressed_el != old_pressed;
    /* mouse-up's pressed_old (set above) also counts: pressed no longer
     * equals old_pressed, so the equality above already covers it - but
     * when the press landed on the SAME element as focus it neither
     * differs (focus unchanged) - the explicit pressed_old check keeps
     * the revert on the state path instead of a full rebuild. */
    bool pressed_reverted = !down && r->pressed_old_el != nullptr;
    if ((st_changed || pressed_reverted) && !r->edit_el && !r->open_select &&
        !r->drag_sel && !r->drag_sel_active && r->has_interact_rules &&
        r->rule_count > 0) {
        r->focus_old_el = old_focus;
        r->pressed_old_el = old_pressed;
        r->state_pending = 1;
    } else {
        r->has_dirty = 1;
    }
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
    /* serialized against the background fill thread (reads the layout tree) */
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
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
            /* scroll event on the element that actually moved (bubbles,
             * like the browser) */
            WUI_DOM_FIRE(reinterpret_cast<whaleui_dom_element_t*>(sc->el),
                         "scroll", 0, 0, 0, 0, 0, 0);
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
    /* DOM wheel on the element under the pointer (bubbles): preventDefault
     * cancels the engine's scroll (the DOM contract). Deltas carry the
     * pixel amounts (CSS wheel semantics). */
    if (hit && hit->el) {
        int prevented = 0;
        whaleui_dom_dispatch_phased(
            reinterpret_cast<whaleui_dom_element_t*>(hit->el), "wheel", 0,
            x, y, 0, 0.0f, static_cast<float>(-delta), &prevented);
        if (prevented) {
            return;
        }
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
        if (is_single_line_input(n)) {
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
             * the next scrollable ancestor so the page keeps scrolling (an
             * inner card/textarea used to claim every wheel event). */
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
    /* border.y is DOCUMENT space (layout no longer bakes the scroll into
     * child coordinates - the P3 scroll fix); the target scroll is the
     * element's document y, minus room for the fixed header. The old
     * "+ cur" was left over from the baked model and overshot by the
     * current scroll on every click (an #anchor jumped past the target). */
    int target = n->border.y - 80; /* 80px for the fixed header */
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
    /* Tab / Shift+Tab: move the edit focus to the next / previous
     * editable control. Text input starts on FOCUS (not on typing), so
     * IME/keyboard is reliably enabled regardless of how the focus
     * arrived (click / Tab / API). */
    if (keycode == WHALEUI_KEY_TAB) {
        whaleui_render_focus_editable(
            r, (mods & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
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
    std::string ins = input_insert_filter(r->edit_el, std::string(utf8),
                                              static_cast<size_t>(a),
                                              static_cast<size_t>(b));
    if (ins.empty()) {
        return;
    }
    edit_replace(r, r->edit_el, static_cast<size_t>(a), static_cast<size_t>(b),
                 ins);
}

extern "C" void whaleui_render_handle_editing(whaleui_render_t* r,
                                              const char* utf8, int caret)
{
    if (!r) {
        return;
    }
    r->compose = utf8 ? utf8 : "";
    r->compose_caret = caret;
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

/* background <details> expand fill thread (spawned by the render frame):
 * appends one append_rows batch per tree_mx acquisition. The render frame
 * holds tree_mx while it touches the tree, so a batch never interleaves
 * with a frame; frames parked idle leave the lock free and the fill runs
 * at full speed. The text-metric context (g_metric_render) is only
 * touched under tree_mx, so the fill measures glyphs exactly like the
 * frame does. Exits when the DOM rows are exhausted, the expand was
 * cancelled (fill_stop / stream_expand cleared) or the tree was rebuilt. */
static const size_t kFillBatch = 80; /* rows per append_rows batch
                                      * (~22ms): short enough that an
                                      * interaction frame waits at most one
                                      * batch for the tree lock */
static void fill_worker_fn(whaleui_render_t* r)
{
    r->fill_finished.store(0);
    r->fill_running.store(1);
    int since_paint = 0;
    for (;;) {
        if (r->fill_stop.load()) {
            break;
        }
        {
            std::unique_lock<std::recursive_mutex> lk(r->tree_mx);
            if (r->fill_stop.load()) {
                break;
            }
            if (!r->tree || !r->tree->root || !r->stream_expand.active ||
                !r->stream_expand.list) {
                break; /* tree rebuilt / expand finished elsewhere */
            }
#ifdef WHALEUI_BUILD_FULL
            g_metric_render = r; /* real glyph metrics (same as frame) */
#endif
            int n = whaleui_layout_append_rows(
                r->tree, r->stream_expand.list, r->rules, r->rule_count,
                &r->theme_vars, nullptr, &r->scrolls, r->anim,
                r->text_scale, kFillBatch);
            if (n <= 0) {
                break; /* DOM rows exhausted or append blocked */
            }
        }
        /* repaint throttled: wake the render frame every ~4 batches
         * (~160 rows) instead of every batch - keeps the frame loop free
         * for input between batches */
        if (++since_paint >= 4) {
            since_paint = 0;
            r->fill_dirty.store(1); /* a chunk landed: repaint it */
        }
        /* yield is a busy-spin here: the fill re-acquires the lock before
         * the frame loop's worker thread gets scheduled, starving input
         * frames (hover mid-fill waited ~150ms). A short sleep hands the
         * CPU over so an interaction frame wins the lock between batches. */
        std::this_thread::sleep_for(std::chrono::microseconds(600));
    }
    r->fill_dirty.store(1); /* show the final rows */
    r->fill_running.store(0);
    r->fill_finished.store(1);
}

/* recompute the cascaded opacity of `nd`'s subtree: each element reads its
 * own style, text runs inherit the chain product */
static void anim_apply_opacity(whaleui_layout_node_t* nd, float po)
{
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
    for (whaleui_layout_node_t* c = nd->first_child; c; c = c->next) {
        anim_apply_opacity(c, nd->opacity);
    }
}

/* apply the animation tick's interpolated values to every running-animation
 * element NOT in `skip` (elements in `skip` had their values applied by the
 * layout build during a relayout â€?their whole subtree was rebuilt). Phase
 * 1 writes the styles + refreshes the cached paint flags (xf/fx/sk single
 * writer), phase 2 recomputes the opacity chains from the final styles.
 * Returns 1 when at least one style value actually changed (a frame whose
 * interpolated values all equal the current styles carries no visual
 * damage). This is the COMPOSITION glue between the two animation paths:
 * while a layout animation runs (demo: .anim-bar width), the frame takes
 * the relayout_multi branch and previously never applied paint-only
 * transitions (demo: .anim-card:hover lift) â€?the hover transform stayed
 * pinned at its transition's start value until the next state relayout,
 * so the card lifted only after the mouse left ("hover reacts after the
 * mouse moved away"). */
static int apply_paint_only_anim(whaleui_render_t* r,
                                 const std::vector<lxb_dom_element*>* skip)
{
    int n = whaleui_anim_els(r->anim, nullptr, 0);
    if (n <= 0 || !r->tree) {
        return 0;
    }
    std::vector<lxb_dom_element*> act;
    act.resize(static_cast<size_t>(n));
    whaleui_anim_els(r->anim, act.data(), n);
    std::vector<whaleui_layout_node_t*> nodes;
    int changed = 0;
    for (lxb_dom_element* el : act) {
        if (skip && std::find(skip->begin(), skip->end(), el) != skip->end()) {
            continue;
        }
        /* text runs share their element's animatable style but must not
         * have the animation values applied to their own style (a
         * translateY would offset the glyphs inside the already-
         * transformed box - "ghost text" regression). Their opacity is
         * handled by anim_apply_opacity. */
        whaleui_layout_node_t* nd = find_node_by_el(r->tree, el);
        if (!nd || nd->is_text) {
            continue;
        }
        if (whaleui_anim_apply_ov(r->anim, el, nd->style)) {
            changed = 1;
        }
        /* apply_ov wrote interpolated values straight into the style map,
         * behind layout's build: refresh the cached paint flags or an
         * interpolated transform keeps xf=0 and paint_node never
         * translates. Position cannot arrive through a paint-only
         * animation (is_layout_prop), so the fx/sk refresh only keeps the
         * single-writer invariant. */
        whaleui_layout_node_refresh_paint_flags(nd);
        nodes.push_back(nd);
    }
    for (whaleui_layout_node_t* nd : nodes) {
        anim_apply_opacity(nd, 1.0f);
    }
    return changed;
}

extern "C" int whaleui_render_frame(whaleui_render_t* r, whaleui_dom_document_t* doc)
{
    if (!r || !doc) {
        return -1;
    }
    /* the whole frame (state relayout, fill management, DOM relayout,
     * scroll application, paint) touches the layout tree; hold tree_mx
     * from the top so the background fill thread's append batches never
     * interleave with ANY of it (a hover-away state relayout previously
     * ran unlocked before the idle check and raced the fill -> corrupted
     * style maps / intermittent crash on big lists). Idle/parked frames
     * release it immediately (return below). */
    std::unique_lock<std::recursive_mutex> tree_lk(r->tree_mx);
    /* Run mutations the app posted for this thread (see whaleui_dom_post):
     * a caller that must not block hands its edit over instead of waiting
     * for the document lock. They run first so this frame already sees
     * them. */
    whaleui_dom_pump_posted();

    /* The document lock is taken in SHORT SEGMENTS, not for the whole
     * frame. A frame that held it end to end blocked every app-side DOM
     * call for its full duration (39ms at 3200 rows), which reads as a
     * frozen UI. The segments are the phases that actually touch lexbor:
     * the stylesheet re-collect, the dirty drain, the cascade/rebuild and
     * the text measurement. The box pass, the bounds walk and paint work on
     * the already-built layout tree (styles and text are copied into the
     * nodes), so they run without the lock. */
    std::unique_lock<std::recursive_mutex> dom_lk(whaleui_dom_mutex(),
                                                  std::defer_lock);
    dom_lk.lock();

    /* the document's stylesheet sources changed through the DOM API (a <style>
     * injected or rewritten, a <link href> swapped): re-collect before this
     * frame, so the mutation is honoured on the very next repaint like every
     * other DOM edit. Done HERE (not only in the app worker loop) so a direct
     * whaleui_render_frame call - tests, offscreen tools - sees the same
     * behavior as the real loop. The hook is installed by the owning window;
     * it re-parses and calls set_css, which resets the animation state and
     * marks the tree dirty for a full rebuild. */
    if (r->css_reload_fn && whaleui_dom_take_css_dirty(doc)) {
        r->css_reload_fn(r, r->css_reload_ud);
        r->has_dirty = 1; /* the cascade may have changed everywhere */
    }

    /* Release the document lock for the tree-only phases below. The dirty
     * drain and the cascade happen further down; they re-take it. */
    if (dom_lk.owns_lock()) {
        dom_lk.unlock();
    }

    /* advance all running animations and collect the current values (the
     * paint-only fast path below applies them without relaying out) */
    const uint64_t now = SDL_GetTicks();
    const bool animating = r->anim && whaleui_anim_tick(r->anim, now) != 0;
    /* smooth scroll easing (opt-in only): move each live position toward
     * its wheel target in small per-frame steps. This is what decouples
     * wheel events from frame rendering - the event only accumulates the
     * target (whaleui_scroll_smooth_fn), the position here glides toward
     * it, so discrete mouse-wheel notches render like touchpad deltas.
     * Gated on the hook being installed (scroll_fn == smooth fn): the
     * default path never populates scroll_tgt, so without this gate every
     * frame paid a map-emptiness check plus branch cost for a feature that
     * is off. Zero behavior change - the loop body is untouched. */
    if (r->scroll_fn == whaleui_scroll_smooth_fn && !r->scroll_tgt.empty() &&
        r->tree) {
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
    std::vector<int> dom_structural;
    whaleui_dom_take_dirty_typed(doc, dom_dirty, dom_structural);
    /* bump the text version of ONLY the structurally edited elements - those
     * may have changed their text, while a hover/class edit cannot. A global
     * bump here would re-rasterize the whole viewport on every DOM edit, and
     * DOM edits are not rare (animations, editors, dynamic lists). */
    for (size_t di = 0; di < dom_dirty.size() && di < dom_structural.size();
         ++di) {
        if (dom_structural[di]) {
            r->el_ver[dom_dirty[di]] = ++r->el_ver_seq;
        }
    }
    /* interaction-state change (:hover/:active/:focus) on a rule-bearing
     * page: relayout only the previous + current state elements of every
     * pseudo-class state (was a whole-tree rebuild on every change - a
     * 22k-node page froze for seconds per hover). A change that touches no
     * layout property (color/transform/background/underline are the common
     * cases) rebuilds the element's styles and copies the old geometry -
     * no whole-tree box pass (relayout_style). Unified for all states: the
     * per-state difference is only WHICH element entered/left (hover vs
     * focus vs pressed below), the relayout/repaint decision is shared. */
    if (r->state_pending) {
        r->state_pending = 0;
#ifdef WHALEUI_BUILD_FULL
        /* the state relayout below rebuilds subtrees, and when a :hover rule
         * touches a layout key it re-runs the whole-tree box pass. Both
         * measure text through render_text_metric, which needs
         * g_metric_render; that pointer is only set around the full-layout
         * path further down (and cleared after it), so this branch measured
         * with the 0.6em ESTIMATE: hovering an element re-wrapped an h1 from
         * its real 251px to an estimated 231px, changed the flex line and
         * shifted the whole page ("hover moves everything / the flex box grew
         * 20px"). */
        g_metric_render = r;
#endif
        if (r->has_interact_rules && r->rule_count > 0 && r->tree) {
            std::map<std::string, std::string> vars2 = r->theme_vars;
            if (!r->tree->vars_collected && r->tree->root) {
                whaleui_style_collect_vars_full(
                    r->tree->root->el, r->rules, r->rule_count,
                    r->tree->vars);
                r->tree->vars_collected = true;
            }
            for (auto& kv : r->tree->vars) {
                vars2[kv.first] = kv.second;
            }
            /* unified interaction-state relayout: every element that
             * entered OR left a state since the last frame is re-cascaded
             * with the CURRENT state (an old target's :hover/:focus/:active
             * no longer matches, so it reverts through the same path).
             * hover_prev carries the elements crossed within the same
             * frame (fast mouse moves) - without it only the last crossing
             * pair was reverted and intermediate targets kept :hover. */
            std::vector<lxb_dom_element*> targets;
            /* add `e` and its ancestor chain: :hover/:active BUBBLE (see the
             * cascade in style.cpp), so a card with :hover{transform} has to be
             * re-cascaded when the pointer sits on one of its children. The
             * state list below only carries the hit element itself, so the card
             * never received its hover style and never lifted - reported as
             * "moves a bit, and only completes while the mouse keeps sweeping
             * the card". */
            auto add_with_ancestors = [&](lxb_dom_element* e) {
                if (!e) {
                    return;
                }
                targets.push_back(e);
                for (lxb_dom_node* a = e->node.parent; a;
                     a = a->parent) {
                    if (a->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                        targets.push_back(lxb_dom_interface_element(a));
                    }
                }
            };
            for (lxb_dom_element* e : r->hover_prev) {
                add_with_ancestors(e);
            }
            lxb_dom_element* sa[] = {r->hover_old_el, r->hover_el,
                                     r->focus_old_el, r->focus_el,
                                     r->pressed_old_el, r->pressed_el};
            for (lxb_dom_element* e : sa) {
                add_with_ancestors(e);
            }
            /* hover_prev is NOT cleared here: the partial-repaint branch
             * below still needs it to redraw the reverted boxes (cleared
             * together with hover_old_el at the repaint sites). */
            for (lxb_dom_element* el : targets) {
                if (!el ||
                    std::find(dom_dirty.begin(), dom_dirty.end(), el) !=
                        dom_dirty.end()) {
                    continue; /* DOM-dirty element: the DOM path rebuilds it */
                }
                whaleui_layout_node_t* node =
                    find_node_by_el(r->tree, el);
                if (!node) {
                    continue;
                }
                whaleui_style_state st2;
                st2.hover = r->hover_el;
                st2.focus = r->focus_el;
                st2.pressed = r->pressed_el;
                WhaleUIComputedStyle ns = whaleui_style_compute(
                    el, r->rules, r->rule_count, vars2, &st2);
                bool layout_changed = false;
                /* style_compute returns the cascade without the parent
                 * inheritance merge (build does that), so inherited keys
                 * (font-size/line-height/...) are absent from `ns` when no
                 * rule set them - compare only keys the new cascade
                 * actually carries, those are the ones :hover rules wrote */
                for (const char* k : kLayoutKeys) {
                    WhaleUIComputedStyle::const_iterator it = ns.find(k);
                    if (it == ns.end()) {
                        continue;
                    }
                    if (layout_key_differs(k, sget(node->style, k),
                                           it->second)) {
                        layout_changed = true;
                        break;
                    }
                }
                /* stale cached style for this element (keyed on the old
                 * interaction state) would feed relayout's style cache:
                 * erase via the per-element index, O(entries) */
                {
                    auto bit = r->tree->style_cache_by_el.find(el);
                    if (bit != r->tree->style_cache_by_el.end()) {
                        for (size_t bi = 0; bi < bit->second.size(); ++bi) {
                            r->tree->style_cache.erase(bit->second[bi]);
                        }
                        r->tree->style_cache_by_el.erase(bit);
                    }
                }
                if (layout_changed) {
                    dom_dirty.push_back(el); /* full relayout + box pass */
                    /* the box pass can move a different element under the
                     * (stationary) pointer: re-hit-test before paint */
                    r->hover_recheck = 1;
                } else {
                    /* the state move changed no style on this element (no
                     * :hover/:active/:focus rule selects it - e.g. the
                     * body when the mouse leaves a link): rebuilding it is
                     * pure waste (body on a 34k-node page = seconds).
                     * Compare the new cascade against the current style on
                     * the cascade's own keys (the state rules' outputs)
                     * and skip when identical. */
                    bool cascade_changed = false;
                    for (auto& kv : ns) {
                        auto sit = node->style.find(kv.first);
                        if (sit == node->style.end() ||
                            sit->second != kv.second) {
                            cascade_changed = true;
                            break;
                        }
                    }
                    /* a state change on an element whose cascade did NOT
                     * change must still repaint the PRESS/FOCUS pair: the
                     * state pass compares per-element cascades, so a no-op
                     * element is skipped - but the partial-repaint branch
                     * keys off hover/focus/pressed_old markers, and those
                     * are the ONLY thing that repaints the CHANGED element.
                     * Pin them whenever this target is a state endpoint,
                     * even when its own cascade is identical. Without this
                     * the changed element's damage was never recorded and
                     * the frame painted the OLD pixels (the "flash": press
                     * shows :active, release repaints nothing until the
                     * next motion). */
                    bool is_endpoint = el == r->hover_el ||
                                       el == r->hover_old_el ||
                                       el == r->focus_el ||
                                       el == r->focus_old_el ||
                                       el == r->pressed_el ||
                                       el == r->pressed_old_el;
                    if (is_endpoint) {
                        if (el == r->hover_el && !r->hover_old_el) {
                            r->hover_old_el = el;
                        }
                        if (el == r->focus_el && !r->focus_old_el) {
                            r->focus_old_el = el;
                        }
                        if (el == r->pressed_el && !r->pressed_old_el) {
                            r->pressed_old_el = el;
                        }
                    }
                    if (!cascade_changed) {
                        continue;
                    }
                    whaleui_layout_relayout_style(
                        r->tree, el, r->rules, r->rule_count,
                        &r->theme_vars, &st2, &r->scrolls, r->anim,
                        r->text_scale);
                    /* The style-only pass leaves dom_dirty/has_dirty empty,
                     * which would hit the frame's idle early-return and
                     * never repaint the hovered box. hover_old_el (kept set
                     * here, or pinned to the hovered element on a first
                     * hover) drives the frame's partial repaint of the
                     * changed box(es). The rebuilt subtree's nodes carry
                     * zeroed paint bounds (only box/bounds passes fill
                     * them): mark the path dirty and recompute from the
                     * ROOT once below - the walk early-returns on clean
                     * nodes, so only the marked path is visited.
                     * Recomputing just the subtree is NOT enough: the
                     * relayout re-chunks the parent (fresh chunk objects
                     * with zeroed boxes), and the parent's merged bounds /
                     * chunk boxes only refresh on a root-anchored walk -
                     * otherwise the chunk cull uses zero boxes and the hit
                     * test misses the rebuilt element ("hover works on the
                     * first frame, misses from the second"). */
                    {
                        whaleui_layout_node_t* fnode =
                            find_node_by_el(r->tree, el);
                        if (fnode) {
                            mark_bounds_dirty(fnode);
                        }
                    }
                    /* a first hover (no previous target) still needs the
                     * partial-repaint machinery: pin the hovered element.
                     * Same for focus/pressed endpoints (their old markers
                     * may have been consumed by an earlier frame while the
                     * new endpoint still needs its own repaint). */
                    if (el == r->hover_el && !r->hover_old_el) {
                        r->hover_old_el = el;
                    }
                    if (el == r->focus_el && !r->focus_old_el) {
                        r->focus_old_el = el;
                    }
                    if (el == r->pressed_el && !r->pressed_old_el) {
                        r->pressed_old_el = el;
                    }
                    /* the style-only relayout changed this element: its box
                     * must be repainted this frame. hover_el/hover_old_el
                     * already drive the partial repaint below; hover_prev
                     * elements (crossed within the frame) are checked by
                     * the same partial branch and cleared there. */
                }
            }
            /* the style-only relayouts above re-chunked their parents: run
             * one root-anchored bounds pass for all of them (early-returns
             * on clean subtrees; a whole-tree walk cost ~40ms per hover
             * before the bd bits, now only the marked paths). Without this
             * the parents' merged bounds / chunk boxes stay zeroed and the
             * chunk cull + hit test miss the rebuilt elements. */
            if (r->tree && r->tree->root) {
                compute_paint_bounds(r->tree->root);
                r->bounds_valid = 1;
            }
        }
#ifdef WHALEUI_BUILD_FULL
        g_metric_render = nullptr; /* state layout done; paint re-rasterizes */
#endif
    }

    /* async first layout (opt-in): no tree yet -> build on a worker so a
     * large page does not freeze the window while it lays out; the frame
     * loop picks up the finished tree. Only the FIRST layout is async -
     * later full rebuilds stay synchronous. The worker reads the DOM
     * (no user interaction exists before the first paint, so no concurrent
     * DOM write) and an anim=NULL layout (animation state is created by
     * the following synchronous frames). */
    /* A SLICED first layout (r->tree->resumes non-empty) is already on screen:
     * finish the full build on a worker so the page catches up in the
     * background. The old behaviour (no tree yet -> build everything on a
     * worker) is gone: the first layout is now synchronous but budgeted, so
     * the window shows the viewport instead of a blank clear colour. */
    if (r->async_layout && r->tree && !r->tree->resumes.empty() &&
        !r->layout_thread && doc) {
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
                r->layout_text_scale, 0);
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
            mark_bounds_dirty_all(r->tree->root);
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
    /* progressive <details> expand - BACKGROUND fill thread. The first
     * frame's relayout (below, budgeted via tree->pending_budget) built
     * the viewport rows; a dedicated thread appends the remaining rows in
     * batches (append_rows under tree_mx), so the render frame never
     * blocks on a 20-30ms layout batch. Interaction frames stay short
     * (input + local repaint only); the fill runs between them (idle-time
     * filling - "full render" is just a signal) and at full speed while
     * the frame loop is parked. fill_dirty wakes a repaint per batch. */
    if (r->stream_expand.active && r->tree && r->tree->root &&
        r->stream_expand.list && !r->fill_running.load() &&
        r->fill_thread == nullptr &&
        r->tree->by_el.count(r->stream_expand.list) != 0) {
        /* the first frame's relayout has landed (the list is in the tree):
         * start appending the remaining rows in the background */
        r->fill_stop.store(0);
        r->fill_finished.store(0);
        r->fill_thread = new std::thread(fill_worker_fn, r);
    }
    if (r->fill_thread && r->fill_finished.load()) {
        /* the fill thread exited: reap it and finish the expand */
        r->fill_thread->join();
        delete r->fill_thread;
        r->fill_thread = nullptr;
        r->fill_dirty.store(1); /* show the last landed rows */
        if (r->stream_expand.active) {
            bool exhausted = false;
            auto fnd = r->tree->by_el.find(r->stream_expand.list);
            if (fnd != r->tree->by_el.end()) {
                size_t laid = 0;
                for (whaleui_layout_node_t* c = fnd->second->first_child;
                     c; c = c->next) {
                    if (c->el && !c->is_text) {
                        ++laid;
                    }
                }
                size_t dom = 0;
                for (lxb_dom_node* nd =
                         r->stream_expand.list->node.first_child;
                     nd; nd = nd->next) {
                    if (nd->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                        ++dom;
                    }
                }
                exhausted = laid == dom;
            }
            r->stream_expand.active = false;
            r->stream_expand.det = nullptr;
            r->stream_expand.list = nullptr;
            if (!exhausted) {
                /* it stopped early (tree rebuilt mid-fill / append
                 * blocked): finish with a full rebuild */
                r->has_dirty = 1;
            }
        }
    }
    if (r->fill_dirty.load()) {
        r->fill_dirty.store(0);
        r->bounds_valid = 0; /* new rows landed / content shifted */
        mark_bounds_dirty_all(r->tree->root);
        r->scroll_dirty = 1; /* repaint only (no whole-tree relayout) */
        r->alive = 1;
    }
    /* skip the whole frame when nothing changed: idle frames cost ~0.
     * Repaint when the layout/state is dirty, a wheel scroll happened, an
     * animation/transition is running, an editable caret is blinking, the
     * DOM was mutated, or a hover change is waiting for its partial
     * repaint (hover_old_el set by set_hover / the state-relayout pass).
     * A focused caret does NOT force a repaint by itself: only a blink
     * PHASE flip does (caret_dirty below) - otherwise an idle focused
     * input burns a full repaint every frame at the refresh rate. */
    bool caret_dirty = false;
    if (r->edit_el) {
        caret_dirty = caret_blink_phase() != r->caret_phase_painted;
    }
    if (dom_lk.owns_lock()) {
        dom_lk.unlock(); /* nothing below this point touches lexbor */
    }
    if (!r->has_dirty && r->tree && !r->scroll_dirty &&
        !animating && !caret_dirty && !r->hover_old_el &&
        !r->focus_old_el && !r->pressed_old_el && r->hover_prev.empty() &&
        !r->font_scale_pending && !r->fill_dirty.load() &&
        dom_dirty.empty() && !r->hover_recheck) {
        if (r->edit_el) {
            /* focused but visually unchanged (same blink phase): park the
             * paint, keep the loop ticking cheaply so the next phase flip
             * repaints */
            r->alive = 1;
        } else if (r->fill_running.load()) {
            /* background fill active: cheap poll frames (no paint) so a
             * batch's fill_dirty wakes a repaint without needing input */
            r->alive = 1;
        } else {
            r->alive = 0; /* static page: the app loop can park idle */
        }
        return 0;
    }
    /* a scroll moved the content under a (possibly stationary) pointer:
     * the frame re-hit-tests before painting. Snapshot the scroll decision
     * FIRST: scroll_dirty is consumed (zeroed) below, but the paint path
     * needs to know this frame is a scroll frame even when the position
     * update itself produced no delta (same-position wheel already handled
     * by coalescing). */
    const bool want_scroll_frame = r->scroll_dirty != 0;
    if (r->scroll_dirty) {
        r->hover_recheck = 1;
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
    /* global font-scale change (+/-): apply IN PLACE on the existing tree
     * (multiply font-size values, reflow geometry) - the stylesheet did
     * not change, so a whole-tree rebuild (re-cascade 34k elements, ~5s)
     * would be pure waste. */
    if (r->font_scale_pending && r->tree && r->tree->root) {
        r->font_scale_pending = 0;
        float cur = r->text_scale > 0.0f ? r->text_scale : 1.0f;
        float prev = r->font_scale_applied > 0.0f ? r->font_scale_applied
                                                  : 1.0f;
        if (cur != prev) {
            whaleui_layout_scale_fonts(r->tree, cur / prev);
            whaleui_layout_relayout_geometry(r->tree, r->fb_w, r->fb_h);
            fix_run_heights(r);
            fix_scroll_max(r);
            r->bounds_valid = 0; /* every box moved */
            mark_bounds_dirty_all(r->tree->root);
            r->drag_scroll_node = nullptr;
            r->scroll_max_el = nullptr;
            r->wheel_node = nullptr;
            r->hover_recheck = 1; /* every box moved under the pointer */
            r->has_dirty = 0; /* the in-place pass already reflowed */
            r->scroll_dirty = 1;
            r->alive = 1;
        }
        r->font_scale_applied = cur;
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
    /* did the tick's interpolated values actually differ from the styles
     * this frame (set by the paint-only apply path)? 0 = every animated
     * value is unchanged (held delay, finished transition): the animation
     * carries no visual damage and the frame can skip its strip - the
     * below-fold skip below then parks the paint entirely. Initialized
     * from the tick itself: when the tick reports NO running animation at
     * all, values trivially did not change (0); a running tick sets 1 and
     * the paint-only apply path may clear it back when every value lands
     * identical. */
    int anim_values_changed = animating ? 1 : 0;
    bool geom_ok = false;
    if (need_layout) {
        /* plain resize (no DOM/rule change - geometry_only set by
         * render_resize, cleared by set_css): re-lay-out the EXISTING tree
         * for the new viewport. Styles/runs are unchanged, so this skips
         * the style cascade + rebuild entirely - a 34k-node page resizes
         * in tens of ms instead of seconds (and the window drag stays
         * live). The hovered/focused/pressed styles already live on the
         * tree (they were applied locally after the last rebuild) and
         * survive a geometry-only pass untouched. */
        if (r->geometry_only && r->tree && r->tree->root) {
            r->geometry_only = 0;
            if (whaleui_layout_relayout_geometry(r->tree, r->fb_w,
                                                 r->fb_h) == 0) {
                r->has_dirty = 0;
                g_last_tree = r->tree;
                r->bounds_valid = 0; /* every box moved */
                mark_bounds_dirty_all(r->tree->root);
                r->drag_scroll_node = nullptr;
                r->scroll_max_el = nullptr;
                r->wheel_node = nullptr;
                fix_run_heights(r);
                fix_scroll_max(r);
                r->hover_recheck = 1; /* every box moved under the pointer */
                geom_ok = true;
            }
            /* relayout failed: fall through to the full rebuild */
        }
        if (!geom_ok) {
        /* full rebuild covers every interaction state: drop the pending
         * partial-repaint markers so they cannot stall the idle check */
        r->hover_old_el = nullptr;
        r->focus_old_el = nullptr;
        r->pressed_old_el = nullptr;
        r->hover_prev.clear();
        /* Was there no tree at all (first layout) vs a rebuild? Must be read
         * BEFORE the destroy below - after it `r->tree` is always null, so
         * the slice decision applied to every rebuild as well and a resized
         * page was cut to one viewport's worth of rows (content missing
         * after a resize). The slice decision applies only to the FIRST
         * layout: a rebuild must stay whole, or the page would lose
         * content. */
        const bool wui_first_layout = (r->tree == nullptr);
        whaleui_layout_destroy(r->tree);
        r->tree = nullptr;
        /* Build with an interaction-NEUTRAL state so every element hits the
         * style cache (keyed per element + state): the cascade does not
         * depend on which element is hovered, so a whole-tree rebuild after
         * a RESIZE (rules unchanged) must not re-cascade all 34k elements
         * just because the mouse sits over one of them (froze mid-drag ~3s).
         * The hovered/focused/pressed elements get their state styles
         * re-applied right after (local relayout_style, geometry copied). */
        lxb_dom_element* sa[6] = {r->hover_old_el, r->hover_el,
                                  r->focus_old_el, r->focus_el,
                                  r->pressed_old_el, r->pressed_el};
        whaleui_style_state st0;
        /* First layout of a large page: build only the viewport's worth and
         * let the worker above catch up (r->tree->resumes stays non-empty,
         * which is what marks the tree as sliced). */
        size_t cut_rows = 0;
        if (r->async_layout && wui_first_layout && r->fb_h > 0) {
            cut_rows = static_cast<size_t>(r->fb_h / 25) + 8;
        }
        r->tree = whaleui_layout_compute(doc, r->rules, r->rule_count,
                                         &r->theme_vars, r->fb_w, r->fb_h,
                                         &st0, &r->scrolls, r->anim,
                                         r->text_scale, cut_rows);
        g_last_tree = r->tree; /* DOM geometry queries see this frame */
        r->has_dirty = 0;
        for (int si = 0; si < 6; ++si) {
            if (!sa[si]) {
                continue;
            }
            /* :focus-within bubbles like :hover (see add_with_ancestors
             * above): the focus element's ancestors must re-cascade too,
             * or a form's :focus-within rule never applies after click.
             * Dedup via a small visited set (the 6 slots overlap). */
            lxb_dom_element* chain[6] = {sa[si], nullptr, nullptr,
                                         nullptr, nullptr, nullptr};
            int nc = 1;
            if (sa[si] == r->focus_el || sa[si] == r->focus_old_el) {
                for (lxb_dom_node* a = sa[si]->node.parent; a && nc < 6;
                     a = a->parent) {
                    if (a->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                        chain[nc++] = lxb_dom_interface_element(a);
                    }
                }
            }
            for (int ci = 0; ci < nc; ++ci) {
                if (!find_node_by_el(r->tree, chain[ci])) {
                    continue;
                }
                whaleui_style_state stx;
                stx.hover = r->hover_el;
                stx.focus = r->focus_el;
                stx.pressed = r->pressed_el;
                whaleui_layout_relayout_style(r->tree, chain[ci], r->rules,
                                              r->rule_count, &r->theme_vars,
                                              &stx, &r->scrolls, r->anim,
                                              r->text_scale);
            }
        }
        r->bounds_valid = 0; /* subtree paint bounds are stale */
        mark_bounds_dirty_all(r->tree->root);
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
        /* fresh tree: whatever sits under the (stationary) pointer now is
         * a fresh question - re-hit-test before painting */
        r->hover_recheck = 1;
#ifdef WHALEUI_BUILD_FULL
        g_metric_render = nullptr; /* layout done; paint re-rasterizes */
#endif
        }
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
        {
            /* ONLY elements whose running animation touches a layout
             * property: relayout_multi reflows their flex/grid
             * ancestors. A paint-only transition (the UA
             * `select { transition: border-color }` firing on :hover)
             * must not come through here - reflowing its flex ancestor
             * (the page header) recomputed the container height and
             * shifted every centered child, and the shift persisted
             * ("hover lifted the header row by a few px and it stayed
             * there until a full repaint").
             * Enumerated from the tick's act table via by_el - O(animated)
             * instead of a whole-tree walk per frame. */
            int n = whaleui_anim_els(r->anim, nullptr, 0);
            std::vector<lxb_dom_element*> act;
            if (n > 0) {
                act.resize(static_cast<size_t>(n));
                whaleui_anim_els(r->anim, act.data(), n);
            }
            for (lxb_dom_element* el : act) {
                if (whaleui_anim_has_layout_el(r->anim, el) &&
                    find_node_by_el(r->tree, el)) {
                    anim_els.push_back(el);
                }
            }
        }
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
            /* the animated subtrees were rebuilt (rechunk_parent already
             * marked their ancestor paths - see rechunk_parent): only those
             * paths need a bounds recompute. compute_paint_bounds early-
             * returns on clean nodes, so a root-anchored walk visits just
             * the marked paths instead of the whole tree (mark_all +
             * full compute visited every node per animation frame). */
            r->bounds_valid = 0; /* recomputed from the marked paths below */
            compute_paint_bounds(r->tree->root);
            r->bounds_valid = 1;
            r->drag_scroll_node = nullptr; /* nodes were recreated */
            r->scroll_max_el = nullptr;    /* scroll_max may have changed */
            r->wheel_node = nullptr;       /* hit cache is stale */
            /* the box pass re-estimated every text run's height from its
             * wrap width; the full rebuild path corrects those to the
             * renderer's per-glyph wrap (fix_run_heights below), but this
             * animation path did not - so a run next to a wide sibling
             * (checkbox input + text in one <p>) kept a 2-line estimate
             * (32px) while full frames painted it at 19px. The run
             * shrank/grew every other frame -> the checkbox label visibly
             * jumped  while a width animation ran. Same
             * correction as the full-rebuild tail, so both paths agree. */
            fix_run_heights(r);
            /* ponytail: skip the full-tree fix_scroll_max walk on layout-
             * animation frames. A width/height animation inside a scroll
             * container is rare, and the range corrects on the next full
             * relayout (scroll/hover/DOM/edit); the walk is per-frame
             * baseline cost otherwise. */
            /* the build applied animated styles only INSIDE the rebuilt
             * subtrees: paint-only animated elements elsewhere (a hover
             * transform transition concurrent with the width bar) still
             * need this frame's interpolated values - see
             * apply_paint_only_anim for the composition rationale. */
            anim_values_changed |=
                apply_paint_only_anim(r, &anim_els);
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
         * animation cost ms at 2k. The animated set comes straight from
         * the tick's act table resolved through by_el: O(animated) per
         * frame, zero tree walk. */
        anim_values_changed |= apply_paint_only_anim(r, nullptr);
    }
    /* DOM mutations: incremental relayout. Only the affected subtrees get
     * a fresh style cascade + build; the box pass re-positions the tree
     * while untouched branches keep their computed styles. Falls back to a
     * full rebuild (has_dirty) when the tree is inconsistent. */
    /* DOM edits that only touch paint (attribute/style changes whose
     * layout keys are unchanged) rebuild just the element's styles and
     * partial-repaint its box; structural edits or layout-key changes must
     * re-run the box pass and repaint widely (dom_geom_changed). */
    bool dom_geom_changed = false;
    std::vector<lxb_dom_element*> dom_repaint;
    if (!need_layout && !dom_dirty.empty()) {
#ifdef WHALEUI_BUILD_FULL
        /* the incremental relayouts in this branch (append_rows /
         * relayout_style / relayout) rebuild subtrees and run box passes:
         * without the metric context they measured text with the 0.6em
         * estimate and re-wrapped runs (real 251px -> estimated 231px), which
         * is what shifted the layout on a hover or a click. The matching
         * clear sits at the end of this branch. */
        g_metric_render = r;
#endif
        /* DOM mutated. Invalidate ONLY the mutated elements' cached
         * computed styles: untouched branches keep theirs (the cascade for
         * them is unchanged). The vars collection also stays - re-run only
         * when a style-attribute edit touched it (whaleui_dom_mark_vars_dirty,
         * taken below); open/class/other-attribute mutations cannot change
         * var() definitions. Clearing the whole cache here made an
         * incremental <details> expand (6.8k new nodes) re-cascade every
         * element - a ~31s frame instead of the affected subtree only. */
        if (r->tree) {
            for (lxb_dom_element* el : dom_dirty) {
                if (!el) {
                    continue;
                }
                auto bit = r->tree->style_cache_by_el.find(el);
                if (bit != r->tree->style_cache_by_el.end()) {
                    for (size_t bi = 0; bi < bit->second.size(); ++bi) {
                        r->tree->style_cache.erase(bit->second[bi]);
                    }
                    r->tree->style_cache_by_el.erase(bit);
                }
            }
            if (whaleui_dom_take_vars_dirty()) {
                r->tree->vars.clear();
                r->tree->vars_collected = false;
            }
        }
        if (!dom_lk.owns_lock()) {
            dom_lk.lock(); /* cascade + rebuild read lexbor */
        }
        const bool struct_dirty = whaleui_dom_take_struct_dirty() != 0;
        bool ok = true;
        whaleui_style_state st;
        st.hover = r->hover_el;
        st.focus = r->focus_el;
        st.pressed = r->pressed_el;
        /* merged vars for the layout-key pre-check (same merge as the
         * interaction-state path above) */
        std::map<std::string, std::string> vars2 = r->theme_vars;
        if (r->tree && !r->tree->vars_collected && r->tree->root) {
            whaleui_style_collect_vars_full(r->tree->root->el, r->rules,
                                            r->rule_count, r->tree->vars);
            r->tree->vars_collected = true;
        }
        for (auto& kv : r->tree->vars) {
            vars2[kv.first] = kv.second;
        }
        std::vector<lxb_dom_element*> full_list;
        for (lxb_dom_element* el : dom_dirty) {
            if (!el) {
                continue;
            }
            if (struct_dirty) {
                /* structural edit (children/text shape changed): the
                 * subtree must be rebuilt and geometry may shift - the
                 * full relayout path below. A list whose TAIL grew by new
                 * <li> rows is the one structural edit that does NOT need
                 * the old rows rebuilt: whaleui_layout_append_rows builds
                 * only the new rows and shifts what follows (seconds on a
                 * 10k-row list -> the new rows only). */
                if (whaleui_layout_append_rows(
                        r->tree, el, r->rules, r->rule_count,
                        &r->theme_vars, &st, &r->scrolls, r->anim,
                        r->text_scale,
                        std::numeric_limits<size_t>::max()) > 0) {
                    /* incremental: geometry grew, no subtree rebuild;
                     * wide repaint below covers it */
                    r->hover_recheck = 1;
                    continue;
                }
                /* a freshly created element (li built by append_rows on its
                 * parent) has no laid-out subtree yet - rebuilding it alone
                 * is pointless (its parent's rebuild covers it) */
                if (!find_node_by_el(r->tree, el)) {
                    continue;
                }
                full_list.push_back(el);
                continue;
            }
            whaleui_layout_node_t* node = find_node_by_el(r->tree, el);
            if (!node) {
                full_list.push_back(el);
                continue;
            }
            /* attribute/style edit: re-cascade and diff only the layout
             * keys - unchanged means paint-only (color/transform/...):
             * rebuild styles + copy geometry, no whole-tree box pass */
            WhaleUIComputedStyle ns = whaleui_style_compute(
                el, r->rules, r->rule_count, vars2, &st);
            bool lay_changed = false;
            for (const char* k : kLayoutKeys) {
                WhaleUIComputedStyle::const_iterator it = ns.find(k);
                if (it == ns.end()) {
                    continue; /* inherited-only key: not set by the edit */
                }
                if (layout_key_differs(k, sget(node->style, k),
                                       it->second)) {
                    lay_changed = true;
                    break;
                }
            }
            /* A descendant-selector rule (`.box.big .row`, `.card:hover
             * .acts`) makes the edit change the computed style of elements
             * BELOW `el`, not of `el` itself. The diff above only looks at
             * the edited element, so such an edit took the paint-only
             * branch: descendants kept the style they had at load time and
             * the screen showed the old layout until some unrelated
             * interaction (a hover) happened to re-cascade them. Rebuild the
             * whole subtree when the sheet has any rule whose ancestor part
             * matches this element. */
            if (r->ancestor_prefixes_rules != r->rules ||
                r->ancestor_prefixes_count != r->rule_count) {
                r->ancestor_prefixes.clear();
                for (size_t ri = 0; ri < r->rule_count; ++ri) {
                    if (!r->rules[ri].selector) {
                        continue;
                    }
                    std::string pf =
                        whaleui_style_ancestor_prefix(r->rules[ri].selector);
                    if (!pf.empty()) {
                        r->ancestor_prefixes.push_back(pf);
                    }
                }
                r->ancestor_prefixes_rules = r->rules;
                r->ancestor_prefixes_count = r->rule_count;
            }
            bool desc_affected = false;
            for (size_t pi = 0; pi < r->ancestor_prefixes.size() &&
                                !desc_affected;
                 ++pi) {
                if (whaleui_style_match(r->ancestor_prefixes[pi].c_str(), el,
                                        &st)) {
                    desc_affected = true;
                }
            }
            if (desc_affected && r->tree) {
                /* the rebuilt subtree re-cascades through the layout style
                 * cache (build() consults it whenever an animation context
                 * is present), so every descendant's cached entry must go -
                 * otherwise `.box.big .row` still resolves to the height
                 * computed before the class changed. */
                std::function<void(lxb_dom_node*)> drop_cache =
                    [&](lxb_dom_node* dn) {
                        if (!dn) {
                            return;
                        }
                        if (dn->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                            auto bit = r->tree->style_cache_by_el.find(
                                lxb_dom_interface_element(dn));
                            if (bit != r->tree->style_cache_by_el.end()) {
                                for (size_t bi = 0; bi < bit->second.size();
                                     ++bi) {
                                    r->tree->style_cache.erase(
                                        bit->second[bi]);
                                }
                                r->tree->style_cache_by_el.erase(bit);
                            }
                        }
                        for (lxb_dom_node* c = dn->first_child; c;
                             c = c->next) {
                            drop_cache(c);
                        }
                    };
                drop_cache(&el->node);
            }
            if (lay_changed || desc_affected) {
                full_list.push_back(el);
            } else {
                if (whaleui_layout_relayout_style(
                        r->tree, el, r->rules, r->rule_count,
                        &r->theme_vars, &st, &r->scrolls, r->anim,
                        r->text_scale) < 0) {
                    full_list.push_back(el);
                } else {
                    dom_repaint.push_back(el);
                }
            }
        }
        /* Rebuild every changed element, then run the box pass ONCE.
         *
         * Calling whaleui_layout_relayout per element ran a WHOLE-TREE box
         * pass each time (relayout_impl's tail: b.layout(tree->root, ...)),
         * so an app that sets sizes on N elements in one frame paid N full
         * layout passes - the reported "performance collapses as soon as the
         * page is driven from C++". relayout_multi rebuilds the same
         * subtrees but re-lays out only their topmost flex/grid ancestors
         * (or the element itself in a pure block chain), deduplicated. */
        {
            std::vector<lxb_dom_element*> batch;
            batch.reserve(full_list.size());
            for (lxb_dom_element* el : full_list) {
                /* only elements the tree knows about: a brand-new node is
                 * covered by its parent's rebuild */
                if (el && find_node_by_el(r->tree, el)) {
                    batch.push_back(el);
                }
            }
            if (!batch.empty() &&
                whaleui_layout_relayout_multi(
                    r->tree, batch.data(), batch.size(), r->rules,
                    r->rule_count, &r->theme_vars, &st, &r->scrolls, r->anim,
                    r->text_scale) < 0) {
                ok = false;
            }
        }
        /* the cascade/rebuild above is the last lexbor touch of this
         * frame: release the document lock so app-side DOM calls stop
         * waiting while the box pass and paint run */
        if (dom_lk.owns_lock()) {
            dom_lk.unlock();
        }
        dom_geom_changed = !full_list.empty() || struct_dirty;
        if (dom_geom_changed) {
            /* geometry moved: the pointer may be over a different element */
            r->hover_recheck = 1;
        }
        /* an animation running on top of the edits: the paint paths do not
         * compose a DOM strip with the anim strip - fall back to full */
        if (animating) {
            dom_geom_changed = true;
            dom_repaint.clear();
        }
        if (ok) {
            g_last_tree = r->tree;
            if (!dom_geom_changed) {
                /* paint-only edits: recompute the changed subtrees' bounds
                 * locally (ancestors unchanged -> still valid); paint
                 * partial-repaints their boxes (dom_repaint) */
                /* paint-only edits: mark the edited subtrees dirty (the mark
                 * bubbles to the root) and recompute from the ROOT - the walk
                 * early-returns on unmarked nodes, so only the marked paths are
                 * visited. Recomputing just 
d here leaves its ancestors'
                 * merged bounds stale and the cull then uses stale boxes. */
                for (lxb_dom_element* el : dom_repaint) {
                    whaleui_layout_node_t* nd =
                        find_node_by_el(r->tree, el);
                    if (nd) {
                        mark_bounds_dirty(nd);
                    }
                }
                compute_paint_bounds(r->tree->root);
                r->bounds_valid = 1;
            } else {
                r->bounds_valid = 0; /* subtree paint bounds are stale */
                mark_bounds_dirty_all(r->tree->root);
                /* geometry changed tree-wide: a stale interaction-state
                 * partial repaint (hover_old_el etc.) would redraw only a
                 * small strip and leave the shifted/collapsed areas frozen
                 * until the next mouse move - full repaint instead */
                r->hover_old_el = nullptr;
                r->focus_old_el = nullptr;
                r->pressed_old_el = nullptr;
                r->hover_prev.clear();
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
            }
            r->drag_scroll_node = nullptr; /* nodes were recreated */
            r->scroll_max_el = nullptr;    /* scroll_max may have changed */
            r->wheel_node = nullptr;       /* hit cache is stale */
#ifdef WHALEUI_BUILD_FULL
            g_metric_render = nullptr; /* layout done; paint re-rasterizes */
#endif
        } else {
            r->has_dirty = 1; /* tree inconsistent: full rebuild */
            dom_repaint.clear();
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
    const int scroll_dy = 0;

    /* paint: collect batched GPU draw commands. Text goes to the CPU layer
     * (moved on scroll), uploaded + composited by a compute pass. */
    Clip full = {0, 0, r->fb_w, r->fb_h};
    /* empty strip: dirty_rect GROWS it, so partial branches (hover /
     * animation / embedded scroll) shrink the repaint to their boxes. A
     * strip starting at `full` never shrinks - dirty_rect only unions -
     * and every "partial" frame repainted the whole window (the animation
     * frame-drop report). */
    Clip strip = {0, 0, 0, 0};
    bool partial = false; /* load-only repaint of a dirty region */
    /* partial strips (hover / animation) are computed from node bounds -
     * compute them BEFORE the strip math. After a relayout bounds_valid
     * is 0 and every bound reads (0,0,0,0), so the hover strip came out
     * empty and a hover change repainted nothing (the relayout had applied
     * the style, the paint just never covered the box). */
    if (r->tree && r->tree->geom_relaid) {
        /* a style-only/local relayout fell back to a whole-tree box pass
         * (relayout_impl set this): every node's geometry was recomputed,
         * so paint bounds AND the geometry-dependent caches are stale.
         * Without this the partial-repaint strips (hover / animation)
         * were computed from old bounds and repainted the wrong boxes -
         * hover changes on pages whose text re-flows differently could
         * leave elements painted at a shifted spot that no later frame
         * corrected until a full repaint ("hover lifts, stays, click
         * snaps back"). */
        r->tree->geom_relaid = 0;
        r->bounds_valid = 0;
        mark_bounds_dirty_all(r->tree->root);
        r->drag_scroll_node = nullptr;
        r->scroll_max_el = nullptr;
        r->wheel_node = nullptr;
    }
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
     * not move while dragging the scrollbar").
     * scroll_el is ALSO the repaint signal for non-scroll events (select
     * open/close/pick changes nothing scrollable): only a REAL scroll
     * position change takes the scroll_frame path. A signal-only frame
     * falls through so hover/focus/pressed damage accumulates normally -
     * treating every signal as a page scroll cleared those markers and
     * the state repaint never happened ("clicking flashes / hover
     * sticks"). */
    bool scrolled = false;
    if (r->scroll_el && want_scroll_frame) {
        /* the scroll position actually moved this frame (wheel/drag set
         * scroll_dirty alongside scroll_el): a REAL scroll takes the
         * scroll_frame path. A bare scroll_el without scroll_dirty is a
         * repaint-only signal (select open/close/pick) - it falls through
         * so hover/focus/pressed damage accumulates normally. scroll_dirty
         * was snapshotted into want_scroll_frame at the frame top (it is
         * already consumed/zeroed): testing r->scroll_dirty here would
         * NEVER be true and no scroll frame would ever repaint. */
        scrolled = true;
    }
    const bool scroll_frame = scrolled || r->drag_scroll_el;
    if (scroll_frame) {
        r->scroll_el = nullptr;
        /* a real scroll repaints the WHOLE viewport at the new offset, so
         * no partial strip is needed - but hover/focus/pressed markers are
         * kept: the partial branch below accumulates their (live,
         * post-scroll) boxes into the strip, and a full-viewport paint
         * covers the rest. Clearing them here dropped the hovered card's
         * rows from the repaint ("scrolling smears the hovered element").
         * The animation branch below is still skipped on scroll frames
         * (its strip would shrink the paint to the animated boxes only). */
    } else if (r->scroll_el) {
        /* repaint-only signal (select open/close/pick): the content did
         * NOT scroll, so keep the state markers - but the select popup
         * paints outside the tree, so force a full-viewport paint for
         * this frame (the popup region has no other damage source). */
        r->scroll_el = nullptr;
    } else if (!r->open_select &&
               (r->hover_old_el || r->focus_old_el ||
                r->pressed_old_el || !r->hover_prev.empty() ||
                !dom_repaint.empty())) {
        /* interaction-state / paint-only DOM edits: repaint only the old +
         * new state elements and the changed DOM elements (their styles
         * changed; the relayout already re-cascaded, geometry is stable).
         * NOT gated on edit_el: a focused input's hover/selection changes
         * must still repaint its box - the caret gate below only skips
         * fully unchanged phases, and caret_dirty forces a paint then.
         * NOT gated on !animating: an animation running while the mouse
         * moves must still repaint the hovered box - the animation branch
         * below accumulates its boxes into the SAME strip.
         * The damage set IS the (small) element list, so resolve each
         * through by_el instead of walking the whole tree testing every
         * node against the set - O(targets) instead of O(tree) per hover
         * frame on a 34k-node page. */
        strip_element(r, r->hover_old_el, &strip);
        strip_element(r, r->hover_el, &strip);
        strip_element(r, r->focus_old_el, &strip);
        strip_element(r, r->focus_el, &strip);
        strip_element(r, r->pressed_old_el, &strip);
        strip_element(r, r->pressed_el, &strip);
        for (lxb_dom_element* e : r->hover_prev) {
            strip_element(r, e, &strip);
        }
        for (lxb_dom_element* e : dom_repaint) {
            strip_element(r, e, &strip);
        }
        if (strip.w > 0 && strip.h > 0) {
            partial = true;
        }
        r->hover_old_el = nullptr;
        r->focus_old_el = nullptr;
        r->pressed_old_el = nullptr;
        r->hover_prev.clear();
        dom_repaint.clear();
    }
    /* hover re-check: the relayout/scroll this frame performed can have
     * moved a DIFFERENT element under the (stationary) pointer. Without
     * this that element only receives :hover on the next 1px of mouse
     * movement - "hover dies after scrolling / after a hover-driven layout
     * change settles". Runs after the strip branches above so their marker
     * consumption is done; a changed target re-enters the normal state
     * path next frame (state_pending) while this frame repaints both
     * boxes. */
    if (r->hover_recheck) {
        r->hover_recheck = 0;
        if (r->mouse_has && r->tree && r->tree->root &&
            !r->drag_scroll_el && !r->open_select && !r->pressed_el) {
            lxb_dom_element* was = r->hover_el;
            whaleui_render_set_hover(r, static_cast<int>(r->mouse_wx),
                                     static_cast<int>(r->mouse_wy));
            if (r->hover_el != was) {
                strip_element(r, was, &strip);
                strip_element(r, r->hover_el, &strip);
            }
        }
    }
    /* animation damage accumulates into the same strip, but only when the
     * frame is NOT a scroll frame (scroll repaints the whole viewport -
     * an animation strip would shrink it to the animated boxes and leave
     * the scrolled content unrepainted - "scrolling leaves trails / the
     * page freezes mid-scroll on animated pages"). Rendering behavior is
     * independent of whether an animation is running.
     * A scroll frame clears nothing explicitly - the whole viewport
     * repaints, so any strip from a previous partial frame is simply not
     * consulted (strip is frame-local). */
    /* presented-frame fast path: when NOTHING that feeds the image moved
     * since the last presented frame, skip the whole paint + GPU + present
     * below and return early. The gate is the tick's own verdict, not a new
     * fingerprint: anim_values_changed==0 means every interpolated value is
     * identical to the current styles (held delay, finished transition).
     * The outer conditions exclude scroll / layout / DOM / edit / popup and
     * REQUIRE no pending interaction state (state_pending is set by
     * set_hover OUTSIDE the frame, so a hover change between frames must
     * not be mistaken for a no-op - the state relayout above has not run
     * yet when this check executes). Interaction state is ALSO checked via
     * partial: the state relayout above CONSUMES state_pending (and the
     * partial branch consumes the hover/focus/pressed markers into the
     * partial flag) before this check executes, so testing only
     * state_pending here would miss a hover change already handled this
     * frame and skip its repaint. An infinite animation (demo pulse/bar)
     * changes values every frame, so it never takes this path - only HELD
     * animations and static wakeups park here. Runs BEFORE the anim-strip
     * branch (an unchanged-values frame must not grow a strip). Scroll
     * frames never reach here: scroll_dirty is set above. */
    if (r->presented_valid && r->tree && !scroll_frame && !need_layout &&
        !r->has_dirty && !want_scroll_frame &&
        (!r->edit_el || !caret_dirty) &&
        !r->open_select && dom_dirty.empty() && !r->hover_recheck &&
        !anim_values_changed && !animating && !r->state_pending &&
        !partial) {
        /* identical inputs: the GPU target still holds this exact image.
         * Advance the loop state (the tick already ran) and skip the
         * repaint - the swapchain keeps presenting the last image. */
        r->alive = animating ? 1 : 0;
        return 0;
    }
    if (!r->presented_valid || need_layout || r->has_dirty ||
        !dom_dirty.empty()) {
        r->presented_valid = 0;
    }
    if (!scroll_frame && animating && !need_layout && !r->has_dirty &&
        !r->edit_el && !r->open_select) {
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
         * fully while a dropdown is open. Runs AFTER (not instead of) the
         * interaction-state branch above: both damage sources share one
         * strip, so a hover change during an animation repaints the
         * hovered box AND the animated boxes (the old else-if chain let
         * the animation branch win and swallowed hover feedback). */
        const bool lay_anim = whaleui_anim_needs_layout(r->anim);
        /* anim-travel margin: per-element, derived from the actual
         * transform - a width/opacity animation (no transform) needs only
         * a couple of pixels of anti-alias room, while a translate needs
         * its travel distance (capped). A fixed 40px margin on every
         * animated element inflated the 2k strip (and the text-layer
         * upload) for bar/opacity animations that never move. */
        const int AM = 40;
        /* enumerated from the tick's act table via by_el - O(animated)
         * instead of a whole-tree walk per frame. A paint-only animation
         * whose values did not change this frame (held delay, finished
         * transition) adds no damage at all: the strip stays empty and the
         * skip below parks the paint while the animation stays alive. */
        std::vector<lxb_dom_element*> anim_els;
        if (anim_values_changed) {
            int n = whaleui_anim_els(r->anim, nullptr, 0);
            if (n > 0) {
                anim_els.resize(static_cast<size_t>(n));
                whaleui_anim_els(r->anim, anim_els.data(), n);
            }
        }
        for (lxb_dom_element* el : anim_els) {
            whaleui_layout_node_t* nd = find_node_by_el(r->tree, el);
            if (!nd) {
                continue;
            }
            /* the strip is in VIEWPORT space (paint clips against
             * the framebuffer), the layout bounds are DOCUMENT
             * space: add the ancestor scroll offsets or a scrolled
             * animation's strip lands at the wrong rows, the
             * element is culled and the animation appears frozen
             * once it is partially scrolled out of view. (The
             * hover branch above does the same off accumulation.) */
            int off = 0;
            for (whaleui_layout_node_t* p = nd->parent; p;
                 p = p->parent) {
                off += scroll_delta(r, p);
            }
            int x0 = nd->border.x, y0 = nd->border.y + off;
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
                int poff = 0;
                for (whaleui_layout_node_t* q = p->parent; q;
                     q = q->parent) {
                    poff += scroll_delta(r, q);
                }
                x0 = p->border.x < x0 ? p->border.x : x0;
                y0 = p->border.y + poff < y0 ? p->border.y + poff : y0;
                int px1 = p->border.x + p->border.w;
                int py1 = p->border.y + poff + p->border.h;
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
        if (strip.w > 0 && strip.h > 0) {
            partial = true;
        } else if (!r->scroll_dirty && !r->hover_old_el &&
                   !r->focus_old_el && !r->pressed_old_el &&
                   r->hover_prev.empty() && dom_dirty.empty()) {
            /* every animating element is clipped out of the viewport (or
             * has an empty box): the frame carries no visible damage.
             * Skip the paint instead of falling back to a full-window
             * repaint - an animation below the fold (e.g. a hero
             * animation 1000px down a long page) previously repainted the
             * whole window every frame (full CPU paint + full text-layer
             * upload), which is the "animated page crawls at fps<1"
             * report. The animation keeps ticking (alive=1) so the loop
             * paints again the moment the element scrolls into view. */
            r->alive = 1;
            return 0;
        }
    }
    if (partial) {
        /* a partial repaint leaves the scrollbar column stale whenever
         * the strip touches it - the thumb can straddle the strip edge,
         * and a relayout right before this frame (hover) can have moved
         * the thumb so the OLD half-cleaned thumb shows through the new
         * track. Widen the strip to every visible scrollbar column it
         * already touches (cheap: 8px x the box height), so the column
         * always carries freshly-painted content + track + thumb.
         * Columns are resolved through by_el (no tree walk): scroll
         * containers are rare, so track them in a persistent per-render
         * registry instead of discovering them per partial frame. */
        for (lxb_dom_element* bel : r->scroll_boxes) {
            whaleui_layout_node_t* nd = find_node_by_el(r->tree, bel);
            if (!nd || !nd->visible || nd->is_text || nd == r->tree->root ||
                nd->scroll_max <= 0 || nd->border.h < 24) {
                continue;
            }
            int oy = node_scroll_off(r, nd);
            int bx0 = nd->border.x + nd->border.w - 8;
            int by0 = nd->border.y + oy;
            int bx1 = bx0 + 8;
            int by1 = by0 + nd->border.h;
            /* only columns the strip already touches: a scrollbar far
             * from the animated region must not widen the strip to
             * full-screen (that negated the dirty-rect work) */
            if (bx1 > strip.x && bx0 < strip.x + strip.w &&
                by1 > strip.y && by0 < strip.y + strip.h) {
                dirty_rect(bx0 < 0 ? 0 : bx0, by0 < 0 ? 0 : by0,
                           bx1 > r->fb_w ? r->fb_w : bx1,
                           by1 > r->fb_h ? r->fb_h : by1, &strip);
            }
        }
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
     * through the popup. */
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
     * single blit to the swapchain - no per-element GPU round-trips.
     * BACKEND CONTRACT (a future WebGPU backend must keep this shape): all
     * frame-RATE policy lives in the worker (app.cpp pacing sleep) - this
     * path never sleeps, drops, or reorders frames. It acquires, blits,
     * and submits exactly once per call; given identical inputs it produces
     * an identical image. A backend swap therefore inherits the FPS policy
     * (dual-track pacing, occlusion park, caret skip) unchanged. */

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
        bool pop_anim = r->open_select && r->open_select_t != 0 &&
                        whaleui_anim_now() < r->open_select_t + 2500;
        /* same reasoning as the tail gate: an animation started this frame
         * must keep the loop alive even though the frame's tick predates it */
        const bool anim_live = animating ||
                               (r->anim && whaleui_anim_active(r->anim) != 0);
        r->alive = (anim_live || r->edit_el || r->stream_expand.active ||
                    pop_anim)
                       ? 1
                       : 0;
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
    /* the image on screen now matches the current inputs: arm the
     * presented-frame fast path (a later frame whose tick reports no value
     * change and no other dirty state skips paint + GPU work). Snapshot
     * the caret phase painted into this image (the caret gate above
     * compares against it). */
    r->presented_valid = 1;
    if (r->edit_el) {
        r->caret_phase_painted = caret_blink_phase();
    }
    /* keep the frame loop alive while an animation runs or an editable
     * caret blinks; a static page goes idle (the app loop parks). An open
     * select popup keeps frames coming for its open transition (up to a
     * bounded grace past the open timestamp - the paint computes progress
     * from the same clock, so this terminates with it). */
    {
        bool pop_anim = false;
        if (r->open_select && r->open_select_t != 0) {
            uint64_t now = whaleui_anim_now();
            pop_anim = now < r->open_select_t + 2500;
        }
        /* `animating` is the tick's verdict from the TOP of this frame. An
         * animation REGISTERED during this frame's relayout (a C++ style
         * edit that sets `animation`, a class toggle that starts one) is not
         * in it yet - the tick that would report it runs on the NEXT frame.
         * Reporting alive=0 here parked the worker immediately after the
         * frame that started the animation, so it never advanced until some
         * unrelated input (a mouse move, a resize) requested a frame: the
         * reported "CSS animation defined from C++ does not play until I
         * resize the window". Ask the animation state directly as well. */
        const bool anim_live = animating ||
                               (r->anim && whaleui_anim_active(r->anim) != 0);
        r->alive = (anim_live || r->edit_el || r->stream_expand.active ||
                    pop_anim)
                       ? 1
                       : 0;
    }
    return 0;
}















































