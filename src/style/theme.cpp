/* Theme: Fluent (Windows 11) preset as the built-in default.
 *
 * Structure per the project spec: every tag gets a neutral preset (all tags
 * are divs internally), a small set of shared utility classes is kept
 * (.card/.row/.column/.center/.muted/...), and colors come from custom
 * properties so the theme can hot-swap. Pages can override any of this -
 * their stylesheets cascade after the default. */

#include "style/theme.h"

#include <cstring>

const char* whaleui_theme_default_css(void)
{
    return
        /* baseline: all tags are divs internally. html carries the window
         * background (it always spans the full viewport); body inherits it */
        "html, body { margin: 0; padding: 0; }\n"
        "html { background-color: var(--bg); }\n"
        "body { background-color: var(--bg); color: var(--fg);\n"
        "       font-family: \"Segoe UI\", sans-serif; font-size: 14px;\n"
        "       line-height: 1.5; }\n"
        "h1 { font-size: 24px; margin: 2px 0 12px; font-weight: 600; }\n"
        "h2 { font-size: 17px; margin: 0 0 10px; font-weight: 600; }\n"
        "h3 { font-size: 14px; margin: 8px 0 4px; font-weight: 600; }\n"
        "p { margin: 6px 0; }\n"
        "strong, b { font-weight: 600; }\n"
        "em, i { font-style: italic; }\n"
        "a { color: var(--accent); text-decoration: none; }\n"
        "ul, ol { margin: 6px 0; padding-left: 24px; }\n"
        "li { margin: 3px 0; }\n"
        /* Fluent-style controls */
        "button { background: var(--accent); color: var(--accent-fg);\n"
        "         border: none; padding: 6px 16px; border-radius: 6px;\n"
        "         font-size: 13px; font-weight: 600; cursor: pointer; }\n"
        "input, select, textarea { background: var(--field); color: var(--fg);\n"
        "         border: 1px solid var(--border); padding: 5px 10px;\n"
        "         border-radius: 6px; font-size: 13px; }\n"
        "summary { cursor: pointer; }\n"
        "hr { border: none; border-top: 1px solid var(--border); margin: 12px 0; }\n"
        /* shared utility classes */
        ".card { background: var(--card); border: 1px solid var(--border);\n"
        "        border-radius: 8px; padding: 16px 18px; }\n"
        ".row { display: flex; flex-direction: row; }\n"
        ".column { display: flex; flex-direction: column; }\n"
        ".center { display: flex; justify-content: center; align-items: center; }\n"
        ".hidden { display: none; }\n"
        ".muted { color: var(--muted); }\n";
}

void whaleui_theme_vars(whaleui_theme_t theme, const char* accent,
                        std::map<std::string, std::string>& out)
{
    bool dark = theme == WHALEUI_THEME_DARK;
    /* Windows 11 Fluent tokens (approximate). Accent stays the same across
     * light/dark (like Win11); surfaces and text adapt. */
    out["--bg"] = dark ? "#202020" : "#f3f3f3";
    out["--fg"] = dark ? "#f5f5f5" : "#1a1a1a";
    out["--card"] = dark ? "#2d2d2d" : "#ffffff";
    out["--field"] = dark ? "#262626" : "#ffffff";
    out["--border"] = dark ? "#3d3d3d" : "#d9d9d9";
    out["--muted"] = dark ? "#9f9f9f" : "#767676";
    out["--accent"] = accent && *accent ? accent : "#0067c0";
    out["--accent-fg"] = "#ffffff";
}
