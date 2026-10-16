#ifndef WHALEUI_CORE_CLIPBOARD_H
#define WHALEUI_CORE_CLIPBOARD_H

/* Clipboard - internal interface (SDL-backed; see clipboard.cpp). */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run the queued async clipboard callbacks (called once per loop iteration
 * from app.cpp). Returns the number of callbacks run. */
int whaleui_clipboard_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_CLIPBOARD_H */
