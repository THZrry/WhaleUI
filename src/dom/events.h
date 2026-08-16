#ifndef WHALEUI_DOM_EVENTS_H
#define WHALEUI_DOM_EVENTS_H

/* DOM events - internal interface.
 *
 * Event listeners live in a side map (element* -> listeners): lexbor nodes
 * carry no event slots. Listeners fire on the target element only (no
 * bubbling/capture - see doc/04-api-dom.md). The event object is transient
 * (valid only during the callback). */

#include "dom/dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Drop listeners registered on `el` (element destroyed). */
void whaleui_events_clear_element(struct lxb_dom_element* el);

/* Drop every listener (document destroyed; elements are unreachable). */
void whaleui_events_clear_all(void);

/* Dispatch a real event from the app event loop (mouse/key/wheel data
 * attached). Returns 0 on success. */
int whaleui_dom_dispatch_event_full(struct lxb_dom_element* el,
                                    const char* type,
                                    int key_code, int mouse_x, int mouse_y,
                                    int mouse_button, float wheel_x,
                                    float wheel_y);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_DOM_EVENTS_H */
