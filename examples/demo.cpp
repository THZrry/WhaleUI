/* WhaleUI demo: a small HTML page rendered through the full pipeline.
 *
 * Keys:
 *   T        - toggle light/dark theme
 *   ESC      - quit
 */

#include "whaleui.h"

#include <cstdio>
#include <cstring>

static const char* kHtml =
    "<html><head><style>"
    ".app { display: flex; flex-direction: column; gap: 12px; padding: 24px; }"
    ".row { display: flex; gap: 12px; }"
    ".card { background: var(--card); border: 1px solid var(--border);"
    "        border-radius: 8px; padding: 16px; flex: 1; }"
    ".card h2 { margin-top: 0; }"
    ".card .muted { font-size: 12px; }"
    ".accent { background: var(--accent); color: white; padding: 8px 16px;"
    "          border-radius: 4px; display: inline-block; }"
    "</style></head>"
    "<body><div class=\"app\">"
    "<h1>WhaleUI Demo</h1>"
    "<div class=\"row\">"
    "<div class=\"card\">"
    "<h2>Layout</h2>"
    "<p>Box model, flex, themes - all from a tiny HTML+CSS engine.</p>"
    "<span class=\"accent\">Press T to toggle theme</span>"
    "</div>"
    "<div class=\"card\">"
    "<h2>Status</h2>"
    "<p class=\"muted\">Rendering via SDL3 GPU + SDL3_ttf.</p>"
    "<p>ESC quits.</p>"
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
    /* register a system font so text can render */
    whaleui_font_register("C:/Windows/Fonts/arial.ttf");
    whaleui_font_register("C:/Windows/Fonts/msyh.ttc");

    whaleui_window_t* win = whaleui_window_create(app, "WhaleUI Demo", 720, 480);
    if (!win) {
        std::fprintf(stderr, "window create failed\n");
        return 1;
    }
    if (whaleui_window_load_html(win, kHtml) != 0) {
        std::fprintf(stderr, "load html failed\n");
        return 1;
    }
    if (whaleui_window_show(win) != 0) {
        std::fprintf(stderr, "window show failed\n");
        return 1;
    }
    whaleui_app_run(app);

    whaleui_window_destroy(win);
    whaleui_app_destroy(app);
    return 0;
}
