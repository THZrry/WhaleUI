#ifndef WHALEUI_DOM_DOM_H
#define WHALEUI_DOM_DOM_H

/* DOM - internal interface.
 *
 * Step 3: handles ARE the lexbor objects. A whaleui_dom_document_t* is an
 * lxb_html_document* and a whaleui_dom_element_t* is an lxb_dom_element*,
 * passed through as opaque pointers (C-compatible, stable identity, no
 * wrapper allocation). The structs below are never instantiated; they only
 * give the public API a named type. Internal modules cast back via
 * reinterpret_cast. */

#include "whaleui.h"

#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_dom_element { char _opaque; };
struct whaleui_dom_document { char _opaque; };
struct whaleui_style_declaration {
    std::vector<std::string> names;
    std::vector<std::string> values;
};
struct lxb_dom_element; /* lexbor element (opaque here) */

/* Record a DOM mutation for the next frame's incremental relayout: the
 * renderer consumes the per-document dirty set and rebuilds only the
 * affected layout subtrees instead of the whole tree. */
void whaleui_dom_mark_dirty(struct lxb_dom_element* el);

/* Move the dirty elements belonging to `doc` into `out` (and clear them
 * from the set). Call once per frame per render context. */
void whaleui_dom_take_dirty(struct whaleui_dom_document* doc,
                            std::vector<struct lxb_dom_element*>& out);

/* Same drain, but `out_structural` (parallel to `out`) says whether each
 * element had a STRUCTURAL edit (children/text added, removed, moved,
 * replaced). Only those can change a run's text; style-only edits (hover,
 * class, attribute) leave the text raster caches valid. The renderer uses this
 * to version text caches per element instead of invalidating all of them. */
void whaleui_dom_take_dirty_typed(struct whaleui_dom_document* doc,
                                  std::vector<struct lxb_dom_element*>& out,
                                  std::vector<int>& out_structural);

/* Record that the document's CSS custom-property source changed (a style
 * attribute holding --var definitions was edited): the next incremental
 * relayout must re-collect vars. */
void whaleui_dom_mark_vars_dirty(void);

/* Returns and clears the vars-dirty flag (renderer calls it once per
 * dom_dirty frame). */
int whaleui_dom_take_vars_dirty(void);

/* Structural DOM change (append/remove/move/replace children, text node
 * swap, inner/outer HTML): marks the element dirty AND sets the frame's
 * structural flag - the renderer must run a full relayout (the subtree's
 * shape changed, geometry may shift following siblings) and a wide
 * repaint. Attribute/style-only edits (whaleui_dom_mark_dirty without the
 * struct flag) can take the cheaper layout-key-diff -> style-only path. */
void whaleui_dom_mark_dirty_struct(struct lxb_dom_element* el);

/* Returns and clears the structural flag (once per frame). */
int whaleui_dom_take_struct_dirty(void);

/* A stylesheet source changed through the DOM (<style> text/attributes, a
 * <link rel=stylesheet> attribute, or any add/remove/move under <head>):
 * the collected stylesheet the render context holds is stale. The render
 * worker must re-collect (whaleui_window_refresh_css) before the next frame -
 * the engine otherwise only collects CSS at load / resize / theme change, so
 * an injected or rewritten stylesheet never took effect ("CSS mounted from
 * the API does nothing" while the same CSS in the markup works). Per document:
 * returns 1 and clears the flag for `doc` only. */
int whaleui_dom_take_css_dirty(struct whaleui_dom_document* doc);

/* Non-consuming form: the worker uses it to decide whether a frame must be
 * requested (the frame itself consumes the flag when it re-collects). */
int whaleui_dom_has_css_dirty(struct whaleui_dom_document* doc);

/* Mark `doc`'s stylesheet stale from outside the DOM layer (whaleui_css_apply
 * attaches a programmatic sheet, so the assembled sheet must be rebuilt). */
void whaleui_dom_mark_css_dirty(struct whaleui_dom_document* doc);

/* The DOM-layer active element (document.activeElement mirror). Single
 * writer: whaleui_dom_focus/blur (public API) and the render focus
 * bookkeeping call this; whaleui_dom_active_element reads it. nullptr
 * falls back to the document body, like the browser. */
void whaleui_dom_set_active(struct lxb_dom_element* el);
/* Clear the mirror on `doc` (focus moved to nothing - a click outside any
 * focusable element). */
void whaleui_dom_clear_active(struct lxb_dom_document* doc);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_DOM_DOM_H */
