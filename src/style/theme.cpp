/* Theme styles: Fluent / Metro / Material / Classic / Aero / GTK / macOS.
 * Each style is a default stylesheet (shared structure, per-style control
 * radius + colors) plus a light/dark variable table. */

#include "style/theme.h"

#include <cstdio>
#include <cstring>

namespace {

struct ThemeDef
{
    const char* name;
    const char* label;
    int btn_radius;   /* button border-radius px */
    int in_radius;    /* input border-radius px */
    int card_radius;  /* card border-radius px */
};

const ThemeDef kThemes[] = {
    {"fluent",   "Fluent (Win11)",   6, 6, 8},
    {"metro",    "Metro (Win8)",     0, 0, 0},
    {"material", "Material Design",  4, 4, 8},
    {"classic",  "Windows Classic",  0, 0, 0},
    {"aero",     "Windows 7 Aero",   0, 2, 4},
    {"gtk",      "GTK (Adwaita)",    5, 5, 6},
    {"macos",    "macOS",            6, 6, 10},
};
const int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));

const ThemeDef* find_def(const char* name)
{
    for (int i = 0; i < kThemeCount; ++i) {
        if (std::strcmp(kThemes[i].name, name) == 0) {
            return &kThemes[i];
        }
    }
    return &kThemes[0];
}

void base_css(const ThemeDef& d, std::string& css)
{
    char rad[512];
    css =
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
        "summary { cursor: pointer; }\n"
        "option { display: none; }\n"  /* options live in the select popup */
        "hr { border: none; border-top: 1px solid var(--border); margin: 12px 0; }\n";
    std::snprintf(rad, sizeof(rad),
        "button { background: var(--btn-bg); color: var(--btn-fg);\n"
        "         border: none; padding: 6px 16px; border-radius: %dpx;\n"
        "         font-size: 13px; font-weight: 600; cursor: pointer; }\n"
        "button:hover { background: var(--btn-bg-hover); }\n"
        "a:hover { text-decoration: underline; }\n"
        "input, select, textarea { background: var(--field); color: var(--fg);\n"
        "         border: 1px solid var(--border); padding: 5px 10px;\n"
        "         border-radius: %dpx; font-size: 13px; }\n",
        d.btn_radius, d.in_radius);
    css += rad;
    std::snprintf(rad, sizeof(rad),
        ".card { background: var(--card); border: 1px solid var(--border);\n"
        "        border-radius: %dpx; padding: 16px 18px; }\n",
        d.card_radius);
    css += rad;
    css +=
        ".row { display: flex; flex-direction: row; }\n"
        ".column { display: flex; flex-direction: column; }\n"
        ".center { display: flex; justify-content: center; align-items: center; }\n"
        ".hidden { display: none; }\n"
        ".muted { color: var(--muted); }\n"
        ".select-open { border-color: var(--accent); }\n";
}

/* per-style light/dark variable tables */
namespace {
/* lighten a #RRGGBB color by mixing 25% white */
void brighten_accent(const std::string& accent, std::string& out)
{
    unsigned int c = 0;
    if (accent[0] != '#' || std::sscanf(accent.c_str() + 1, "%x", &c) != 1) {
        out = accent;
        return;
    }
    unsigned int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r = static_cast<unsigned>((r * 3 + 255) / 4);
    g = static_cast<unsigned>((g * 3 + 255) / 4);
    b = static_cast<unsigned>((b * 3 + 255) / 4);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    out = buf;
}
} // namespace

