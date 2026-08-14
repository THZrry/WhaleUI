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

#include <SDL3/SDL.h>

#include <cstdio>

static const char* kHtml =
    "<html><head><style>"
    ":root { --brand: #6366f1; --radius: 12px; }"
    "@media (prefers-color-scheme: dark) {"
    "  .theme-note { color: #fbbf24; }"
    "}"
    ".app { display: flex; flex-direction: column; gap: 14px; padding: 28px; }"
    ".header { display: flex; justify-content: space-between;"
    "          align-items: center; }"
    ".row { display: flex; gap: 14px; }"
    ".card { background: var(--card); border: 1px solid var(--border);"
    "        border-radius: var(--radius); padding: 16px 18px; flex: 1; }"
    ".card h2 { margin-top: 0; }"
    ".badge { background: var(--brand); color: white; padding: 3px 10px;"
    "         border-radius: 999px; font-size: 11px; font-weight: 600;"
    "         display: inline-block; }"
    ".ta-right { text-align: right; }"
    ".ta-center { text-align: center; }"
    ".clip-box { overflow: hidden; background: var(--accent); color: white;"
    "            padding: 8px 10px; border-radius: 6px; width: 150px;"
    "            height: 26px; font-size: 12px; }"
    "</style></head>"
    "<body><div class=\"app\">"

    "<div class=\"header\">"
    "<h1>WhaleUI Demo</h1>"
    "<span class=\"badge\">GPU · D3D12</span>"
    "</div>"
    "<p class=\"muted\">T 切换深浅色 · ESC 退出 · var()/@media 已生效</p>"

    "<div class=\"row\">"
    "<div class=\"card\">"
    "<h2>Controls</h2>"
    "<p style=\"margin-bottom:10px\"><strong>flex</strong> + 圆角 + 边框(圆角跟随弧线)</p>"
    "<button>Primary</button> <button style=\"background:var(--muted)\">Ghost</button>"
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
    "<h2>Custom props & media</h2>"
    "<p>--brand / --radius 来自 :root;--card / --border / --muted 来自主题变量。</p>"
    "<p class=\"theme-note\">深色主题下此文字变黄(@media prefers-color-scheme)。</p>"
    "</div>"
    "<div class=\"card\">"
    "<h2>Overflow</h2>"
    "<div class=\"clip-box\">long text clipped by overflow hidden</div>"
    "<p class=\"muted\" style=\"margin-top:10px;opacity:0.6\">opacity 0.6 次要文字</p>"
    "</div>"
    "</div>"
    "</div></body></html>";

int main(void)
{
    whaleui_app_t* app = whaleui_app_create();
    if (!app) {
        std::fprintf(stderr, "app create failed\n");
        return 1;
    }
    /* register system fonts so text can render */
    whaleui_font_register("C:/Windows/Fonts/arial.ttf");
    whaleui_font_register("C:/Windows/Fonts/msyh.ttc");

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
        std::fprintf(stderr, "window show failed: %s\n", SDL_GetError());
        return 1;
    }
    whaleui_app_run(app);

    whaleui_window_destroy(win);
    whaleui_app_destroy(app);
    return 0;
}
