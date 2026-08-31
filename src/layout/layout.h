#ifndef WHALEUI_LAYOUT_LAYOUT_H
#define WHALEUI_LAYOUT_LAYOUT_H

/* Layout - internal interface.
 *
 * Lexbor provides HTML/CSS *parsing*, not layout computation, so this module
 * implements a small box/flex layout engine of its own. One layout pass
 * walks the DOM, computes styles (via style.cpp) and produces a flat layout
 * tree the renderer draws. */

#include "whaleui.h"
#include "style/style.h"

#include <deque>
#include <map>
#include <string>
#include <unordered_map>

/* lexbor element type */
struct lxb_dom_element;
/* animation engine (animate.h) */
struct whaleui_anim;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct whaleui_rect
{
    int x, y, w, h;
} whaleui_rect_t;

/* Tag category id - the first ECS-style component on layout nodes.
 * Computed ONCE per element during the layout pass (layout.cpp tag_id_of),
 * then queried by paint/hit/click code as an O(1) array read instead of
 * repeated lxb_dom_element_local_name + memcmp tag comparisons. */
enum
{
    WUI_TAG_UNKNOWN = 0,
    WUI_TAG_HTML, WUI_TAG_BODY, WUI_TAG_HEAD,
    WUI_TAG_DIV, WUI_TAG_SPAN, WUI_TAG_P, WUI_TAG_A,
    WUI_TAG_STRONG, WUI_TAG_B, WUI_TAG_EM, WUI_TAG_I,
    WUI_TAG_UL, WUI_TAG_OL, WUI_TAG_LI, WUI_TAG_DL, WUI_TAG_DT, WUI_TAG_DD,
    WUI_TAG_H1, WUI_TAG_H2, WUI_TAG_H3, WUI_TAG_H4, WUI_TAG_H5, WUI_TAG_H6,
    WUI_TAG_IMG, WUI_TAG_BR, WUI_TAG_HR,
    WUI_TAG_INPUT, WUI_TAG_SELECT, WUI_TAG_OPTION, WUI_TAG_TEXTAREA,
    WUI_TAG_BUTTON, WUI_TAG_LABEL,
    WUI_TAG_DETAILS, WUI_TAG_SUMMARY,
    WUI_TAG_PROGRESS, WUI_TAG_METER,
    WUI_TAG_TABLE, WUI_TAG_TR, WUI_TAG_TD, WUI_TAG_TH,
    WUI_TAG_HEADER, WUI_TAG_FOOTER, WUI_TAG_MAIN, WUI_TAG_SECTION,
    WUI_TAG_ARTICLE, WUI_TAG_NAV, WUI_TAG_ASIDE,
    WUI_TAG_CODE, WUI_TAG_PRE, WUI_TAG_BLOCKQUOTE,
    WUI_TAG_COUNT
};

struct whaleui_layout_node
{
    lxb_dom_element* el;
    int tag_id;              /* WUI_TAG_* component (computed at layout) */

    whaleui_rect_t border;   /* border-box: margin excluded */
    whaleui_rect_t content;  /* content-box: padding/border excluded */
    int margin[4];           /* top right bottom left */
    int padding[4];
    int border_w[4];

    unsigned visible : 1;    /* display != none */
    unsigned is_text : 1;    /* text run inside a parent box */
    unsigned in_inline : 1;  /* laid out on an inline line (mixed with
                                siblings): wraps to the line remainder,
                                never centers, top-aligned with the line */
    int z;                   /* z-index (0 default) */
    float opacity;           /* cascaded opacity (1 default) */

    /* subtree paint bounds (absolute viewport coords, union of this node's
     * border box and all descendants; computed lazily by the renderer).
     * Lets partial repaints cull whole subtrees instead of walking them. */
    whaleui_rect_t bounds;

    /* vertical scroll of an overflow:auto/scroll container. The children
     * are laid out shifted up by scroll_y (absolute coords stay consistent
     * for hit-test/paint); scroll_max = content height - visible height. */
    int scroll_y;
    int scroll_max;

    WhaleUIComputedStyle style; /* computed style (owned by the node) */
    std::string text;           /* for text runs */

    struct whaleui_layout_node* parent;
    struct whaleui_layout_node* first_child;
    struct whaleui_layout_node* next;
};

typedef struct whaleui_layout_node whaleui_layout_node_t;

/* key for the cascade cache: the element plus the interaction state that
 * drives pseudo-class matching (:hover/:active/:focus) */
struct whaleui_layout_style_key
{
    struct lxb_dom_element* el;
    struct lxb_dom_element* hover;
    struct lxb_dom_element* focus;
    struct lxb_dom_element* pressed;
    bool operator<(const whaleui_layout_style_key& o) const
    {
        if (el != o.el) return el < o.el;
        if (hover != o.hover) return hover < o.hover;
        if (focus != o.focus) return focus < o.focus;
        return pressed < o.pressed;
    }
};

struct whaleui_layout_tree
{
    whaleui_layout_node_t* root;
    int viewport_w, viewport_h;

    /* element -> layout node (the element node itself, not its text runs).
     * Built during the layout pass; find_node_by_el is an O(1) lookup
     * instead of a full-tree walk. Nodes rebuilt by whaleui_layout_relayout
     * replace their entries. */
    std::unordered_map<lxb_dom_element*, whaleui_layout_node_t*> by_el;

