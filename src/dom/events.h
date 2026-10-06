#ifndef WHALEUI_DOM_EVENTS_H
#define WHALEUI_DOM_EVENTS_H

/* DOM events - internal interface.
 *
 * Event listeners live in a side map (element* -> listeners): lexbor nodes
 * carry no event slots. Dispatch follows the DOM three-phase model:
 * capture down the ancestor chain, target, bubble back up. The event
 * object is transient (valid only during the callback). */

#include "dom/dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Drop listeners registered on `el` (element destroyed). */
void whaleui_events_clear_element(struct lxb_dom_element* el);

/* Drop every listener (document destroyed; elements are unreachable). */
void whaleui_events_clear_all(void);

/* Register a listener with an explicit capture flag (the public
 * whaleui_dom_add_event_listener is the bubble (capture=0) shorthand). */
int whaleui_dom_add_event_listener_ex(whaleui_dom_element_t* el,
                                      const char* type, whaleui_event_cb cb,
                                      void* userdata, int capture);

/* Dispatch a real event from the app event loop (mouse/key/wheel data
 * attached). Returns 0 on success. */
int whaleui_dom_dispatch_event_full(whaleui_dom_element_t* el,
                                    const char* type,
                                    int key_code, int mouse_x, int mouse_y,
                                    int mouse_button, float wheel_x,
                                    float wheel_y);

/* Three-phase dispatch (capture -> target -> bubble). out_prevented (may be
 * NULL) reports whether any listener called prevent_default: the engine's
 * click/key handlers consult it before running their default behavior
 * (select toggle, checkbox flip, details expand, focus move). Returns 0 on
 * success. */
int whaleui_dom_dispatch_phased(whaleui_dom_element_t* el,
                                const char* type,
                                int key_code, int mouse_x, int mouse_y,
                                int mouse_button, float wheel_x,
                                float wheel_y, int* out_prevented);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_DOM_EVENTS_H */
