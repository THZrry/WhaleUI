/* Theme: built-in default stylesheet + light/dark variable tables. */

#include "style/theme.h"

#include <cstring>

const char* whaleui_theme_default_css(void)
{
    return
        /* baseline: all tags are divs internally */
        "html, body { margin: 0; padding: 0; }\n"
        "body { background-color: var(--bg); color: var(--fg);\n"
        "       font-family: sans-serif; font-size: 14px; line-height: 1.5;\n"
        "       -webkit-font-smoothing: antialiased; }\n"
        "h1 { font-size: 22px; margin: 4px 0 12px; font-weight: 600; }\n"
        "h2 { font-size: 16px; margin: 0 0 10px; font-weight: 600; color: var(--fg); }\n"
        "h3 { font-size: 14px; margin: 8px 0 4px; font-weight: 600; }\n"
        "p { margin: 6px 0; }\n"
        "strong, b { font-weight: bold; }\n"
        "em, i { font-style: italic; }\n"
        "a { color: var(--accent); text-decoration: none; }\n"
        "ul, ol { margin: 6px 0; padding-left: 22px; }\n"
        "li { margin: 3px 0; }\n"
        "button { background: var(--accent); color: #ffffff; border: none;\n"
        "         padding: 7px 16px; border-radius: 6px; font-size: 13px;\n"
        "         font-weight: 600; cursor: pointer; }\n"
        "input, select, textarea { background: var(--card); color: var(--fg);\n"
        "         border: 1px solid var(--border); padding: 6px 10px;\n"
        "         border-radius: 6px; font-size: 13px; }\n"
        "summary { cursor: pointer; }\n"
        "hr { border: none; border-top: 1px solid var(--border); margin: 12px 0; }\n"
        /* shared utility classes */
        ".card { background: var(--card); border: 1px solid var(--border);\n"
        "        border-radius: 10px; padding: 16px 18px; }\n"
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
    out["--bg"] = dark ? "#15171c" : "#f4f5f7";
    out["--fg"] = dark ? "#e6e8ee" : "#1f2430";
    out["--card"] = dark ? "#1f2229" : "#ffffff";
    out["--border"] = dark ? "#2f333c" : "#e2e4ea";
    out["--muted"] = dark ? "#9aa1ae" : "#6b7280";
    out["--accent"] = accent && *accent ? accent : "#3b82f6";
}