    std::deque<whaleui_layout_node_t> arena; /* stable node storage */
    std::deque<std::string> text_arena;      /* text-run storage */

    /* CSS custom properties (--var) collected from the document. Collected
     * once per tree (vars_collected); relayout passes reuse it (the page's
     * vars do not change between DOM mutations, and the collect walk is
     * expensive even on var-less pages). render.cpp clears it when the DOM
     * is mutated so the next pass re-collects. */
    std::map<std::string, std::string> vars;
    bool vars_collected;

    /* computed-style cascade cache (element + interaction state): a
     * width-animation relayout rebuilds the animated element each frame
     * and whaleui_style_compute re-matches every candidate rule per build
     * - the per-frame cost. The cascade is static (animations are applied
     * on top afterwards), so keying on (el, hover, focus, pressed) makes
     * stable frames O(1). Cleared alongside vars when the DOM changes. */
    std::map<whaleui_layout_style_key, WhaleUIComputedStyle> style_cache;
};

typedef struct whaleui_layout_tree whaleui_layout_tree_t;

/* One layout pass: compute styles + boxes for the whole document.
 * theme_vars: extra custom properties (e.g. --bg for the current theme).
 * hover_el: element under the mouse (for :hover rules), may be NULL.
 * scrolls: element -> current scroll_y for overflow:auto/scroll containers
 * (may be NULL). Returns NULL on failure. The caller owns the tree. */
whaleui_layout_tree_t* whaleui_layout_compute(whaleui_dom_document_t* doc,
                                              const whaleui_css_rule_t* rules, size_t count,
                                              const std::map<std::string, std::string>* theme_vars,
                                              int viewport_w, int viewport_h,
                                              const whaleui_style_state* st,
                                              const std::map<struct lxb_dom_element*, int>* scrolls,
                                              struct whaleui_anim* anim,
                                              float text_scale);
void whaleui_layout_destroy(whaleui_layout_tree_t* tree);

/* Incremental relayout: rebuild the layout subtree of `el` (computed style
 * + children, re-read from the live DOM) inside an existing tree, splice it
 * into the parent's child chain, then re-run the box pass over the tree.
 * Subtrees outside the affected path keep their computed styles - only the
 * changed branch and the layout pass (no style cascade for untouched
 * branches) re-run. Returns 0 on success; 1 when `el` has no node in this
 * tree (different document / already removed - caller just skips it); -1
 * on failure (caller should fall back to a full rebuild). The replaced
 * nodes stay in the arena (orphaned, pointer-safe) until the next full
 * rebuild. */
int whaleui_layout_relayout(whaleui_layout_tree_t* tree,
                            struct lxb_dom_element* el,
                            const whaleui_css_rule_t* rules, size_t count,
                            const std::map<std::string, std::string>* theme_vars,
                            const whaleui_style_state* st,
                            const std::map<struct lxb_dom_element*, int>* scrolls,
                            struct whaleui_anim* anim,
                            float text_scale);

/* Batch relayout for a layout-affecting animation: ~same as relayout but
 * rebuilds several animated subtrees before a single whole-tree box pass
 * (per-element relayout would box-pass the whole tree per element). */
int whaleui_layout_relayout_multi(
    whaleui_layout_tree_t* tree, struct lxb_dom_element* const* els,
    size_t nel, const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>* theme_vars,
    const whaleui_style_state* st,
    const std::map<struct lxb_dom_element*, int>* scrolls,
    struct whaleui_anim* anim, float text_scale);

/* Text-width metric hook: the renderer installs this so layout measures
 * REAL glyph widths (inline-line x positions, wrap points, flex sizing)
 * instead of the built-in estimate. letter_spacing_px is the resolved
 * letter-spacing (already px). Return the pixel width of the UTF-8 string,
 * or <= 0 to fall back to the estimate. NULL (pure layout tests) uses the
 * estimate. */
typedef float (*whaleui_text_metric_fn)(const char* utf8, size_t len,
                                        float font_px, bool bold,
                                        const char* family,
                                        float letter_spacing_px);
void whaleui_layout_set_text_metric(whaleui_text_metric_fn fn);

/* REAL line height (px) for a font/size/style, so laid-out text boxes
 * (textarea heights, scroll_max) match the painted glyphs. NULL uses the
 * fs*1.2 estimate. */
typedef float (*whaleui_line_height_fn)(float font_px, bool bold,
                                        const char* family);
void whaleui_layout_set_line_height_metric(whaleui_line_height_fn fn);

/* EXACT wrapped line count for a run: the renderer's per-glyph wrap.
 * When installed, the layout sizes text runs with the same line count the
 * renderer will paint, so run positions / box heights / scroll_max agree
 * to the pixel instead of drifting by an estimated line. NULL keeps the
 * estimate. */
typedef size_t (*whaleui_wrap_lines_fn)(const char* utf8, size_t len,
                                        int avail, int font_px, bool bold,
                                        const char* family,
                                        float letter_spacing_px);
void whaleui_layout_set_wrap_lines_metric(whaleui_wrap_lines_fn fn);

/* wrapped line count of `s` at `avail` px (per-line, real pixel widths);
 * C wrapper of the anonymous-namespace helper (not linkable directly).
 * Exported for tests; the renderer installs the real text metric. */
size_t whaleui_est_wrap_lines(const char* utf8, size_t len, float fs,
                              int avail, bool bold, const char* family,
                              float lsp_px);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_LAYOUT_LAYOUT_H */
