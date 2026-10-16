#ifndef WHALEUI_CORE_TIMERS_H
#define WHALEUI_CORE_TIMERS_H

/* Timers - internal interface (see timers.cpp). */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fire every due timeout/interval on the loop thread. Returns the count. */
int whaleui_timers_pump(void);

/* Fire the pending requestAnimationFrame callbacks (once per rendered frame). */
int whaleui_animation_frames_pump(void);

/* 1 when any timer (including a pending frame callback) is live. */
int whaleui_timers_pending(void);

/* Milliseconds until the next due timeout/interval, or -1 when none. */
double whaleui_timers_next_due_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_TIMERS_H */
