#ifndef WHALEUI_STYLE_THEME_INTERNAL_H
#define WHALEUI_STYLE_THEME_INTERNAL_H

/* Theme internals. The built-in stylesheets and the per-style color
 * tables live in separate translation units so each one stays small
 * enough to read in a single pass:
 *   theme_css.cpp    UA baseline + component classes + per-style overlay
 *   theme_colors.cpp light/dark palettes for the eight styles
 *   theme.cpp        metrics table + assembly + C API
 * The shared type is VarMap (a plain string map); var() in a stylesheet is
 * resolved against it at cascade time. */

#include <map>
#include <string>

typedef std::map<std::string, std::string> VarMap;

/* UA baseline stylesheet (WHATWG UA stylesheet, engine subset). */
extern const char* const kWhaleuiUaCss;

/* Component classes + control baseline, shared by every style. */
extern const char* const kWhaleuiComponentsCss;

/* The per-style overlay for one style name. Never null; an unknown name
 * falls back to the default style. */
const char* whaleui_theme_overlay_css(const char* style);

/* Fill the color variables (--bg/--fg/--accent/--btn-* ...) for a style.
 * dark selects the palette; a non-empty accent overrides the style's own
 * accent color (the app's theme-color setting). */
void whaleui_theme_palette(const char* style, int dark, const char* accent,
                           VarMap& out);

#endif /* WHALEUI_STYLE_THEME_INTERNAL_H */
