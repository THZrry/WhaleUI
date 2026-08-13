#ifndef WHALEUI_STYLE_THEME_H
#define WHALEUI_STYLE_THEME_H

/* Theme: built-in default stylesheet + light/dark variable tables.
 *
 * The default stylesheet is a small Chromium-like baseline: every tag has a
 * preset (all tags are divs internally, per the project spec), a shared set
 * of utility classes is kept, and colors come from custom properties so the
 * theme can hot-swap. */

#include "whaleui.h"

#include <map>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in default stylesheet (applied before any document CSS). */
const char* whaleui_theme_default_css(void);

/* Fill vars with --bg/--fg/--card/--border/--accent for the resolved theme. */
void whaleui_theme_vars(whaleui_theme_t theme, const char* accent,
                        std::map<std::string, std::string>& out);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_STYLE_THEME_H */
