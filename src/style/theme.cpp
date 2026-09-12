/* Theme styles: metrics table, stylesheet assembly and the C API.
 *
 * The stylesheets live in theme_css.cpp (UA baseline + components +
 * per-style overlay) and the light/dark palettes in theme_colors.cpp;
 * this file owns the per-style metrics (font, type scale, radii, control
 * sizes), joins the two into the default stylesheet, and exposes the
 * theme entry points.
 *
 * Style order: index 0 is the default (fluent). Sizes and fonts come from
 * the reference systems - fluent-css --h-default/--rounded-default,
 * Fabric Core button metrics, M3 label-large + shape tokens,
 * 7.css --w7-font/--w7-el-bdr, Adwaita 10pt +
 * --border-radius-default, photon 13px/4px, and the Simple 主题说明
 * (bigger whitespace, plain black/white). */

#include "style/theme.h"
#include "style/theme_internal.h"

#include <cstdio>
#include <cstring>

namespace {

struct ThemeSpec
{
    const char* name;
    const char* label;
    const char* font_sans;
    const char* font_mono;
    const char* line_height;
    int text_sm, text_base, text_lg;
    int h1, h2, h3, h4, h5, h6, h_weight;
    int radius_btn, radius_field, radius_lg, radius_sm, radius_md;
    int btn_h, btn_px, btn_weight, in_h, in_px;
    const char* focus_ring_w;
    const char* disabled_opacity;
};

const ThemeSpec kSpecs[] = {
    /* fluent：fluent-css --h-default 32px / --rounded-default 4px / 字重 600 /
     * --duration-hover 100ms；字号 = Fluent 2 type ramp */
    {"fluent", "Fluent (Win11)",
     "\"Segoe UI\", system-ui, \"Helvetica Neue\", Arial, sans-serif",
     "\"Cascadia Code\", Consolas, monospace",
     "1.43",
     12, 14, 16, 32, 28, 24, 20, 16, 14, 600,
     4, 4, 8, 2, 4,
     32, 12, 600, 32, 12,
     "2px", "0.6"},

    /* metro：Fabric 按钮无圆角无阴影，min-height 32 / padding 0 16 */
    {"metro", "Metro (Win8)",
     "\"Segoe UI\", system-ui, Arial, sans-serif",
     "Consolas, \"Courier New\", monospace",
     "1.5",
     12, 14, 16, 40, 28, 20, 17, 14, 12, 400,
     0, 0, 0, 0, 0,
     32, 16, 400, 32, 8,
     "2px", "0.6"},

    /* material：M3 label-large 14px/500，按钮 40 高、胶囊圆角，
     * shape: text field 4 / card 12 / chip 8 */
    {"material", "Material Design",
     "\"Roboto\", \"Segoe UI\", system-ui, sans-serif",
     "\"Roboto Mono\", Consolas, monospace",
     "1.43",
     12, 14, 16, 32, 28, 24, 22, 16, 14, 400,
     20, 4, 12, 8, 12,
     40, 24, 500, 40, 16,
     "2px", "0.5"},

    /* aero：7.css --w7-font 9pt(12px) / --w7-el-bdr 3px /
     * 按钮 min-height 23px、padding 0 12px */
    {"aero", "Windows 7 Aero",
     "\"Segoe UI\", Tahoma, sans-serif",
     "Consolas, \"Courier New\", monospace",
     "1.3",
     11, 12, 15, 24, 19, 16, 14, 13, 12, 400,
     3, 2, 4, 2, 3,
     23, 12, 400, 23, 4,
     "1px", "0.6"},

    /* gtk：Adwaita 10pt(13px) / --border-radius-default 8px /
     * 按钮 padding 9px 12px、focus ring 2px */
    {"gtk", "GTK (Adwaita)",
     "\"Cantarell\", \"Adwaita Sans\", \"Segoe UI\", sans-serif",
     "\"Adwaita Mono\", \"DejaVu Sans Mono\", monospace",
     "1.4",
     11, 13, 15, 24, 20, 17, 15, 13, 12, 700,
     8, 8, 12, 4, 6,
     34, 12, 600, 34, 12,
     "2px", "0.5"},

    /* macos：photon 13px system / line-height 1.6 / 按钮 padding 3px 8px、
     * 12px 字号 / 输入框 25px 高、focus 3px 外环 */
    {"macos", "macOS",
     "system-ui, -apple-system, \"SF Pro Text\", \"Helvetica Neue\", sans-serif",
     "\"SF Mono\", Menlo, Consolas, monospace",
     "1.6",
     12, 13, 16, 36, 30, 24, 18, 14, 12, 500,
     4, 4, 6, 2, 4,
     26, 10, 400, 25, 10,
     "3px", "0.5"},

    /* browser (Simple)：留白比传统浏览器稍大，字号 15px，字重 600 标题 */
    {"browser", "Browser (Simple)",
     "\"Segoe UI\", system-ui, \"Helvetica Neue\", Arial, sans-serif",
     "\"Cascadia Mono\", Consolas, monospace",
     "1.6",
     13, 15, 18, 28, 23, 19, 16, 14, 13, 600,
     4, 4, 8, 2, 4,
     32, 16, 400, 32, 10,
     "2px", "0.6"},
};
const int kSpecCount = static_cast<int>(sizeof(kSpecs) / sizeof(kSpecs[0]));

const ThemeSpec* find_spec(const char* name)
{
    for (int i = 0; i < kSpecCount; ++i) {
        if (name && std::strcmp(kSpecs[i].name, name) == 0) {
            return &kSpecs[i];
        }
    }
    return &kSpecs[0];
}

void put(VarMap& o, const char* key, const char* value)
{
    if (value && *value) {
        o[key] = value;
    }
}

void put_px(VarMap& o, const char* key, int n)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%dpx", n);
    o[key] = buf;
}

