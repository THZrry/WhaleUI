#ifndef WHALEUI_CORE_WEBAPI_H
#define WHALEUI_CORE_WEBAPI_H

/* Web platform APIs that live outside the DOM/render modules
 * (see webapi.cpp): matchMedia, CSS.supports, screen/navigator info,
 * Web Storage, microtasks and idle callbacks. */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run queued microtasks (queueMicrotask), then idle callbacks that are due.
 * Called from the render worker each iteration. Returns the count run. */
int whaleui_webapi_pump_microtasks(void);
int whaleui_webapi_pump_idle(void);

/* 1 when a microtask or idle callback is queued (keeps the loop awake for
 * microtasks; idle callbacks do not hold the loop open). */
int whaleui_webapi_has_microtasks(void);
int whaleui_webapi_has_idle(void);

/* Re-evaluate every live MediaQueryList (window resize / theme change). */
void whaleui_webapi_refresh_media(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_WEBAPI_H */
