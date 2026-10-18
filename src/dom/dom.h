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

#include <mutex>
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

/* 1 when the pending set holds at least one element (of any document).
 * The app loop uses this to wake the render worker: a DOM edit made from
 * the app thread produced no frame request, so the worker stayed parked and
 * the page showed the pre-edit pixels until unrelated input (a mouse move)
 * happened to wake it - the reported "C++ changes display and nothing
 * redraws for ten seconds". */
int whaleui_dom_pending_any(void);

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

/* --- document lock -------------------------------------------------------
 *
 * The render worker walks the lexbor document (layout build, hit test,
 * style cascade) while the app mutates it. lexbor has no internal locking,
 * so an edit that lands mid-walk corrupts the tree: the same sequence of
 * API calls produced a correct page one run and a blank/frozen page the
 * next, depending on where the frame happened to be when the edit arrived.
 *
 * WRITE-PREFERRED, SHORT HOLD. The frame does NOT hold this for its whole
 * duration - it takes it around the individual lexbor touches (cascade,
 * rebuild, text measure, hit test). Holding it across a 50ms layout froze
 * every app-side DOM call for 50ms, which is exactly the "UI freezes, the
 * window does not respond" symptom: a caller blocked on a mutex looks
 * identical to a hung UI thread. Short holds keep a DOM call to ~1ms even
 * while a heavy frame runs.
 *
 * It is RECURSIVE because a frame drains the dirty queues and can dispatch
 * a listener that mutates the DOM from inside a hold. */
void whaleui_dom_lock(void);
void whaleui_dom_unlock(void);

/* Queue a mutation to run on the next frame's thread instead of blocking
 * the caller. The callback runs with the document lock held, before the
 * frame's layout pass, in submission order. This is the zero-latency path
 * for a UI thread that must not stall: `set_text` from a background job can
 * post here and return immediately. */
typedef void (*whaleui_dom_mutation_fn)(void* userdata);
void whaleui_dom_post(whaleui_dom_mutation_fn fn, void* userdata);
/* Run every queued mutation (called by the frame before layout). Returns
 * how many ran. */
int whaleui_dom_pump_posted(void);
/* 1 when at least one mutation is queued (the app loop keeps requesting
 * frames so posted work is not left waiting for unrelated input). */
int whaleui_dom_posted_pending(void);

#ifdef __cplusplus
}
#endif

/* The mutex behind whaleui_dom_lock/unlock, for callers that need to hold
 * it across several statements (the frame locks it per phase rather than
 * for its whole duration - see the note above). */
std::recursive_mutex& whaleui_dom_mutex(void);

/* RAII helper for C++ callers inside the engine. */
class WhaleUIDomLock {
public:
    WhaleUIDomLock() { whaleui_dom_lock(); }
    ~WhaleUIDomLock() { whaleui_dom_unlock(); }
    WhaleUIDomLock(const WhaleUIDomLock&) = delete;
    WhaleUIDomLock& operator=(const WhaleUIDomLock&) = delete;
};

#endif /* WHALEUI_DOM_DOM_H */
