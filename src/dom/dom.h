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

#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_dom_element { char _opaque; };
struct whaleui_dom_document { char _opaque; };
struct lxb_dom_element; /* lexbor element (opaque here) */

/* Record a DOM mutation for the next frame's incremental relayout: the
 * renderer consumes the per-document dirty set and rebuilds only the
 * affected layout subtrees instead of the whole tree. */
void whaleui_dom_mark_dirty(struct lxb_dom_element* el);

/* Move the dirty elements belonging to `doc` into `out` (and clear them
 * from the set). Call once per frame per render context. */
void whaleui_dom_take_dirty(struct whaleui_dom_document* doc,
                            std::vector<struct lxb_dom_element*>& out);

/* Record that the document's CSS custom-property source changed (a style
 * attribute holding --var definitions was edited): the next incremental
 * relayout must re-collect vars. */
void whaleui_dom_mark_vars_dirty(void);

/* Returns and clears the vars-dirty flag (renderer calls it once per
 * dom_dirty frame). */
int whaleui_dom_take_vars_dirty(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_DOM_DOM_H */
