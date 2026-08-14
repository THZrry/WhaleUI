/* WhaleUI demo: showcase the implemented HTML/CSS feature set.
 *
 * Keys:
 *   T        - toggle light/dark theme
 *   ESC      - quit
 *
 * Shows: box model, flex (row/column/gap/justify), border-radius, borders,
 * buttons/inputs, lists, bold, text-align, opacity, custom properties
 * (var()), @media prefers-color-scheme.
 */

#include "whaleui.h"

#include <cstdio>

static const char* kHtml =
    "<html><head><style>"
    ":root { --brand: #7c4dff; --radius: 10px; }"
    "@media (prefers-color-scheme: dark) {"
    "  .theme-note { color: #ffd166; }"
    "}"
    ".app { display: flex; flex-direction: column; gap: 16px; padding: 24px; }"
    ".row { display: flex; gap: 16px; }"
    ".card { background: var(--card); border: 1px solid var(--border);"
    "        border-radius: var(--radius); padding: 16px; flex: 1; }"
    ".card h2 { margin-top: 0; }"
    ".badge { background: var(--brand); color: white; padding: 4px 12px;"
    "         border-radius: 999px; font-size: 12px; display: inline-block; }"
    ".muted { color: var(--muted); font-size: 12px; }"
    ".ta-right { text-align: right; }"
    ".ta-center { text-align: center; }"
    ".clip-box { overflow: hidden; background: var(--accent); color: white;"
    "            padding: 8px; width: 140px; height: 24px; }"
    "</style></head>"
    "<body><div class=\"app\">"
    "<h1>WhaleUI Demo</h1>"
    "<p class=\"muted\">T 切换深浅色 · ESC 退出 · 媒体查询与 var() 已生效</p>"

    "<div class=\"row\">"
    "<div class=\"card\">"
    "<h2>Flex & Cards</h2>"
    "<p>box model + <strong>flex</strong> row/column, gap, justify, "
    "<span class=\"badge\">border-radius</span></p>"
    "<button>Action</button> <input value=\"type here\">"
    "</div>"
    "<div class=\"card\">"
    "<h2>Lists</h2>"
    "<ul><li>item one</li><li>item <strong>two</strong></li><li>item three</li></ul>"
    "<p class=\"ta-right\">right-aligned</p>"
    "<p class=\"ta-center\">centered</p>"
    "</div>"
    "</div>"

    "<div class=\"row\">"
    "<div class=\"card\">"
    "<h2>Custom props & media</h2>"
    "<p>--brand / --radius / --card / --border 来自 :root 与主题变量。</p>"
    "<p class=\"theme-note\">此文字在深色主题下变黄(@media prefers-color-scheme)。</p>"
    "</div>"
    "<div class=\"card\">"
    "<h2>Overflow</h2>"
    "<div class=\"clip-box\">This text is clipped by overflow hidden.</div>"
    "<p class=\"muted\" style=\"margin-top:8px\">opacity 0.6 的次要文字</p>"
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

    whaleui_window_t* win = whaleui_window_create(app, "WhaleUI Demo", 760, 520);
    if (!win) {
        std::fprintf(stderr, "window create failed\n");
        return 1;
    }
    if (whaleui_window_load_html(win, kHtml) != 0) {
        std::fprintf(stderr, "load html failed\n");
        return 1;
    }
    if (whaleui_window_show(win) != 0) {
        std::fprintf(stderr, "window show failed: no SDL_GPU backend "
                             "(needs D3D11/Vulkan/OpenGL)\n");
        return 1;
    }
    whaleui_app_run(app);

    whaleui_window_destroy(win);
    whaleui_app_destroy(app);
    return 0;
}
