/* WhaleUI demo: showcase the implemented HTML/CSS feature set.
 *
 * Keys:
 *   T        - toggle light/dark theme
 *   ESC      - quit
 *
 * Shows: box model, flex (row/column/gap/justify), border-radius (background
 * + border follow the corner arcs), buttons/inputs, lists, bold,
 * text-align, opacity, custom properties (var()), @media
 * prefers-color-scheme, overflow clipping.
 */

#include "whaleui.h"

#include <cstdio>

static const char* kHtml =
    "<html><head><style>"
    "@media (prefers-color-scheme: dark) {"
    "  .theme-note { color: #4cc2ff; }"
    "}"
    ".row .card { flex: 1; }"   /* page structure: cards share the row */
    ".ta-right { text-align: right; }"
    ".ta-center { text-align: center; }"
    ".clip-box { overflow: hidden; background: var(--accent);"
    "            color: var(--accent-fg); padding: 8px 10px;"
    "            border-radius: 6px; width: 150px; height: 26px;"
    "            font-size: 12px; }"
    "#theme { min-width: 150px; }"
    "</style></head>"
    "<body><div class=\"app\">"

    "<div class=\"header\">"
    "<h1>WhaleUI Demo</h1>"
    "<select id=\"theme\">"
    "<option value=\"fluent\">Fluent (Win11)</option>"
    "<option value=\"metro\">Metro (Win8)</option>"
    "<option value=\"material\">Material Design</option>"
    "<option value=\"classic\">Windows Classic</option>"
    "<option value=\"aero\">Windows 7 Aero</option>"
    "<option value=\"gtk\">GTK (Adwaita)</option>"
    "<option value=\"macos\">macOS</option>"
    "</select>"
    "</div>"
    "<p class=\"muted\">下拉选择主题 · T 切换深浅色 · ESC 退出 · "
    "主题由库内置样式提供,页面 CSS 仅做演示</p>"

    "<div class=\"row\">"
    "<div class=\"card\">"
    "<h2>Controls</h2>"
    "<p style=\"margin-bottom:10px\"><strong>flex</strong> + 圆角边框(跟随弧线)</p>"
    "<button>Primary</button> <button style=\"background:var(--muted)\">Secondary</button>"
    "<p style=\"margin-top:10px\"><input value=\"input text\"></p>"
    "</div>"
    "<div class=\"card\">"
    "<h2>Typography</h2>"
    "<ul><li>item <strong>bold</strong></li><li>item two</li><li>item three</li></ul>"
    "<p class=\"ta-right\">right-aligned</p>"
    "<p class=\"ta-center\">centered</p>"
    "</div>"
    "</div>"

    "<div class=\"row\">"
    "<div class=\"card\">"
    "<h2>Theme vars</h2>"
    "<p>--card / --border / --muted / --accent 随主题与深浅色切换,"
    "对未自定义样式的标签同样生效。</p>"
    "<p class=\"theme-note\">深色主题下此文字变亮蓝(@media prefers-color-scheme)。</p>"
    "</div>"
    "<div class=\"card\">"
    "<h2>Overflow</h2>"
    "<div class=\"clip-box\">long text clipped by overflow hidden</div>"
    "<p class=\"muted\" style=\"margin-top:10px;opacity:0.6\">opacity 0.6 次要文字</p>"
    "</div>"
    "</div>"
    "</div></body></html>";

static void on_theme_select(whaleui_app_t* app, const char* value, void* userdata)
{
    (void)userdata;
    whaleui_app_set_theme_style(app, value);
}

/* key handling lives in the app, not the library */
static void on_key(whaleui_app_t* app, int keycode, int pressed, void* userdata)
{
    (void)userdata;
    if (!pressed) {
        return;
    }
    if (keycode == WHALEUI_KEY_ESCAPE) {
        whaleui_app_quit(app);
    } else if (keycode == WHALEUI_KEY_T) {
        whaleui_theme_t cur = whaleui_app_resolved_theme(app);
        whaleui_app_set_theme(app, cur == WHALEUI_THEME_DARK
                                       ? WHALEUI_THEME_LIGHT
                                       : WHALEUI_THEME_DARK);
    }
}

int main(void)
{
    whaleui_app_t* app = whaleui_app_create();
    if (!app) {
        std::fprintf(stderr, "app create failed\n");
        return 1;
    }
    /* register system fonts so text can render (Segoe UI is the Win11 font) */
    whaleui_font_register("C:/Windows/Fonts/segoeui.ttf");
    whaleui_font_register("C:/Windows/Fonts/arial.ttf");
    whaleui_font_register("C:/Windows/Fonts/msyh.ttc");

    /* the theme dropdown switches the whole UI style; keys are the app's job */
    whaleui_app_set_select_callback(app, on_theme_select, nullptr);
    whaleui_app_set_key_callback(app, on_key, nullptr);

    whaleui_window_t* win = whaleui_window_create(app, "WhaleUI Demo", 780, 540);
    if (!win) {
        std::fprintf(stderr, "window create failed\n");
        return 1;
    }
    if (whaleui_window_load_html(win, kHtml) != 0) {
        std::fprintf(stderr, "load html failed\n");
        return 1;
    }
    if (whaleui_window_show(win) != 0) {
        std::fprintf(stderr, "window show failed (needs a GPU backend)\n");
        return 1;
    }
    whaleui_app_run(app);

    whaleui_window_destroy(win);
    whaleui_app_destroy(app);
    return 0;
}
