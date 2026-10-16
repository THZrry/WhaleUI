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

/* Non-bubbling dispatch (capture -> target only): the browser's
 * mouseenter/mouseleave/focus/blur never bubble, so ancestor listeners must
 * not fire on them. */
int whaleui_dom_dispatch_nonbubble(whaleui_dom_element_t* el,
                                   const char* type,
                                   int key_code, int mouse_x, int mouse_y,
                                   int mouse_button, float wheel_x,
                                   float wheel_y, int* out_prevented);

/* Convenience for engine-internal dispatch sites (app.cpp / render.cpp):
 * bubble variant, result unused. */
#define WUI_DOM_FIRE(el, type, key, mx, my, btn, wx, wy) \
    do { if (el) whaleui_dom_dispatch_phased((el), (type), (key), (mx), (my), \
                                             (btn), (wx), (wy), nullptr); } while (0)
#define WUI_DOM_FIRE_NB(el, type, key, mx, my, btn, wx, wy) \
    do { if (el) whaleui_dom_dispatch_nonbubble((el), (type), (key), (mx), (my), \
                                                (btn), (wx), (wy), nullptr); } while (0)

/* Full-payload dispatch with the listener-supplied clipboard text reported
 * back: the engine's copy/cut/paste default behavior writes/reads it. */
int whaleui_dom_dispatch_input_clipboard(whaleui_dom_element_t* el,
                                         const char* type,
                                         const whaleui_event_init_t* init,
                                         int* out_prevented,
                                         const char** out_clipboard);

/* Build a payload from the scalar fields (compat shims use this). */
whaleui_event_init_t whaleui_event_init_scalars(int key_code, int mouse_x,
                                                int mouse_y, int mouse_button,
                                                float wheel_x, float wheel_y);

/* Dispatch a drag/drop event carrying a DataTransfer. The transfer is
 * borrowed (the caller keeps ownership). Returns 0 on success; out_prevented
 * (may be NULL) reports preventDefault (a dragover listener calling it is what
 * makes the element a drop target). */
int whaleui_dom_dispatch_drag(whaleui_dom_element_t* el, const char* type,
                              void* data_transfer, int mouse_x, int mouse_y,
                              int mods, int* out_prevented);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_DOM_EVENTS_H */
