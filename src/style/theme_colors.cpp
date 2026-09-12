/* Theme color tables: the light/dark palette of every built-in style.
 *
 * Each palette is a translation of the reference design system's tokens
 * (sources named per style): fluent-css neutral ramp + Win11 Fluent 2
 * semantic colors, Office UI Fabric Core 9.6.1, Material Design 3
 * (mdui color.less), 7.css --w7-*, GNOME Adwaita (adwaita-web),
 * macOS photon.
 *
 * Pairing rule (the thing that is easy to get wrong): every foreground
 * variable is chosen against the surface it sits on -- --fg/--heading on
 * --bg/--card, --field-fg on --field, --btn-fg on --btn-bg,
 * --accent-fg on --accent. Light mode = dark text on a light surface,
 * dark mode = light text on a dark surface.
 *
 * Elevation is written the way the reference systems write it, i.e. as
 * several stacked shadows (ambient + key layer). The engine currently
 * paints only the first layer of a shadow list, so those styles lose the
 * key layer until multi-shadow lands (doc/internal/10-theme-gaps.md).
 * browser / Simple keeps single shadows on purpose: it is the
 * "renders correctly today" style.
 *
 * --edge-light / --edge-dark carry the light and dark side of a border
 * (a real 3D bevel needs per-side border-color, which the engine lacks);
 * --shade is the inset shadow color (gtk).
 *
 * The metrics (fonts, type scale, radii, control sizes) are not here;
 * see the ThemeSpec table in theme.cpp. */

#include "style/theme_internal.h"

#include <cstdio>
#include <cstring>

