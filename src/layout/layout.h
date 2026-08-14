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

/* lexbor element type */
struct lxb_dom_element;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct whaleui_rect
{
    int x, y, w, h;
} whaleui_rect_t;

struct whaleui_layout_node
{
    lxb_dom_element* el;

    whaleui_rect_t border;   /* border-box: margin excluded */
    whaleui_rect_t content;  /* content-box: padding/border excluded */
    int margin[4];           /* top right bottom left */
    int padding[4];
    int border_w[4];

    unsigned visible : 1;    /* display != none */
    unsigned is_text : 1;    /* text run inside a parent box */
    int z;                   /* z-index (0 default) */
    float opacity;           /* cascaded opacity (1 default) */

    WhaleUIComputedStyle style; /* computed style (owned by the node) */
    std::string text;           /* for text runs */

    struct whaleui_layout_node* parent;
    struct whaleui_layout_node* first_child;
    struct whaleui_layout_node* next;
};

typedef struct whaleui_layout_node whaleui_layout_node_t;

struct whaleui_layout_tree
{
    whaleui_layout_node_t* root;
    int viewport_w, viewport_h;

    std::deque<whaleui_layout_node_t> arena; /* stable node storage */
    std::deque<std::string> text_arena;      /* text-run storage */
};

typedef struct whaleui_layout_tree whaleui_layout_tree_t;

/* One layout pass: compute styles + boxes for the whole document.
 * theme_vars: extra custom properties (e.g. --bg for the current theme).
 * hover_el: element under the mouse (for :hover rules), may be NULL.
 * Returns NULL on failure. The caller owns the tree. */
whaleui_layout_tree_t* whaleui_layout_compute(whaleui_dom_document_t* doc,
                                              const whaleui_css_rule_t* rules, size_t count,
                                              const std::map<std::string, std::string>* theme_vars,
                                              int viewport_w, int viewport_h,
                                              const whaleui_style_state* st);
void whaleui_layout_destroy(whaleui_layout_tree_t* tree);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_LAYOUT_LAYOUT_H */
