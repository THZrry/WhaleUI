#ifndef WHALEUI_CORE_EXTRAS_H
#define WHALEUI_CORE_EXTRAS_H

/* Web-platform extras - internal interface (see extras.cpp):
 * Web Audio placeholder, Gamepad (SDL-backed), visual viewport. */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Re-open/close gamepads to match SDL's device list. Returns the number of
 * state changes. Called from app.cpp on gamepad add/remove events. */
int whaleui_gamepad_sync(void);

/* The document element that receives gamepadconnect(ed) events (set on load). */
void whaleui_gamepad_set_event_target(void* doc_element);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_EXTRAS_H */
