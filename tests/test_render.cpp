// test_render: paint pipeline via the software renderer path.
// Creates a window, lays out a tiny page, paints one frame and checks
// framebuffer pixels (works headless-ish: SDL software renderer, no GPU).
#include "whaleui.h"
#include "render/render.h"
#include "core/window.h"

#include <cassert>

int main(void)
{
    whaleui_app_t* app = whaleui_app_create();
    assert(app != nullptr);

    whaleui_window_t* win = whaleui_window_create(app, "render-test", 200, 150);
    assert(win != nullptr);

    const char* html =
        "<html><body><div id=\"red\" style=\"width:100px;height:100px;"
        "background-color:#ff0000;\"></div></body></html>";
    assert(whaleui_window_load_html(win, html) == 0);
    assert(whaleui_app_set_theme(app, WHALEUI_THEME_LIGHT) == 0);
    assert(whaleui_window_show(win) == 0);
    assert(win->render != nullptr);

    /* one frame */
    assert(whaleui_render_frame(win->render, win->document) == 0);

    /* red box covers the top-left 100x100 */
    assert(win->render->pixels[0] == 0xFFFF0000);             /* (0,0) */
    assert(win->render->pixels[50 * 200 + 50] == 0xFFFF0000); /* (50,50) */
    assert(win->render->pixels[99 * 200 + 99] == 0xFFFF0000); /* (99,99) */
    /* right of the box, still inside body: body background (light gray) */
    assert(win->render->pixels[50 * 200 + 150] == 0xFFF3F3F3);

    /* theme switch repaints with the dark background */
    assert(whaleui_app_set_theme(app, WHALEUI_THEME_DARK) == 0);
    whaleui_window_refresh_css(win);
    assert(whaleui_render_frame(win->render, win->document) == 0);
    assert(win->render->pixels[50 * 200 + 150] == 0xFF1E1E1E);
    assert(win->render->pixels[50 * 200 + 50] == 0xFFFF0000);

    whaleui_window_destroy(win);

    /* border-radius: corners outside the arc keep the body background */
    whaleui_window_t* win2 = whaleui_window_create(app, "round", 200, 150);
    assert(win2 != nullptr);
    assert(whaleui_window_load_html(win2,
        "<html><body><div id=\"r\" style=\"width:100px;height:100px;"
        "background-color:#ff0000;border-radius:10px;\"></div></body></html>") == 0);
    assert(whaleui_window_show(win2) == 0);
    assert(whaleui_render_frame(win2->render, win2->document) == 0);
    assert(win2->render->pixels[50 * 200 + 50] == 0xFFFF0000); /* center inside */
    assert(win2->render->pixels[0] == 0xFF1E1E1E);             /* corner outside arc */
    assert(win2->render->pixels[2 * 200 + 2] == 0xFF1E1E1E);   /* just outside the arc */
    assert(win2->render->pixels[5 * 200 + 5] == 0xFFFF0000);   /* just inside the arc */
    whaleui_window_destroy(win2);

    whaleui_app_destroy(app);
    return 0;
}
