#ifndef WHALEUI_PLATFORM_PLATFORM_H
#define WHALEUI_PLATFORM_PLATFORM_H

/* Platform abstraction - internal interface.
 *
 * Responsibilities:
 *   - system font directory enumeration (default-font fallback)
 *   - OS color-scheme detection (light/dark)
 *   - (step 3) SDL init helpers, native event mapping, per-OS backend hooks
 *
 * Windows are NOT created here: SDL3's SDL_CreateWindow is cross-platform, so
 * the core creates windows directly and keeps the SDL_Window* in
 * whaleui_window. Per-OS behavior that SDL does not cover (font dirs, theme,
 * DPI policy) lives in this module. Each backend lives in src/platform/<os>/
 * and implements the functions below. */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native system font directory paths (for the default-font fallback).
 * Fills out with up to `n` paths; returns the number found. */
int whaleui_platform_system_font_dirs(const char** out, int n);

/* Detect the OS color scheme: WHALEUI_THEME_LIGHT or WHALEUI_THEME_DARK. */
whaleui_theme_t whaleui_platform_system_theme(void);

/* (step 3) One-time platform init (SDL subsystems etc.). Returns 0 on success. */
int whaleui_platform_init(void);

/* (step 3) Platform shutdown. */
void whaleui_platform_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_PLATFORM_PLATFORM_H */
