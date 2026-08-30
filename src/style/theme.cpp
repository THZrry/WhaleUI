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
    int btn_radius;   /* button border-radius px (999 = capsule) */
    int in_radius;    /* input border-radius px */
    int card_radius;  /* card border-radius px */
};

const ThemeDef kThemes[] = {
    {"fluent",   "Fluent (Win11)",   4, 4, 8},
    {"metro",    "Metro (Win8)",     0, 0, 0},
    {"material", "Material Design",  999, 4, 12},
    {"classic",  "Windows Classic",  0, 0, 0},
    {"aero",     "Windows 7 Aero",   0, 2, 4},
    {"gtk",      "GTK (Adwaita)",    3, 3, 6},
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
    char rad[2048];
    css =
        "html, body { margin: 0; padding: 0; }\n"
        /* UA stylesheet (WHATWG HTML standard): every tag is a div with
         * tag-specific defaults. display:none for non-rendered metadata and
         * media sources; inline tags keep text flowing in a line. */
        "head, style, script, title, meta, link, base, template, noscript,"
        " source, track, param, area, wbr { display: none; }\n"
        "div, section, article, nav, aside, header, footer, main, address,"
        " p, hr, pre, blockquote, ol, ul, menu, li, dl, dt, dd, figure,"
        " figcaption, table, caption, colgroup, col, thead, tbody, tfoot,"
        " tr, td, th, form, fieldset, details, dialog, picture, map, iframe,"
        " embed, object, video, audio, canvas { display: block; }\n"
        "a, em, strong, small, s, cite, q, dfn, abbr, data, time, code, var,"
        " samp, kbd, sub, sup, i, b, u, mark, ruby, rt, rp, bdi, bdo, span,"
        " label { display: inline; }\n"
        "img, input, button, select, textarea, output, progress, meter,"
        " datalist { display: inline-block; }\n"
        "html { background-color: var(--bg); }\n"
        "body { background-color: var(--bg); color: var(--fg);\n"
        "       font-family: \"Segoe UI\", sans-serif; font-size: 14px;\n"
        "       line-height: 1.5; }\n"
        "h1 { font-size: 24px; margin: 2px 0 12px; font-weight: 600; }\n"
        "h2 { font-size: 17px; margin: 0 0 10px; font-weight: 600; }\n"
        "h3 { font-size: 14px; margin: 8px 0 4px; font-weight: 600; }\n"
        "h4 { font-size: 14px; margin: 8px 0 4px; font-weight: 600; }\n"
        "h5 { font-size: 13px; margin: 8px 0 4px; font-weight: 600; }\n"
        "h6 { font-size: 12px; margin: 8px 0 4px; font-weight: 600; }\n"
        "p { margin: 6px 0; }\n"
        "strong, b { font-weight: 600; }\n"
        "em, i { font-style: italic; }\n"
        "small { font-size: 0.85em; }\n"
        "s { text-decoration: line-through; }\n"
        "u { text-decoration: underline; }\n"
        "mark { background-color: #ffe08a; color: #1a1a1a; }\n"
        "sub, sup { font-size: 0.75em; }\n"
        "code, kbd, samp { font-family: \"Courier New\", monospace;\n"
        "                  font-size: 0.9em; }\n"
        "pre { font-family: \"Courier New\", monospace; font-size: 0.9em;\n"
        "      margin: 6px 0; }\n"
        "var, cite, dfn { font-style: italic; }\n"
        "blockquote { margin: 8px 0 8px 4px; padding: 4px 0 4px 14px;\n"
        "             border-left: 3px solid var(--border);\n"
        "             color: var(--muted); }\n"
        "ul, ol { margin: 6px 0; padding-left: 24px; }\n"
        "li { margin: 3px 0; }\n"
        "dl { margin: 6px 0; }\n"
        "dt { font-weight: 600; }\n"
        "dd { margin: 2px 0 6px 24px; }\n"
        "figure { margin: 8px 0; }\n"
        "figcaption { font-size: 12px; color: var(--muted);\n"
        "             margin-top: 4px; }\n"
        "summary { cursor: pointer; }\n"
        "dialog { display: none; }\n"
        "option { display: none; }\n"  /* options live in the select popup */
        "hr { border: none; border-top: 1px solid var(--border); margin: 12px 0; }\n";
    std::snprintf(rad, sizeof(rad),
        "button { background: var(--btn-bg); color: var(--btn-fg);\n"
        "         border: none; padding: 6px 16px; border-radius: %dpx;\n"
        "         font-size: 13px; font-weight: 600; cursor: pointer;\n"
        "         transition: background-color 120ms, transform 120ms; }\n"
        "button:hover { background: var(--btn-bg-hover); }\n"
        "button:active { background: var(--btn-bg-active);\n"
        "                transform: translateY(1px); }\n"
        "a { color: var(--accent); text-decoration: none;\n"
        "    transition: color 150ms; }\n"
        "a:hover { text-decoration: underline; }\n"
        "input, select, textarea { background: var(--field); color: var(--fg);\n"
        "         border: 1px solid var(--border); padding: 5px 10px;\n"
        "         border-radius: %dpx; font-size: 13px;\n"
        "         transition: border-color 120ms, background-color 120ms; }\n"
        "input:hover, select:hover, textarea:hover { border-color: var(--border-strong); }\n"
        "input:focus, select:focus, textarea:focus { border-color: var(--accent); }\n"
        "textarea { overflow: auto; }\n"
        "select { cursor: pointer; }\n",
        d.btn_radius, d.in_radius);
    css += rad;
    std::snprintf(rad, sizeof(rad),
        ".card { background: var(--card); border: 1px solid var(--border);\n"
        "        border-radius: %dpx; padding: 16px 18px;\n"
        "        box-shadow: var(--shadow);\n"
        "        transition: transform 180ms; }\n"
        ".card:hover { transform: translateY(-2px); }\n",
        d.card_radius);
    css += rad;
    css +=
        ".row { display: flex; flex-direction: row; gap: 12px; }\n"
        ".column { display: flex; flex-direction: column; }\n"
        ".center { display: flex; justify-content: center; align-items: center; }\n"
        ".app { display: flex; flex-direction: column; gap: 14px; padding: 24px 28px; }\n"
        ".header { display: flex; justify-content: space-between; align-items: center; }\n"
        ".badge { background: var(--accent); color: var(--accent-fg);\n"
        "         border-radius: 999px; font-size: 11px; font-weight: 600;\n"
        "         display: inline-block; }\n"
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

/* darken a #RRGGBB color by 15% (pressed button state) */
void darken_accent(const std::string& accent, std::string& out)
{
    unsigned int c = 0;
    if (accent[0] != '#' || std::sscanf(accent.c_str() + 1, "%x", &c) != 1) {
        out = accent;
        return;
    }
    unsigned int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r = static_cast<unsigned>(r * 85 / 100);
    g = static_cast<unsigned>(g * 85 / 100);
    b = static_cast<unsigned>(b * 85 / 100);
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
    out["--border-strong"] = dark ? "#5a5a5a" : "#a0a0a0";
    out["--muted"] = dark ? "#9f9f9f" : "#767676";
    out["--shadow"] = "0 2px 8px rgba(0,0,0,0.12)";
    out["--btn-bg"] = out["--accent"];
    out["--btn-fg"] = "#ffffff";
    out["--btn-bg-hover"] = out["--accent-hover"];
    darken_accent(out["--accent"], out["--btn-bg-active"]);
    char rad[16];
    std::snprintf(rad, sizeof(rad), "%dpx", d.card_radius);
    out["--radius"] = rad;

    const char* style = d.name;
    if (std::strcmp(style, "metro") == 0) {
        out["--bg"] = dark ? "#1e1f22" : "#ffffff";
        out["--fg"] = dark ? "#dbdfe7" : "#191919";
        out["--card"] = dark ? "#1e1f22" : "#ffffff";
        out["--field"] = dark ? "#1e1f22" : "#ffffff";
        out["--border"] = dark ? "#4a4d51" : "#e8e8e8";
        out["--border-strong"] = dark ? "#6a6d72" : "#c0c0c0";
        out["--muted"] = dark ? "#c0c0c0" : "#a2a5b1";
        out["--shadow"] = "none"; /* Metro is flat */
        out["--accent"] = accent && *accent ? accent : "#1fb1f8";
        out["--btn-bg"] = out["--accent"];
        out["--btn-fg"] = "#ffffff";
    } else if (std::strcmp(style, "material") == 0) {
        /* Material Design 3 tokens (mdui/m3.material.io) */
        out["--bg"] = dark ? "#141218" : "#fef7ff";
        out["--fg"] = dark ? "#e6e1e5" : "#1c1b1f";
        out["--card"] = dark ? "#211f26" : "#f3edf7";   /* surface-container */
        out["--field"] = dark ? "#2b2930" : "#ffffff";  /* surface-container-lowest */
        out["--border"] = dark ? "#49454f" : "#c4c7c5"; /* outline-variant */
        out["--border-strong"] = dark ? "#938f99" : "#79747e"; /* outline */
        out["--muted"] = dark ? "#cac4d0" : "#49454f";  /* on-surface-variant */
        out["--shadow"] = "0 4px 12px rgba(0,0,0,0.2)"; /* M3 elevation 2 */
        out["--accent"] = accent && *accent ? accent : (dark ? "#d0bcff" : "#6750a4");
        out["--btn-bg"] = out["--accent"];
        out["--btn-fg"] = dark ? "#381e72" : "#ffffff"; /* on-primary */
        out["--btn-bg-hover"] = dark ? "#e8def8" : "#7a62b4";
        out["--btn-bg-active"] = dark ? "#c2b0e0" : "#5a4a92";
    } else if (std::strcmp(style, "classic") == 0) {
        out["--bg"] = dark ? "#3a3a3a" : "#ece9d8";
        out["--fg"] = dark ? "#e0e0e0" : "#000000";
        out["--card"] = dark ? "#4a4a4a" : "#ffffff";
        out["--field"] = dark ? "#4a4a4a" : "#ffffff";
        out["--border"] = dark ? "#5a5a5a" : "#aca899";
        out["--border-strong"] = dark ? "#6a6a6a" : "#8a8778";
        out["--muted"] = dark ? "#b0b0b0" : "#404040";
        out["--btn-bg"] = dark ? "#5a5a5a" : "#d4d0c8";
        out["--btn-fg"] = dark ? "#e0e0e0" : "#000000";
        out["--btn-bg-hover"] = dark ? "#6a6a6a" : "#e8e4dc";
        out["--btn-bg-active"] = dark ? "#4a4a4a" : "#c0bcb0";
    } else if (std::strcmp(style, "aero") == 0) {
        out["--bg"] = dark ? "#1b1b1b" : "#eef4fb";
        out["--fg"] = dark ? "#e0e8f0" : "#1a1a1a";
        out["--card"] = dark ? "#26282c" : "#f8fbfe";
        out["--field"] = dark ? "#20242a" : "#ffffff";
        out["--border"] = dark ? "#3d444e" : "#bcd4ec";
        out["--border-strong"] = dark ? "#4d5663" : "#8fb4d8";
        out["--muted"] = dark ? "#93a1b0" : "#5f7a92";
        out["--accent"] = accent && *accent ? accent : "#0078d7";
        out["--btn-bg"] = out["--accent"];
    } else if (std::strcmp(style, "gtk") == 0) {
        /* GNOME Adwaita (gtk/theme/Default _colors.scss) */
        out["--bg"] = dark ? "#3a3a3a" : "#f6f5f4";
        out["--fg"] = dark ? "#eeeeec" : "#2e3436";
        out["--card"] = dark ? "#2b2b2b" : "#ffffff";   /* base */
        out["--field"] = dark ? "#2b2b2b" : "#ffffff";
        out["--border"] = dark ? "#4a5053" : "#d5d3d0"; /* darken(bg,18%) */
        out["--border-strong"] = dark ? "#5a6165" : "#bfbcb8"; /* darken(bg,24%) */
        out["--muted"] = dark ? "#a0a6a8" : "#77767b";
        out["--accent"] = accent && *accent ? accent : "#3584e4";
        out["--btn-bg"] = out["--accent"];
    } else if (std::strcmp(style, "macos") == 0) {
        out["--bg"] = dark ? "#1e1e1e" : "#ececec";
        out["--fg"] = dark ? "#f0f0f0" : "#1a1a1a";
        out["--card"] = dark ? "#2c2c2c" : "#ffffff";
        out["--field"] = dark ? "#2c2c2c" : "#ffffff";
        out["--border"] = dark ? "#3f3f3f" : "#d0d0d0";
        out["--border-strong"] = dark ? "#5c5c5c" : "#a8a8a8";
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
