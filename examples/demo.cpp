/* WhaleUI demo: showcase the implemented HTML/CSS feature set.
 *
 * Keys:
 *   T        - toggle light/dark theme
 *   ESC      - quit
 *
 * Shows: box model, flex (row/column/gap/justify), border-radius (background
 * + border follow the corner arcs), buttons/inputs, lists, bold,
 * text-align, opacity, custom properties (var()), @media
 * prefers-color-scheme, overflow clipping, mouse-wheel scrolling,
 * text selection (click + drag), editable controls (input/textarea/
 * contenteditable) with IME text input.
 */

#include "whaleui.h"

#include <cstdio>
#include <string>

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
    "/* --- animation demo (CSS @keyframes + transition) --- */"
    "@keyframes rise { from { opacity: 0; transform: translateY(14px); }"
    "                  to   { opacity: 1; transform: none; } }"
    "@keyframes pulse { 0% { opacity: .35; transform: scale(1); }"
    "                   50% { opacity: 1; transform: scale(1.15); }"
    "                   100% { opacity: .35; transform: scale(1); } }"
    "@keyframes barw { from { width: 10%; } to { width: 90%; } }"
    ".anim-rise { animation: rise .5s cubic-bezier(.22,1,.36,1) both; }"
    ".anim-rise.d1 { animation-delay: .12s; }"
    ".anim-rise.d2 { animation-delay: .24s; }"
    ".anim-pulse { animation: pulse 1.6s ease-in-out infinite; }"
    ".anim-bar { animation: barw 2.4s ease-in-out infinite alternate; }"
    ".anim-card { transition: transform .2s cubic-bezier(.22,1,.36,1); }"
    ".anim-card:hover { transform: translateY(-3px); }"
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
    "<option value=\"browser\">Browser (UA)</option>"
    "</select>"
    "</div>"
    "<p class=\"muted\">下拉选择主题 · T 深浅色 · +/- 文字缩放 · ESC 退出</p>"

    "<div class=\"row\">"
    "<div class=\"card anim-card\">"
    "<h2 class=\"anim-rise\">Animations</h2>"
    "<p class=\"anim-rise d1\">@keyframes rise · cubic-bezier + fill both(错峰)</p>"
    "<div style=\"display:flex;align-items:center;gap:14px;margin:12px 0\">"
    "<span class=\"anim-pulse\" style=\"width:18px;height:18px;"
    "border-radius:50%;background:var(--accent);display:inline-block\"></span>"
    "<span class=\"muted\" style=\"font-size:12px\">pulse · scale + opacity infinite</span>"
    "</div>"
    "<div style=\"height:10px;border-radius:5px;background:var(--border);"
    "overflow:hidden;margin:10px 0\">"
    "<div class=\"anim-bar\" style=\"height:100%;border-radius:5px;"
    "background:var(--accent)\"></div>"
    "</div>"
    "<p class=\"muted\" style=\"font-size:12px\">bar · width 往复(alternate);"
    "hover 卡片上浮(transition transform)</p>"
    "</div>"
    "</div>"

    "<div class=\"row\">"
    "<div class=\"card\">"
    "<h2>Controls</h2>"
    "<p style=\"margin-bottom:10px\"><strong>flex</strong> + 圆角边框(跟随圆弧)</p>"
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
    "<h2>Details &amp; lists</h2>"
    "<details><summary>Expand me</summary><p>hidden body</p></details>"
    "<ol><li>first</li><li>second</li></ol>"
    "</div>"
    "<div class=\"card\">"
    "<h2>Controls</h2>"
    "<p><input type=\"checkbox\" checked> checkbox</p>"
    "<p><input type=\"radio\" name=\"r\" checked> A &nbsp;"
    "<input type=\"radio\" name=\"r\"> B</p>"
    "<p><progress value=\"60\" max=\"100\"></progress></p>"
    "</div>"
    "</div>"

    "<div class=\"row\">"
    "<div class=\"card\" style=\"flex:1\">"
    "<h2>Edit &amp; IME</h2>"
    "<p style=\"margin:2px 0 6px\"><input id=\"name\" value=\"可编辑输入框\" "
    "style=\"width:100%;box-sizing:border-box\"></p>"
    "<p style=\"margin:2px 0 6px\"><textarea style=\"width:100%;height:56px;"
    "box-sizing:border-box\">多行文本&#10;支持 Enter 换行</textarea></p>"
    "<p style=\"margin:2px 0 6px\"><span contenteditable=\"true\" "
    "style=\"border:1px solid var(--border);border-radius:4px;padding:5px 10px;\">"
    "contenteditable 可编辑</span></p>"
    "</div>"
    "<div class=\"card\" style=\"flex:1\">"
    "<h2>Scroll</h2>"
    "<div style=\"overflow:auto;height:120px;background:var(--card);"
    "border:1px solid var(--border);border-radius:4px;padding:8px 10px;\">"
    "滚轮滚动区域<br>line 1<br>line 2<br>line 3<br>line 4<br>line 5<br>"
    "line 6<br>line 7<br>line 8<br>line 9<br>line 10"
    "</div>"
    "<p class=\"muted\" style=\"margin-top:8px\">点击文字选中,拖动扩展;"
    "输入框支持输入法</p>"
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
    "<p class=\"muted\" style=\"margin-top:10px;opacity:0.6\">opacity 0.6 次级文字</p>"
    "</div>"
    "</div>"
    "</div></body></html>";

