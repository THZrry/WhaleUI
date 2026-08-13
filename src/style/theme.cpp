/* Theme: built-in default stylesheet + light/dark variable tables. */

#include "style/theme.h"

#include <cstring>

const char* whaleui_theme_default_css(void)
{
    return
        /* baseline: all tags are divs internally */
        "html, body { margin: 0; padding: 0; }\n"
        "body { background-color: var(--bg); color: var(--fg);\n"
        "       font-family: sans-serif; font-size: 14px; line-height: 1.4; }\n"
        "h1 { font-size: 26px; margin: 12px 0 8px; font-weight: bold; }\n"
        "h2 { font-size: 20px; margin: 10px 0 6px; font-weight: bold; }\n"
        "h3 { font-size: 17px; margin: 8px 0 4px; font-weight: bold; }\n"
        "p { margin: 6px 0; }\n"
        "strong, b { font-weight: bold; }\n"
        "em, i { font-style: italic; }\n"
        "a { color: var(--accent); text-decoration: none; }\n"
        "ul, ol { margin: 6px 0; padding-left: 24px; }\n"
        "li { margin: 2px 0; }\n"
        "button { background: var(--accent); color: #ffffff; border: none;\n"
        "         padding: 6px 14px; border-radius: 4px; font-size: 14px;\n"
        "         cursor: pointer; }\n"
        "input, select, textarea { background: var(--card); color: var(--fg);\n"
        "         border: 1px solid var(--border); padding: 6px 8px;\n"
        "         border-radius: 4px; font-size: 14px; }\n"
        "summary { cursor: pointer; }\n"
        "hr { border: none; border-top: 1px solid var(--border); margin: 8px 0; }\n"
        /* shared utility classes */
        ".card { background: var(--card); border: 1px solid var(--border);\n"
        "        border-radius: 8px; padding: 16px; }\n"
        ".row { display: flex; flex-direction: row; }\n"
        ".column { display: flex; flex-direction: column; }\n"
        ".center { display: flex; justify-content: center; align-items: center; }\n"
        ".hidden { display: none; }\n"
        ".muted { opacity: 0.6; }\n";
}

void whaleui_theme_vars(whaleui_theme_t theme, const char* accent,
                        std::map<std::string, std::string>& out)
{
    bool dark = theme == WHALEUI_THEME_DARK;
    out["--bg"] = dark ? "#1e1e1e" : "#f3f3f3";
    out["--fg"] = dark ? "#e8e8e8" : "#1a1a1a";
    out["--card"] = dark ? "#2b2b2b" : "#ffffff";
    out["--border"] = dark ? "#3d3d3d" : "#d5d5d5";
    out["--muted"] = dark ? "#9a9a9a" : "#666666";
    out["--accent"] = accent && *accent ? accent : "#0078D4";
}
