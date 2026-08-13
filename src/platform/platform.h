#ifndef WHALEUI_PLATFORM_PLATFORM_H
#define WHALEUI_PLATFORM_PLATFORM_H

/* Platform abstraction - internal interface.
 *
 * Each OS backend (src/platform/<os>/) implements this. Windows/Linux/macOS
 * are the supported set; Android/iOS slots are reserved (step 2: stubs). */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native system font directory paths (for the default-font fallback).
 * Fills out with up to `n` paths; returns the number found. */
int whaleui_platform_system_font_dirs(const char** out, int n);

/* Detect the OS color scheme: WHALEUI_THEME_LIGHT or WHALEUI_THEME_DARK. */
whaleui_theme_t whaleui_platform_system_theme(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_PLATFORM_PLATFORM_H */
