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

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#ifdef WHALEUI_BUILD_FULL
#include <SDL3_ttf/SDL_ttf.h>
#endif
#include <lexbor/dom/dom.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

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
    int w = 0;
    size_t ml = 0;
    if (!TTF_MeasureString(font, utf8, len, 0, &w, &ml)) {
        return 0;
    }
    float total = static_cast<float>(w);
    if (lsp_px > 0) {
        size_t chars = 0;
        for (size_t i = 0; i < len; ++i) {
            if ((static_cast<unsigned char>(utf8[i]) & 0xC0) != 0x80) {
                ++chars;
            }
        }
        if (chars > 1) {
            total += lsp_px * static_cast<float>(chars - 1);
        }
    }
    return total;
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
            if (t && alen > 0 &&
                !(alen == 4 && std::memcmp(t, "text", 4) == 0)) {
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
 * paints (same font, same wrapping). Full build: TTF_Text (fallback chain +
 * wrapping handled natively). Lite/minimal: stb_truetype per-glyph metrics,
 * simpler but functionally equivalent. */



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
 * ckey: element to cache the TTF_Text against (NULL = no caching).
 * lsp: letter-spacing in px (>0 paints glyph by glyph with that gap;
 * TTF_Text has no spacing control, so this is a per-glyph path).
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


/* paint-time scroll offset of a scrollable box: the layout tree already
 * shifted the content up by the scroll_y baked in at build time; the paint
 * path adds (current - baked) MORE shift. Since a larger scroll_y moves
 * content UP, the extra offset is negative: baked - current. */
int scroll_delta(whaleui_render_t* r, whaleui_layout_node_t* n)
{
    if (n->scroll_max > 0 && n->el) {
        auto it = r->scrolls.find(n->el);
        if (it != r->scrolls.end()) {
            return n->scroll_y - it->second;
        }
    }
    return 0;
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
#ifdef WHALEUI_BUILD_FULL
    /* real glyph widths for the layout pass (inline x, wrap points) */
    whaleui_layout_set_text_metric(render_text_metric);
#endif
    r->cursor_arrow = nullptr;
    r->cursor_text = nullptr;
    r->cursor_pointer = nullptr;
    r->anim = whaleui_anim_create();
    r->text_scale = 1.0f;
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
                render_build_fallback(r, r->font_default, 16, 0);
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
    for (auto& f : r->fonts) {
        TTF_CloseFont(f.second);
    }
    TTF_CloseFont(r->font_default);
    for (auto& e : r->text_cache) {
        TTF_DestroyText(e.second.t);
        SDL_DestroySurface(e.second.surf);
    }
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
    if (r->text_engine) {
        TTF_DestroySurfaceTextEngine(r->text_engine);
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
    for (auto& e : r->text_cache) {
        TTF_DestroyText(e.second.t);
        SDL_DestroySurface(e.second.surf);
    }
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
    r->last_scrolls.clear();
    r->hover_el = nullptr;
    r->focus_el = nullptr;
    r->pressed_el = nullptr;
    r->sel_anchor_el = nullptr;
    r->sel_focus_el = nullptr;
    r->sel_anchor = r->sel_focus = 0;
    r->selecting = 0;
    r->edit_el = nullptr;
    r->compose.clear();
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
    /* GPU render targets (geometry ping-pong + composite) must follow the
     * new size; recreate the renderer like the FSR toggle does */
    whaleui_gpu_destroy(r->gpu);
    r->gpu = whaleui_gpu_create(r->device, width, height);
    if (!r->gpu) {
        return -2;
    }
    render_fsr_destroy(r);
    render_fsr_create(r);
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
     * within its name group, matching browser behavior) */
    if (hit && hit->el && is_check_radio(hit->el)) {
        if (lxb_dom_element_has_attribute(hit->el, (const lxb_char_t*)"checked", 7)) {
            lxb_dom_element_remove_attribute(hit->el, (const lxb_char_t*)"checked", 7);
        } else {
            lxb_dom_element_set_attribute(hit->el, (const lxb_char_t*)"checked", 7,
                                          (const lxb_char_t*)"", 0);
            size_t nlen = 0;
            const lxb_char_t* nm = lxb_dom_element_get_attribute(
                hit->el, (const lxb_char_t*)"name", 4, &nlen);
            size_t tlen = 0;
            const lxb_char_t* tname = lxb_dom_element_get_attribute(
                hit->el, (const lxb_char_t*)"type", 4, &tlen);
            bool radio = tname && tlen == 5 &&
                         std::memcmp(tname, "radio", 5) == 0;
            if (radio && nm && nlen > 0) {
                /* clear checked on every other radio with the same name */
                std::function<void(lxb_dom_node*)> uncheck =
                    [&](lxb_dom_node* nd) {
                        while (nd) {
                            if (nd->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                                lxb_dom_element* e = lxb_dom_interface_element(nd);
                                if (e != hit->el &&
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
                lxb_dom_document* doc2 = hit->el->node.owner_document;
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
    }
    whaleui_layout_node_t* hit = hit_test(r, r->tree->root, x, y, 0);
    lxb_dom_element* el = hit ? hit->el : nullptr;
    if (el != r->hover_el) {
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
        r->drag_scroll_el = nullptr;
        r->drag_scroll_node = nullptr;
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
        if (ov == "auto" || ov == "scroll") {
            do_scroll(n);
            return;
        }
    }
    /* nothing explicitly scrollable: scroll the page (html root) */
    do_scroll(r->tree->root);
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
    /* consume this document's pending DOM mutations up front: a non-empty
     * set must keep the frame alive so the incremental relayout below runs */
    std::vector<lxb_dom_element*> dom_dirty;
    whaleui_dom_take_dirty(doc, dom_dirty);
    /* skip the whole frame when nothing changed: idle frames cost ~0.
     * Repaint when the layout/state is dirty, a wheel scroll happened, an
     * animation/transition is running, an editable caret is blinking, or
     * the DOM was mutated. */
    if (!r->has_dirty && r->tree && !r->scroll_dirty &&
        !animating && !r->edit_el && dom_dirty.empty()) {
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
    /* layout rebuild needed when the document is dirty OR an animation
     * touches a layout-affecting property (width/margin/font-size/...).
     * Paint-only animations (opacity/transform/colors) skip the rebuild:
     * the tick's values are applied straight onto the tree and only the
     * opacity chain is recomputed - the bulk of the per-frame cost
     * (style cascade + box layout) is gone. */
    const bool need_layout = r->has_dirty || !r->tree ||
                             (animating && whaleui_anim_needs_layout(r->anim));
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
        /* the viewport may have changed size (resize/fullscreen): clamp
         * every live scroll position to its new range so scrollbars and
         * content don't sit past the end */
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
        g_metric_render = nullptr; /* layout done; paint uses TTF_Text */
#endif
    } else if (animating) {
        /* paint-only animation: apply the tick's values to the tree styles
         * and refresh the cascaded opacity (children inherit it) */
        std::function<void(whaleui_layout_node_t*)> apply_ov =
            [&](whaleui_layout_node_t* nd) {
                if (nd->el && whaleui_anim_has_el(r->anim, nd->el)) {
                    whaleui_anim_apply_ov(r->anim, nd->el, nd->style);
                }
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    apply_ov(c);
                }
            };
        apply_ov(r->tree->root);
        std::function<void(whaleui_layout_node_t*, float)> re_op =
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
                    re_op(c, nd->opacity);
                }
            };
        re_op(r->tree->root, 1.0f);
    }
    /* DOM mutations: incremental relayout. Only the affected subtrees get
     * a fresh style cascade + build; the box pass re-positions the tree
     * while untouched branches keep their computed styles. Falls back to a
     * full rebuild (has_dirty) when the tree is inconsistent. */
    if (!need_layout && !dom_dirty.empty()) {
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
            g_metric_render = nullptr; /* layout done; paint uses TTF_Text */
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
     * Any other dirty/animations fall back to a full repaint (dy=0). */
    int scroll_dy = 0;
    if (!r->has_dirty && !r->scroll_dirty && !animating && !r->edit_el &&
        r->tree) {
        std::map<lxb_dom_element*, int> cur;
        std::function<void(whaleui_layout_node_t*)> collect =
            [&](whaleui_layout_node_t* nd) {
                if (nd->scroll_max > 0 && nd->el) {
                    auto it = r->scrolls.find(nd->el);
                    if (it != r->scrolls.end()) {
                        cur[nd->el] = it->second;
                    }
                }
                for (whaleui_layout_node_t* c = nd->first_child; c;
                     c = c->next) {
                    collect(c);
                }
            };
        collect(r->tree->root);
        for (auto& kv : cur) {
            auto it = r->last_scrolls.find(kv.first);
            if (it != r->last_scrolls.end()) {
                scroll_dy += kv.second - it->second;
            }
        }
        r->last_scrolls = std::move(cur);
    }

    /* paint: collect batched GPU draw commands. Text goes to the CPU layer
     * (moved on scroll), uploaded + composited by a compute pass. */
    Clip full = {0, 0, r->fb_w, r->fb_h};
    Clip strip = full;
    if (scroll_dy != 0) {
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
    bool partial = false; /* load-only repaint of a dirty region */
    if (scroll_dy != 0) {
        /* scroll strip handled above */
    } else if (animating && !need_layout && !r->has_dirty && !r->edit_el) {
        /* paint-only animation: repaint only the animating elements'
         * bounding boxes (dirty-rect, keeps the rest of the frame) */
        std::function<void(whaleui_layout_node_t*)> acc =
            [&](whaleui_layout_node_t* nd) {
                if (nd->el && whaleui_anim_has_el(r->anim, nd->el)) {
                    int x0 = nd->border.x, y0 = nd->border.y;
                    int x1 = x0 + nd->border.w, y1 = y0 + nd->border.h;
                    /* widen by the transform translation */
                    std::string tv = sget(nd->style, "transform");
                    if (!tv.empty() && tv != "none") {
                        whaleui_transform_t tf;
                        if (whaleui_transform_eval(
                                tv.c_str(), static_cast<float>(nd->border.w),
                                static_cast<float>(nd->border.h), &tf) == 0) {
                            x0 -= static_cast<int>(tf.tx > 0 ? tf.tx : -tf.tx);
                            y0 -= static_cast<int>(tf.ty > 0 ? tf.ty : -tf.ty);
                            x1 += static_cast<int>(tf.tx > 0 ? tf.tx : -tf.tx);
                            y1 += static_cast<int>(tf.ty > 0 ? tf.ty : -tf.ty);
                        }
                    }
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
    g_backdrop_active = (scroll_dy == 0 && !partial) ? 1 : 0;
    int sel_lo = 0, sel_hi = 0;
    if (!r->bounds_valid) {
        compute_paint_bounds(r->tree->root);
        r->bounds_valid = 1;
    }
    const Clip* paint_clip = (partial || scroll_dy != 0) ? &strip : &full;
    sel_seq(r, &sel_lo, &sel_hi, paint_clip);
    int seq = 0;
    paint_node(r, r->tree->root, 0, 0, seq, sel_lo, sel_hi, paint_clip,
               false);

    /* expanded select list is drawn last (highest z) so later siblings and
     * other content cannot cover it; its position follows the select's
     * scroll-offset ancestors */
    if (r->open_select) {
        whaleui_layout_node_t* s = find_node_by_el(r->tree, r->open_select);
        if (s) {
            int soff = 0;
            for (whaleui_layout_node_t* p = s->parent; p; p = p->parent) {
                soff += scroll_delta(r, p);
            }
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
    if (!SDL_AcquireGPUSwapchainTexture(cmd, r->window, &swapchain, &sw, &sh) || !swapchain) {
        SDL_SubmitGPUCommandBuffer(cmd);
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
    return 0;
}

