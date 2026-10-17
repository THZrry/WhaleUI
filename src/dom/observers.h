#ifndef WHALEUI_DOM_OBSERVERS_H
#define WHALEUI_DOM_OBSERVERS_H

/* MutationObserver - internal interface (see observers.cpp). */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Record a mutation for every observer watching `target`'s subtree. Called by
 * the DOM mutation APIs (dom.cpp):
 *   type: 0 = attributes, 1 = characterData, 2 = childList
 *   attr: the attribute name for type 0 (NULL otherwise)
 *   added/removed: 1 when childList gained/lost nodes */
void whaleui_dom_notify_mutation(struct lxb_dom_element* target, int type,
                                 const char* attr, int added, int removed);

/* Deliver queued records (called once per loop iteration before the frame). */
int whaleui_dom_observers_pump(void);
int whaleui_dom_observers_pending(void);

/* ResizeObserver: compare each observed element's laid-out box against the
 * last delivered one and fire on change. Called once per frame (after
 * layout) from the render worker; returns the number of callbacks fired. */
int whaleui_dom_resize_observers_pump(void);
int whaleui_dom_resize_observers_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_DOM_OBSERVERS_H */