namespace {

const char* pick_accent(const char* user, const char* fallback)
{
    return (user && *user) ? user : fallback;
}

void put(VarMap& o, const char* key, const char* value)
{
    if (value && *value) {
        o[key] = value;
    }
}

/* ---- fluent：fluent-css neutral 色板 + Fluent 2 语义色 ---- */
void fill_fluent(bool dark, const char* accent, VarMap& o)
{
    put(o, "--bg", dark ? "#0f0f0f" : "#f5f5f5");
    put(o, "--fg", dark ? "#d4d4d4" : "#262626");
    put(o, "--heading", dark ? "#ffffff" : "#171717");
    put(o, "--muted", dark ? "#737373" : "#525252");
    put(o, "--card", dark ? "#262626" : "#ffffff");
    put(o, "--panel", dark ? "#171717" : "#fafafa");
    put(o, "--sidebar", dark ? "#1f1f1f" : "#f0f0f0");
    put(o, "--field", dark ? "#262626" : "#ffffff");
    put(o, "--field-fg", dark ? "#ffffff" : "#171717");
    put(o, "--border", dark ? "#404040" : "#d4d4d4");
    put(o, "--border-strong", dark ? "#737373" : "#a3a3a3");
    put(o, "--edge-light", dark ? "#737373" : "#ffffff");
    put(o, "--edge-dark", dark ? "#1f1f1f" : "#a3a3a3");
    put(o, "--shade", dark ? "rgba(0,0,0,0.3)" : "rgba(0,0,0,0.06)");
    put(o, "--field-border", dark ? "#404040" : "#d4d4d4");
    put(o, "--field-border-hover", dark ? "#737373" : "#a3a3a3");
    put(o, "--accent", pick_accent(accent, dark ? "#4cc2ff" : "#0067c0"));
    put(o, "--accent-fg", dark ? "#0a0a0a" : "#ffffff");
    put(o, "--accent-hover", pick_accent(accent, dark ? "#6ecbff" : "#106ebe"));
    put(o, "--accent-active", pick_accent(accent, dark ? "#2ba0e8" : "#005a9e"));
    put(o, "--link", dark ? "#4cc2ff" : "#0067c0");
    put(o, "--link-hover", dark ? "#80d3ff" : "#004e8c");
    put(o, "--hover", dark ? "rgba(255,255,255,0.08)" : "rgba(0,0,0,0.06)");
    put(o, "--active", dark ? "rgba(255,255,255,0.14)" : "rgba(0,0,0,0.12)");
    put(o, "--selection-bg", dark ? "#4cc2ff" : "#0067c0");
    put(o, "--selection-fg", dark ? "#0a0a0a" : "#ffffff");
    put(o, "--btn-bg", dark ? "#262626" : "#ffffff");
    put(o, "--btn-fg", dark ? "#ffffff" : "#171717");
    put(o, "--btn-border", dark ? "#404040" : "#d4d4d4");
    put(o, "--btn-border-hover", dark ? "#737373" : "#a3a3a3");
    put(o, "--btn-bg-hover", dark ? "#303030" : "#f0f0f0");
    put(o, "--btn-bg-active", dark ? "#3a3a3a" : "#e5e5e5");
    put(o, "--danger", "#c42b1c");
    put(o, "--danger-fg", "#ffffff");
    put(o, "--danger-hover", "#a92216");
    put(o, "--warning", dark ? "#fce100" : "#9d5d00");
    put(o, "--success", dark ? "#6ccb5f" : "#107c10");
    put(o, "--mark-bg", dark ? "#2d4a6b" : "#cfe4f7");
    put(o, "--mark-fg", dark ? "#ffffff" : "#171717");
    put(o, "--focus-ring", dark ? "rgba(76,194,255,0.5)" : "rgba(0,103,192,0.5)");
    put(o, "--password-char", "\xE2\x80\xA2");
    /* Fluent 2 elevation：环境层 0 0 2px + 键层（needs: multi-shadow） */
    const char* amb = dark ? "0.4" : "0.12";
    const char* key = dark ? "0.45" : "0.14";
    char buf[112];
    std::snprintf(buf, sizeof(buf), "0 0 2px rgba(0,0,0,%s), 0 1px 2px rgba(0,0,0,%s)", amb, key);
    put(o, "--shadow-sm", buf);
    std::snprintf(buf, sizeof(buf), "0 0 2px rgba(0,0,0,%s), 0 2px 4px rgba(0,0,0,%s)", amb, key);
    put(o, "--shadow", buf);
    std::snprintf(buf, sizeof(buf), "0 0 2px rgba(0,0,0,%s), 0 4px 8px rgba(0,0,0,%s)", amb, key);
    put(o, "--shadow-md", buf);
    std::snprintf(buf, sizeof(buf), "0 0 2px rgba(0,0,0,%s), 0 8px 16px rgba(0,0,0,%s)", amb, key);
    put(o, "--shadow-lg", buf);
}

/* ---- metro：Fabric Core 9.6.1（themePrimary #0078d4 / neutral*） ---- */
void fill_metro(bool dark, const char* accent, VarMap& o)
{
    put(o, "--bg", dark ? "#1b1a19" : "#ffffff");
    put(o, "--fg", dark ? "#f3f2f1" : "#333333");
    put(o, "--heading", dark ? "#ffffff" : "#212121");
    put(o, "--muted", dark ? "#a19f9d" : "#666666");
    put(o, "--card", dark ? "#252423" : "#ffffff");
    put(o, "--panel", dark ? "#292827" : "#f4f4f4");
    put(o, "--sidebar", dark ? "#292827" : "#f8f8f8");
    put(o, "--field", dark ? "#252423" : "#ffffff");
    put(o, "--field-fg", dark ? "#f3f2f1" : "#333333");
    put(o, "--border", dark ? "#484644" : "#eaeaea");
    put(o, "--border-strong", dark ? "#666666" : "#c8c8c8");
    put(o, "--edge-light", dark ? "#666666" : "#ffffff");
    put(o, "--edge-dark", dark ? "#1b1a19" : "#a6a6a6");
    put(o, "--shade", dark ? "rgba(0,0,0,0.3)" : "rgba(0,0,0,0.06)");
    put(o, "--field-border", dark ? "#8a8886" : "#a6a6a6");
    put(o, "--field-border-hover", dark ? "#b3b0ad" : "#333333");
    put(o, "--accent", pick_accent(accent, dark ? "#4cc2ff" : "#0078d4"));
    put(o, "--accent-fg", dark ? "#1b1a19" : "#ffffff");
    put(o, "--accent-hover", pick_accent(accent, dark ? "#6ecbff" : "#106ebe"));
    put(o, "--accent-active", pick_accent(accent, dark ? "#2ba0e8" : "#005a9e"));
    put(o, "--link", dark ? "#4cc2ff" : "#0078d4");
    put(o, "--link-hover", dark ? "#80d3ff" : "#106ebe");
    put(o, "--hover", dark ? "rgba(255,255,255,0.06)" : "rgba(0,0,0,0.05)");
    put(o, "--active", dark ? "rgba(255,255,255,0.12)" : "rgba(0,0,0,0.1)");
    put(o, "--selection-bg", dark ? "#4cc2ff" : "#0078d4");
    put(o, "--selection-fg", dark ? "#1b1a19" : "#ffffff");
    put(o, "--btn-bg", dark ? "#292827" : "#f4f4f4");
    put(o, "--btn-fg", dark ? "#f3f2f1" : "#333333");
    put(o, "--btn-border", "transparent");
    put(o, "--btn-border-hover", "transparent");
    put(o, "--btn-bg-hover", dark ? "#33312e" : "#eaeaea");
    put(o, "--btn-bg-active", dark ? "#3d3a37" : "#dadada");
    put(o, "--danger", "#d13438");
    put(o, "--danger-fg", "#ffffff");
    put(o, "--danger-hover", "#a4262c");
    put(o, "--warning", dark ? "#f2c94c" : "#9d5d00");
    put(o, "--success", dark ? "#6ccb5f" : "#107c10");
    put(o, "--mark-bg", dark ? "#0f4a75" : "#deecf9");
    put(o, "--mark-fg", dark ? "#ffffff" : "#333333");
    put(o, "--focus-ring", "rgba(0,120,212,0.4)");
    put(o, "--password-char", "\xE2\x80\xA2");
    put(o, "--shadow-sm", "none");
    put(o, "--shadow", "none");
    put(o, "--shadow-md", "none");
    put(o, "--shadow-lg", "none");
}

/* ---- material：M3 token（mdui color.less 的 RGB 三元组） ---- */
void fill_material(bool dark, const char* accent, VarMap& o)
{
    put(o, "--bg", dark ? "#141218" : "#fef7ff");
    put(o, "--fg", dark ? "#e6e1e5" : "#1c1b1f");
    put(o, "--heading", dark ? "#e6e1e5" : "#1c1b1f");
    put(o, "--muted", dark ? "#cac4d0" : "#49454f");
    put(o, "--card", dark ? "#211f26" : "#f3edf7");
    put(o, "--panel", dark ? "#1d1b20" : "#f7f2fa");
    put(o, "--sidebar", dark ? "#1d1b20" : "#f7f2fa");
    put(o, "--field", dark ? "#2b2930" : "#ffffff");
    put(o, "--field-fg", dark ? "#e6e1e5" : "#1c1b1f");
    put(o, "--border", dark ? "#444746" : "#c4c7c5");
    put(o, "--border-strong", dark ? "#938f99" : "#79747e");
    put(o, "--edge-light", dark ? "#938f99" : "#ffffff");
    put(o, "--edge-dark", dark ? "#1d1b20" : "#79747e");
    put(o, "--shade", dark ? "rgba(0,0,0,0.3)" : "rgba(0,0,0,0.06)");
    put(o, "--field-border", dark ? "#8a8894" : "#79747e");
    put(o, "--field-border-hover", dark ? "#cac4d0" : "#49454e");
    put(o, "--accent", pick_accent(accent, dark ? "#d0bcff" : "#6750a4"));
    put(o, "--accent-fg", dark ? "#371e73" : "#ffffff");
    put(o, "--accent-hover", pick_accent(accent, dark ? "#eaddff" : "#7561ac"));
    put(o, "--accent-active", pick_accent(accent, dark ? "#c4b0f0" : "#5c4a94"));
    put(o, "--link", dark ? "#d0bcff" : "#6750a4");
    put(o, "--link-hover", dark ? "#e8def8" : "#4f378b");
    put(o, "--hover", dark ? "rgba(255,255,255,0.08)" : "rgba(0,0,0,0.08)");
    put(o, "--active", dark ? "rgba(255,255,255,0.12)" : "rgba(0,0,0,0.12)");
    put(o, "--selection-bg", dark ? "#d0bcff" : "#6750a4");
    put(o, "--selection-fg", dark ? "#371e73" : "#ffffff");
    put(o, "--btn-bg", dark ? "#d0bcff" : "#6750a4");
    put(o, "--btn-fg", dark ? "#371e73" : "#ffffff");
    put(o, "--btn-border", "transparent");
    put(o, "--btn-border-hover", "transparent");
    put(o, "--btn-bg-hover", dark ? "#eaddff" : "#7561ac");
    put(o, "--btn-bg-active", dark ? "#c4b0f0" : "#5c4a94");
    put(o, "--danger", dark ? "#f2b8b5" : "#b3261e");
    put(o, "--danger-fg", dark ? "#601410" : "#ffffff");
    put(o, "--danger-hover", dark ? "#f9dedc" : "#8c1d18");
    put(o, "--warning", dark ? "#e8b931" : "#7d5700");
    put(o, "--success", dark ? "#6dd58c" : "#146c2e");
    put(o, "--mark-bg", dark ? "#4f378b" : "#e8def8");
    put(o, "--mark-fg", dark ? "#eaddff" : "#1c1b1f");
    put(o, "--focus-ring", "rgba(103,80,164,0.4)");
    put(o, "--password-char", "\xE2\x80\xA2");
    /* M3 elevation level1..4：深色下阴影淡一档（needs: multi-shadow） */
    put(o, "--shadow-sm", dark
            ? "0 1px 2px rgba(0,0,0,0.5), 0 0 1px rgba(0,0,0,0.3)"
            : "0 0.5px 1.5px rgba(0,0,0,0.19), 0 0 1px rgba(0,0,0,0.039)");
    put(o, "--shadow", dark
            ? "0 1px 3px rgba(0,0,0,0.55), 0 0 1px rgba(0,0,0,0.3)"
            : "0 0.85px 3px rgba(0,0,0,0.19), 0 0.25px 1px rgba(0,0,0,0.039)");
    put(o, "--shadow-md", dark
            ? "0 2px 5px rgba(0,0,0,0.6), 0 0 1.5px rgba(0,0,0,0.3)"
            : "0 1.25px 5px rgba(0,0,0,0.19), 0 0.3333px 1.5px rgba(0,0,0,0.039)");
    put(o, "--shadow-lg", dark
            ? "0 3px 8px rgba(0,0,0,0.65), 0 0.5px 2px rgba(0,0,0,0.3)"
            : "0 1.85px 6.25px rgba(0,0,0,0.19), 0 0.5px 1.75px rgba(0,0,0,0.039)");
}

/* ---- aero：7.css --w7-* token（el-bg #f2f2f2 / el-bd #8e8f8f / 三段渐变） ---- */
void fill_aero(bool dark, const char* accent, VarMap& o)
{
    put(o, "--bg", dark ? "#1b1b1b" : "#f0f0f0");
    put(o, "--fg", dark ? "#e0e8f0" : "#000000");
    put(o, "--heading", dark ? "#ffffff" : "#000000");
    put(o, "--muted", dark ? "#93a1b0" : "#6d6d6d");
    put(o, "--card", dark ? "#26282c" : "#ffffff");
    put(o, "--panel", dark ? "#2a2d33" : "#f0f0f0");
    put(o, "--sidebar", dark ? "#2a2d33" : "#e8eef5");
    put(o, "--field", dark ? "#20242a" : "#ffffff");
    put(o, "--field-fg", dark ? "#e0e8f0" : "#000000");
    put(o, "--border", dark ? "#3d444e" : "#8e8f8f");
    put(o, "--border-strong", dark ? "#4d5663" : "#6d91ab");
    put(o, "--edge-light", dark ? "#6d91ab" : "#ffffff");
    put(o, "--edge-dark", dark ? "#20242a" : "#8e8f8f");
    put(o, "--shade", dark ? "rgba(0,0,0,0.3)" : "rgba(0,0,0,0.08)");
    put(o, "--field-border", dark ? "#4d5663" : "#abadb3");
    put(o, "--field-border-hover", dark ? "#5f6b7a" : "#5794bf");
    put(o, "--accent", pick_accent(accent, dark ? "#4fc1ff" : "#3c7fb1"));
    put(o, "--accent-fg", dark ? "#0b1a26" : "#ffffff");
    put(o, "--accent-hover", pick_accent(accent, dark ? "#7ad2ff" : "#2f6d9a"));
    put(o, "--accent-active", pick_accent(accent, dark ? "#2ba0e8" : "#26597d"));
    put(o, "--link", dark ? "#4fc1ff" : "#0066cc");
    put(o, "--link-hover", dark ? "#80d3ff" : "#0078d7");
    put(o, "--hover", dark ? "rgba(255,255,255,0.08)" : "rgba(0,0,0,0.05)");
    put(o, "--active", dark ? "rgba(255,255,255,0.14)" : "rgba(0,0,0,0.1)");
    put(o, "--selection-bg", dark ? "#3c7fb1" : "#3399ff");
    put(o, "--selection-fg", "#ffffff");
    put(o, "--btn-bg", dark
            ? "linear-gradient(to bottom, #3a3a3a 45%, #333333 45%, #2a2a2a)"
            : "linear-gradient(to bottom, #f2f2f2 45%, #ebebeb 45%, #cfcfcf)");
    put(o, "--btn-fg", dark ? "#e0e0e0" : "#000000");
    put(o, "--btn-border", dark ? "#555555" : "#8e8f8f");
    put(o, "--btn-border-hover", dark ? "#4fc1ff" : "#3c7fb1");
    put(o, "--btn-bg-hover", dark
            ? "linear-gradient(to bottom, #1f4a66 45%, #17384f 45%, #12303f)"
            : "linear-gradient(to bottom, #eaf6fd 45%, #bee6fd 45%, #a7d9f5)");
    put(o, "--btn-bg-active", dark
            ? "linear-gradient(to bottom, #12303f, #17384f 30%, #1f4a66 50%, #2a6a8f)"
            : "linear-gradient(to bottom, #e5f4fc, #c4e5f6 30%, #98d1ef 50%, #68b3db)");
    put(o, "--danger", "#bf1d1d");
    put(o, "--danger-fg", "#ffffff");
    put(o, "--danger-hover", "#a01818");
    put(o, "--warning", dark ? "#f2c94c" : "#9c6e03");
    put(o, "--success", dark ? "#6ccb5f" : "#107c10");
    put(o, "--mark-bg", dark ? "#23435a" : "#eaf6fd");
    put(o, "--mark-fg", dark ? "#ffffff" : "#000000");
    put(o, "--focus-ring", "rgba(60,127,177,0.5)");
    put(o, "--password-char", "\xE2\x80\xA2");
    /* Win7 窗口/菜单阴影：1px 深描边 + 一层环境模糊（needs: multi-shadow） */
    put(o, "--shadow-sm", dark
            ? "0 0 0 1px rgba(0,0,0,0.5), 0 1px 2px rgba(0,0,0,0.5)"
            : "0 0 0 1px rgba(0,0,0,0.08), 0 1px 2px rgba(0,0,0,0.15)");
    put(o, "--shadow", dark
            ? "0 0 0 1px rgba(0,0,0,0.5), 0 2px 6px rgba(0,0,0,0.55)"
            : "0 0 0 1px rgba(0,0,0,0.08), 0 2px 6px rgba(0,0,0,0.2)");
    put(o, "--shadow-md", dark
            ? "0 0 0 1px rgba(0,0,0,0.5), 0 4px 10px rgba(0,0,0,0.6)"
            : "0 0 0 1px rgba(0,0,0,0.08), 0 4px 10px rgba(0,0,0,0.22)");
    put(o, "--shadow-lg", dark
            ? "0 0 0 1px rgba(0,0,0,0.5), 0 6px 16px rgba(0,0,0,0.65)"
            : "0 0 0 1px rgba(0,0,0,0.08), 0 6px 16px rgba(0,0,0,0.26)");
}

/* ---- gtk：Adwaita（window-bg / view-bg / accent #3584e4） ---- */
void fill_gtk(bool dark, const char* accent, VarMap& o)
{
    put(o, "--bg", dark ? "#242424" : "#fafafa");
    put(o, "--fg", dark ? "#ffffff" : "#1b1b1b");
    put(o, "--heading", dark ? "#ffffff" : "#1b1b1b");
    put(o, "--muted", dark ? "#9a9996" : "#5e5c64");
    put(o, "--card", dark ? "#323232" : "#ffffff");
    put(o, "--panel", dark ? "#2e2e2e" : "#ffffff");
    put(o, "--sidebar", dark ? "#282828" : "#ebebeb");
    put(o, "--field", dark ? "#1e1e1e" : "#ffffff");
    put(o, "--field-fg", dark ? "#ffffff" : "#1b1b1b");
    put(o, "--border", dark ? "#444444" : "#deddda");
    put(o, "--border-strong", dark ? "#5e5e5e" : "#c0bfbc");
    put(o, "--edge-light", dark ? "#6e6e6e" : "#ffffff");
    put(o, "--edge-dark", dark ? "#1e1e1e" : "#c0bfbc");
    /* Adwaita --shade-color: rgba(0,0,0,0.07) 浅色 / 更深 深色 */
    put(o, "--shade", dark ? "rgba(0,0,0,0.3)" : "rgba(0,0,0,0.07)");
    put(o, "--field-border", dark ? "#5e5e5e" : "#c0bfbc");
    put(o, "--field-border-hover", dark ? "#808080" : "#9a9996");
    put(o, "--accent", pick_accent(accent, "#3584e4"));
    put(o, "--accent-fg", "#ffffff");
    put(o, "--accent-hover", pick_accent(accent, "#2f77cd"));
    put(o, "--accent-active", pick_accent(accent, "#2a6ab6"));
    put(o, "--link", dark ? "#78aeed" : "#1c71d8");
    put(o, "--link-hover", dark ? "#9ec2f2" : "#1a5fb4");
    put(o, "--hover", dark ? "rgba(255,255,255,0.06)" : "rgba(0,0,0,0.05)");
    put(o, "--active", dark ? "rgba(255,255,255,0.12)" : "rgba(0,0,0,0.1)");
    put(o, "--selection-bg", "#3584e4");
    put(o, "--selection-fg", "#ffffff");
    put(o, "--btn-bg", dark ? "#3d3d3d" : "#fafafa");
    put(o, "--btn-fg", dark ? "#ffffff" : "#1b1b1b");
    put(o, "--btn-border", dark ? "#4f4f4f" : "#d5d5d5");
    put(o, "--btn-border-hover", dark ? "#6e6e6e" : "#b8b8b6");
    put(o, "--btn-bg-hover", dark ? "#4a4a4a" : "#e9e9e7");
    put(o, "--btn-bg-active", dark ? "#565656" : "#dcdcda");
    put(o, "--danger", "#e01b24");
    put(o, "--danger-fg", "#ffffff");
    put(o, "--danger-hover", "#c01c28");
    put(o, "--warning", dark ? "#e5a50a" : "#c07d05");
    put(o, "--success", dark ? "#26a56b" : "#1b8553");
    put(o, "--mark-bg", dark ? "#1c3f6e" : "#cfe0f7");
    put(o, "--mark-fg", dark ? "#ffffff" : "#1b1b1b");
    put(o, "--focus-ring", "rgba(53,132,228,0.4)");
    put(o, "--password-char", "\xE2\x80\xA2");
    /* Adwaita：subtle / card / popover 三档（needs: multi-shadow） */
    put(o, "--shadow-sm", dark
            ? "0 1px 2px rgba(0,0,0,0.5), 0 1px 1px rgba(0,0,0,0.4)"
            : "0 1px 2px rgba(0,0,0,0.04), 0 1px 1px rgba(0,0,0,0.03)");
    put(o, "--shadow", dark
            ? "0 1px 3px rgba(0,0,0,0.55), 0 2px 5px rgba(0,0,0,0.45)"
            : "0 1px 3px rgba(0,0,0,0.08), 0 2px 5px rgba(0,0,0,0.06)");
    put(o, "--shadow-md", dark
            ? "0 2px 6px rgba(0,0,0,0.6), 0 1px 2px rgba(0,0,0,0.5)"
            : "0 2px 6px rgba(0,0,0,0.1), 0 1px 2px rgba(0,0,0,0.08)");
    put(o, "--shadow-lg", dark
            ? "0 2px 8px rgba(0,0,0,0.65), 0 1px 2px rgba(0,0,0,0.5)"
            : "0 2px 8px rgba(0,0,0,0.15), 0 1px 2px rgba(0,0,0,0.1)");
}

/* ---- macos：photon（primary #3b99fc / border #ddd / #c2c0c2 / #a19fa1） ---- */
void fill_macos(bool dark, const char* accent, VarMap& o)
{
    put(o, "--bg", dark ? "#1e1e1e" : "#ececec");
    put(o, "--fg", dark ? "#f0f0f0" : "#333333");
    put(o, "--heading", dark ? "#ffffff" : "#333333");
    put(o, "--muted", dark ? "#9c9c9c" : "#808080");
    put(o, "--card", dark ? "#2c2c2c" : "#ffffff");
    put(o, "--panel", dark ? "#333333" : "#e8e6e8");
    put(o, "--sidebar", dark ? "#333333" : "#f5f5f4");
    put(o, "--field", dark ? "#2c2c2c" : "#ffffff");
    put(o, "--field-fg", dark ? "#f0f0f0" : "#333333");
    put(o, "--border", dark ? "#3f3f3f" : "#d0d0d0");
    put(o, "--border-strong", dark ? "#5c5c5c" : "#c2c0c2");
    put(o, "--edge-light", dark ? "#6b6b6b" : "#ffffff");
    put(o, "--edge-dark", dark ? "#1e1e1e" : "#a19fa1");
    put(o, "--shade", dark ? "rgba(0,0,0,0.3)" : "rgba(0,0,0,0.06)");
    put(o, "--field-border", dark ? "#4a4a4a" : "#dddddd");
    put(o, "--field-border-hover", dark ? "#6b6b6b" : "#a8a8a8");
    put(o, "--accent", pick_accent(accent, dark ? "#0a84ff" : "#3b99fc"));
    put(o, "--accent-fg", "#ffffff");
    put(o, "--accent-hover", pick_accent(accent, dark ? "#2e95ff" : "#1a82fb"));
    put(o, "--accent-active", pick_accent(accent, dark ? "#0070e0" : "#116cd6"));
    put(o, "--link", dark ? "#4da3ff" : "#007aff");
    put(o, "--link-hover", dark ? "#7abaff" : "#0066d6");
    put(o, "--hover", dark ? "rgba(255,255,255,0.06)" : "rgba(0,0,0,0.05)");
    put(o, "--active", dark ? "rgba(255,255,255,0.12)" : "rgba(0,0,0,0.1)");
    put(o, "--selection-bg", dark ? "#0a84ff" : "#007aff");
    put(o, "--selection-fg", "#ffffff");
    put(o, "--btn-bg", dark
            ? "linear-gradient(to bottom, #4a4a4a, #3f3f3f)"
            : "linear-gradient(to bottom, #fcfcfc, #f1f1f1)");
    put(o, "--btn-fg", dark ? "#f0f0f0" : "#333333");
    put(o, "--btn-border", dark ? "#555555" : "#c2c0c2");
    put(o, "--btn-border-hover", dark ? "#6b6b6b" : "#a19fa1");
    put(o, "--btn-bg-hover", dark
            ? "linear-gradient(to bottom, #555555, #4a4a4a)"
            : "linear-gradient(to bottom, #ffffff, #f5f5f5)");
    put(o, "--btn-bg-active", dark ? "#3a3a3a" : "#dddddd");
    put(o, "--danger", "#e04b46");
    put(o, "--danger-fg", "#ffffff");
    put(o, "--danger-hover", "#c93b36");
    put(o, "--warning", dark ? "#fdbc40" : "#e0a02f");
    put(o, "--success", dark ? "#2aa83c" : "#34c84a");
    put(o, "--mark-bg", dark ? "#1c3f6e" : "#cfe4ff");
    put(o, "--mark-fg", dark ? "#ffffff" : "#333333");
    put(o, "--focus-ring", "rgba(59,153,252,0.4)");
    put(o, "--password-char", "\xE2\x80\xA2");
    /* photon：按钮 0 1px 1px，窗口/popover 再叠一层大模糊（needs: multi-shadow） */
    put(o, "--shadow-sm", dark
            ? "0 1px 1px rgba(0,0,0,0.5)"
            : "0 1px 1px rgba(0,0,0,0.06)");
    put(o, "--shadow", dark
            ? "0 1px 1px rgba(0,0,0,0.5), 0 2px 6px rgba(0,0,0,0.45)"
            : "0 1px 1px rgba(0,0,0,0.06), 0 2px 6px rgba(0,0,0,0.1)");
    put(o, "--shadow-md", dark
            ? "0 1px 1px rgba(0,0,0,0.5), 0 4px 12px rgba(0,0,0,0.5)"
            : "0 1px 1px rgba(0,0,0,0.06), 0 4px 12px rgba(0,0,0,0.12)");
    put(o, "--shadow-lg", dark
            ? "0 2px 4px rgba(0,0,0,0.5), 0 8px 24px rgba(0,0,0,0.55)"
            : "0 2px 4px rgba(0,0,0,0.08), 0 8px 24px rgba(0,0,0,0.16)");
}

/* ---- browser (Simple)：黑白两套基调 + 一点主题蓝 ----
 * 单阴影：本主题只用引擎当前支持的写法。 */
void fill_browser(bool dark, const char* accent, VarMap& o)
{
    put(o, "--bg", dark ? "#1e1e1e" : "#ffffff");
    put(o, "--fg", dark ? "#e6e6e6" : "#1a1a1a");
    put(o, "--heading", dark ? "#f0f0f0" : "#1a1a1a");
    put(o, "--muted", dark ? "#9d9d9d" : "#5f6368");
    put(o, "--card", dark ? "#252526" : "#ffffff");
    put(o, "--panel", dark ? "#2a2a2a" : "#f7f7f7");
    put(o, "--sidebar", dark ? "#2a2a2a" : "#f7f7f7");
    put(o, "--field", dark ? "#252526" : "#ffffff");
    put(o, "--field-fg", dark ? "#e6e6e6" : "#1a1a1a");
    put(o, "--border", dark ? "#4a4a4a" : "#d0d0d0");
    put(o, "--border-strong", dark ? "#666666" : "#a8a8a8");
    put(o, "--edge-light", dark ? "#808080" : "#ffffff");
    put(o, "--edge-dark", dark ? "#1a1a1a" : "#8a8a8a");
    put(o, "--shade", dark ? "rgba(0,0,0,0.3)" : "rgba(0,0,0,0.06)");
    put(o, "--field-border", dark ? "#4a4a4a" : "#d0d0d0");
    put(o, "--field-border-hover", dark ? "#666666" : "#a8a8a8");
    put(o, "--accent", pick_accent(accent, dark ? "#4d94ff" : "#0066cc"));
    put(o, "--accent-fg", "#ffffff");
    put(o, "--accent-hover", pick_accent(accent, dark ? "#7ab0ff" : "#0073e6"));
    put(o, "--accent-active", pick_accent(accent, dark ? "#3d82e6" : "#0052a3"));
    put(o, "--link", dark ? "#4d94ff" : "#0066cc");
    put(o, "--link-hover", dark ? "#7ab0ff" : "#0052a3");
    put(o, "--hover", dark ? "rgba(255,255,255,0.06)" : "rgba(0,0,0,0.05)");
    put(o, "--active", dark ? "rgba(255,255,255,0.12)" : "rgba(0,0,0,0.1)");
    put(o, "--selection-bg", dark ? "#264f78" : "#0066cc");
    put(o, "--selection-fg", "#ffffff");
    put(o, "--btn-bg", dark ? "#3a3a3a" : "#ffffff");
    put(o, "--btn-fg", dark ? "#e6e6e6" : "#1a1a1a");
    put(o, "--btn-border", dark ? "#565656" : "#d0d0d0");
    put(o, "--btn-border-hover", dark ? "#6b6b6b" : "#a8a8a8");
    put(o, "--btn-bg-hover", dark ? "#454545" : "#f2f2f2");
    put(o, "--btn-bg-active", dark ? "#505050" : "#e8e8e8");
    put(o, "--danger", "#c62222");
    put(o, "--danger-fg", "#ffffff");
    put(o, "--danger-hover", "#a81b1b");
    put(o, "--warning", dark ? "#f2c94c" : "#9a6700");
    put(o, "--success", dark ? "#57ab5a" : "#1a7f37");
    put(o, "--mark-bg", dark ? "#2b4a6f" : "#d7e8fa");
    put(o, "--mark-fg", dark ? "#ffffff" : "#1a1a1a");
    put(o, "--focus-ring", "rgba(0,102,204,0.4)");
    put(o, "--password-char", "\xE2\x80\xA2");
    put(o, "--shadow-sm", dark ? "0 1px 2px rgba(0,0,0,0.4)" : "0 1px 2px rgba(0,0,0,0.08)");
    put(o, "--shadow", dark ? "0 1px 4px rgba(0,0,0,0.45)" : "0 1px 4px rgba(0,0,0,0.1)");
    put(o, "--shadow-md", dark ? "0 2px 8px rgba(0,0,0,0.5)" : "0 2px 8px rgba(0,0,0,0.12)");
    put(o, "--shadow-lg", dark ? "0 4px 14px rgba(0,0,0,0.55)" : "0 4px 14px rgba(0,0,0,0.14)");
}

} // namespace

void whaleui_theme_palette(const char* style, int dark, const char* accent,
                           VarMap& out)
{
    const char* s = style ? style : "fluent";
    bool d = dark != 0;
    if (std::strcmp(s, "browser") == 0) {
        fill_browser(d, accent, out);
    } else if (std::strcmp(s, "metro") == 0) {
        fill_metro(d, accent, out);
    } else if (std::strcmp(s, "material") == 0) {
        fill_material(d, accent, out);
    } else if (std::strcmp(s, "aero") == 0) {
        fill_aero(d, accent, out);
    } else if (std::strcmp(s, "gtk") == 0) {
        fill_gtk(d, accent, out);
    } else if (std::strcmp(s, "macos") == 0) {
        fill_macos(d, accent, out);
    } else { /* fluent (default) */
        fill_fluent(d, accent, out);
    }
}