void put_int(VarMap& o, const char* key, int n)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", n);
    o[key] = buf;
}

/* metrics shared by every style */
void put_spec(const ThemeSpec& d, VarMap& o)
{
    put(o, "--font-sans", d.font_sans);
    put(o, "--font-mono", d.font_mono);
    put(o, "--line-height", d.line_height);
    put_px(o, "--text-sm", d.text_sm);
    put_px(o, "--text-base", d.text_base);
    put_px(o, "--text-lg", d.text_lg);
    put_px(o, "--h1", d.h1);
    put_px(o, "--h2", d.h2);
    put_px(o, "--h3", d.h3);
    put_px(o, "--h4", d.h4);
    put_px(o, "--h5", d.h5);
    put_px(o, "--h6", d.h6);
    put_int(o, "--h-weight", d.h_weight);
    put_px(o, "--radius-btn", d.radius_btn);
    put_px(o, "--radius-field", d.radius_field);
    put_px(o, "--radius-lg", d.radius_lg);
    put_px(o, "--radius", d.radius_lg); /* legacy alias */
    put_px(o, "--radius-sm", d.radius_sm);
    put_px(o, "--radius-md", d.radius_md);
    put_px(o, "--btn-h", d.btn_h);
    put_px(o, "--btn-px", d.btn_px);
    put_int(o, "--btn-weight", d.btn_weight);
    put_px(o, "--in-h", d.in_h);
    put_px(o, "--in-px", d.in_px);
    put(o, "--focus-ring-w", d.focus_ring_w);
    put(o, "--disabled-opacity", d.disabled_opacity);
}

/* UA + components + this style's overlay */
void build_css(const ThemeSpec& d, std::string& css)
{
    css = kWhaleuiUaCss;
    css += kWhaleuiComponentsCss;
    css += whaleui_theme_overlay_css(d.name);
}

void fill_vars(const ThemeSpec& d, bool dark, const char* accent, VarMap& out)
{
    put_spec(d, out);
    whaleui_theme_palette(d.name, dark ? 1 : 0, accent, out);
}

} // namespace

extern "C" int whaleui_theme_count(void)
{
    return kSpecCount;
}

extern "C" const char* whaleui_theme_name(int i)
{
    if (i < 0 || i >= kSpecCount) {
        i = 0;
    }
    return kSpecs[i].name;
}

extern "C" const char* whaleui_theme_label(int i)
{
    if (i < 0 || i >= kSpecCount) {
        i = 0;
    }
    return kSpecs[i].label;
}

extern "C" const char* whaleui_theme_resolve(const char* name)
{
    return find_spec(name)->name;
}

extern "C" const char* whaleui_theme_default_css(const char* style)
{
    static std::string buf;
    buf.clear();
    build_css(*find_spec(style), buf);
    return buf.c_str();
}

extern "C" void whaleui_theme_vars(const char* style, whaleui_theme_t theme,
                                   const char* accent,
                                   std::map<std::string, std::string>& out)
{
    fill_vars(*find_spec(style), theme == WHALEUI_THEME_DARK, accent, out);
}