void fill_vars(const ThemeDef& d, bool dark, const char* accent,
               std::map<std::string, std::string>& out)
{
    out["--accent"] = accent && *accent ? accent : "#0067c0";
    out["--accent-fg"] = "#ffffff";
    brighten_accent(out["--accent"], out["--accent-hover"]);
    /* default surface tokens, overridden per style below */
    out["--bg"] = dark ? "#202020" : "#f3f3f3";
    out["--fg"] = dark ? "#f5f5f5" : "#1a1a1a";
    out["--card"] = dark ? "#2d2d2d" : "#ffffff";
    out["--field"] = dark ? "#262626" : "#ffffff";
    out["--border"] = dark ? "#3d3d3d" : "#d9d9d9";
    out["--muted"] = dark ? "#9f9f9f" : "#767676";
    out["--btn-bg"] = out["--accent"];
    out["--btn-fg"] = "#ffffff";
    out["--btn-bg-hover"] = out["--accent-hover"];

    const char* style = d.name;
    if (std::strcmp(style, "metro") == 0) {
        out["--bg"] = dark ? "#1e1e1e" : "#ffffff";
        out["--fg"] = dark ? "#e6e6e6" : "#111111";
        out["--card"] = dark ? "#2b2b2b" : "#f5f5f5";
        out["--field"] = dark ? "#222222" : "#ffffff";
        out["--border"] = dark ? "#444444" : "#cccccc";
        out["--muted"] = dark ? "#9a9a9a" : "#666666";
        out["--accent"] = accent && *accent ? accent : "#00bcf2";
        out["--btn-bg"] = out["--accent"];
        out["--btn-fg"] = dark ? "#111111" : "#ffffff";
    } else if (std::strcmp(style, "material") == 0) {
        out["--bg"] = dark ? "#121212" : "#fafafa";
        out["--fg"] = dark ? "#e0e0e0" : "#212121";
        out["--card"] = dark ? "#1e1e1e" : "#ffffff";
        out["--field"] = dark ? "#1e1e1e" : "#ffffff";
        out["--border"] = dark ? "#333333" : "#e0e0e0";
        out["--muted"] = dark ? "#9e9e9e" : "#757575";
        out["--accent"] = accent && *accent ? accent : (dark ? "#bb86fc" : "#6200ee");
        out["--btn-bg"] = out["--accent"];
        out["--btn-fg"] = dark ? "#000000" : "#ffffff";
    } else if (std::strcmp(style, "classic") == 0) {
        out["--bg"] = dark ? "#3a3a3a" : "#ece9d8";
        out["--fg"] = dark ? "#e0e0e0" : "#000000";
        out["--card"] = dark ? "#4a4a4a" : "#ffffff";
        out["--field"] = dark ? "#4a4a4a" : "#ffffff";
        out["--border"] = dark ? "#5a5a5a" : "#aca899";
        out["--muted"] = dark ? "#b0b0b0" : "#404040";
        out["--btn-bg"] = dark ? "#5a5a5a" : "#d4d0c8";
        out["--btn-fg"] = dark ? "#e0e0e0" : "#000000";
        out["--btn-bg-hover"] = dark ? "#6a6a6a" : "#e8e4dc";
    } else if (std::strcmp(style, "aero") == 0) {
        out["--bg"] = dark ? "#1b1b1b" : "#eef4fb";
        out["--fg"] = dark ? "#e0e8f0" : "#1a1a1a";
        out["--card"] = dark ? "#26282c" : "#f8fbfe";
        out["--field"] = dark ? "#20242a" : "#ffffff";
        out["--border"] = dark ? "#3d444e" : "#bcd4ec";
        out["--muted"] = dark ? "#93a1b0" : "#5f7a92";
        out["--accent"] = accent && *accent ? accent : "#0078d7";
        out["--btn-bg"] = out["--accent"];
    } else if (std::strcmp(style, "gtk") == 0) {
        out["--bg"] = dark ? "#2e3436" : "#f6f5f4";
        out["--fg"] = dark ? "#eeeeec" : "#2e3436";
        out["--card"] = dark ? "#3a3f41" : "#ffffff";
        out["--field"] = dark ? "#353a3c" : "#ffffff";
        out["--border"] = dark ? "#4a5053" : "#d5d3d0";
        out["--muted"] = dark ? "#a0a6a8" : "#77767b";
        out["--accent"] = accent && *accent ? accent : "#3584e4";
        out["--btn-bg"] = out["--accent"];
    } else if (std::strcmp(style, "macos") == 0) {
        out["--bg"] = dark ? "#1e1e1e" : "#ececec";
        out["--fg"] = dark ? "#f0f0f0" : "#1a1a1a";
        out["--card"] = dark ? "#2c2c2c" : "#ffffff";
        out["--field"] = dark ? "#2c2c2c" : "#ffffff";
        out["--border"] = dark ? "#3f3f3f" : "#d0d0d0";
        out["--muted"] = dark ? "#9c9c9c" : "#808080";
        out["--accent"] = accent && *accent ? accent : "#007aff";
        out["--btn-bg"] = out["--accent"];
    }
    /* fluent (and any future style) keeps the defaults above */
}

} // namespace

extern "C" int whaleui_theme_count(void)
{
    return kThemeCount;
}

extern "C" const char* whaleui_theme_name(int i)
{
    if (i < 0 || i >= kThemeCount) {
        i = 0;
    }
    return kThemes[i].name;
}

extern "C" const char* whaleui_theme_label(int i)
{
    if (i < 0 || i >= kThemeCount) {
        i = 0;
    }
    return kThemes[i].label;
}

extern "C" const char* whaleui_theme_resolve(const char* name)
{
    if (!name) {
        return kThemes[0].name;
    }
    return find_def(name)->name;
}

extern "C" const char* whaleui_theme_default_css(const char* style)
{
    static std::string buf;
    buf.clear();
    base_css(*find_def(style), buf);
    return buf.c_str();
}

extern "C" void whaleui_theme_vars(const char* style, whaleui_theme_t theme,
                                   const char* accent,
                                   std::map<std::string, std::string>& out)
{
    bool dark = theme == WHALEUI_THEME_DARK;
    fill_vars(*find_def(style), dark, accent, out);
}
