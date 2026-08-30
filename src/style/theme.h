#ifndef WHALEUI_STYLE_THEME_H
#define WHALEUI_STYLE_THEME_H

/* Theme styles: built-in design presets (Fluent / Metro / Material /
 * Classic / Aero / GTK / macOS / Browser-UA), each with a default
 * stylesheet + light/dark variable tables. Stylesheets are translations of
 * the referenced design systems (fluent-css, 7.css, mdui, photon,
 * adwaita-web, Fabric Core 9.6.1 - sources marked inline in theme.cpp).
 * Themes are global: every element that uses var(--*) or inherits body
 * color/font follows the current style, including tags the user's HTML does
 * not style. */

#include "whaleui.h"

#include <map>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

/* number of built-in theme styles */
int whaleui_theme_count(void);

/* style id for index i ("fluent", "metro", ...) - valid until next call */
const char* whaleui_theme_name(int i);

/* display label for index i ("Fluent", "Metro", ...) */
const char* whaleui_theme_label(int i);

/* normalize a style name; unknown names fall back to "fluent" */
const char* whaleui_theme_resolve(const char* name);

/* built-in default stylesheet for a theme style */
const char* whaleui_theme_default_css(const char* style);

/* fill vars (--bg/--fg/--card/--border/--accent/--btn-* ...) for a style */
void whaleui_theme_vars(const char* style, whaleui_theme_t theme,
                        const char* accent,
                        std::map<std::string, std::string>& out);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_STYLE_THEME_H */