static void on_theme_select(whaleui_app_t* app, const char* value, void* userdata)
{
    (void)userdata;
    whaleui_app_set_theme_style(app, value);
}

/* key handling lives in the app, not the library */
static float g_text_scale = 1.0f;
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
    } else if (keycode == '=' ) {
        g_text_scale += 0.25f;
        if (g_text_scale > 2.0f) {
            g_text_scale = 1.0f;
        }
        whaleui_app_set_text_scale(app, g_text_scale);
    } else if (keycode == '-') {
        g_text_scale -= 0.25f;
        if (g_text_scale < 1.0f) {
            g_text_scale = 1.0f;
        }
        whaleui_app_set_text_scale(app, g_text_scale);
    } else if (keycode == 'r') {
        /* reduced-motion: pages without JS show their static content
         * (reveal-on-scroll elements become visible) */
        static int g_reduced = 0;
        g_reduced = !g_reduced;
        whaleui_app_set_reduced_motion(app, g_reduced);
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    whaleui_app_t* app = whaleui_app_create();
    if (!app) {
        std::fprintf(stderr, "app create failed\n");
        return 1;
    }
    /* register system fonts so text can render (Segoe UI is the Win11 font).
     * Only the two the demo actually uses: Segoe UI (Latin) + Microsoft YaHei
     * (CJK). Fewer loaded fonts = less memory (see font.cpp). */
    whaleui_font_register("C:/Windows/Fonts/segoeui.ttf");
    whaleui_font_register("C:/Windows/Fonts/msyh.ttc");

    /* the theme dropdown switches the whole UI style; keys are the app's job */
    whaleui_app_set_select_callback(app, on_theme_select, nullptr);
    whaleui_app_set_key_callback(app, on_key, nullptr);

    whaleui_window_t* win = whaleui_window_create(app, "WhaleUI Demo", 800, 680);
    if (!win) {
        std::fprintf(stderr, "window create failed\n");
        return 1;
    }
    if (argc > 1) {
        /* demo <file.html>: render an external HTML file (bare path or
         * file:// URI; relative paths resolve against the working dir) */
        std::string uri = argv[1];
        if (uri.rfind("file://", 0) != 0) {
            uri = "file://" + uri;
        }
        if (whaleui_window_load_uri(win, uri.c_str()) != 0) {
            std::fprintf(stderr, "load %s failed\n", argv[1]);
            return 1;
        }
    } else if (whaleui_window_load_html(win, kHtml) != 0) {
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
