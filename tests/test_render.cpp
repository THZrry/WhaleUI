// test_render: paint pipeline verification on the SDL_GPU path.
// Color parsing always runs; window-level assertions run only when a GPU
// backend is available (skipped on headless/RDP without a GPU driver).
#include "whaleui.h"
#include "render/render.h"
#include "core/window.h"
#include "layout/layout.h"

#include <lexbor/dom/dom.h>

#include <cassert>
#include <cstdio>
#include <cstring>

int main(void)
{
    /* color parsing is pure logic - always verified */
    {
        unsigned int c = 0;
        assert(whaleui_render_parse_color("#ff0000", &c) == 0 && c == 0xFFFF0000);
        assert(whaleui_render_parse_color("#00ff00", &c) == 0 && c == 0xFF00FF00);
        assert(whaleui_render_parse_color("#0000ff", &c) == 0 && c == 0xFF0000FF);
        assert(whaleui_render_parse_color("#f00", &c) == 0 && c == 0xFFFF0000);
        assert(whaleui_render_parse_color("#0000ff80", &c) == 0 && c == 0x800000FF);
        assert(whaleui_render_parse_color("white", &c) == 0 && c == 0xFFFFFFFF);
        assert(whaleui_render_parse_color("transparent", &c) == 0 && c == 0x00000000);
        assert(whaleui_render_parse_color("rgb(255,0,0)", &c) == 0 && c == 0xFFFF0000);
        assert(whaleui_render_parse_color("rgba(255,0,0,128)", &c) == 0 && c == 0x80FF0000);
        assert(whaleui_render_parse_color("notacolor", &c) != 0);
    }

    whaleui_app_t* app = whaleui_app_create();
    assert(app != nullptr);

    whaleui_window_t* win = whaleui_window_create(app, "render-test", 200, 150);
    assert(win != nullptr);

    const char* html =
        "<html><body><div id=\"red\" style=\"width:100px;height:100px;"
        "background-color:#ff0000;\"></div></body></html>";
    assert(whaleui_window_load_html(win, html) == 0);
    assert(whaleui_app_set_theme(app, WHALEUI_THEME_LIGHT) == 0);

    if (whaleui_window_show(win) != 0) {
        /* no GPU backend in this environment - skip the paint assertions */
        std::fprintf(stderr, "GPU backend unavailable, skipping paint checks\n");
        whaleui_window_destroy(win);
        whaleui_app_destroy(app);
        return 0;
    }
    assert(win->render != nullptr);

    /* one frame */
    assert(whaleui_render_frame(win->render, win->document) == 0);

    /* red box covers the top-left 100x100 */
    assert(win->render->pixels[0] == 0xFFFF0000);             /* (0,0) */
    assert(win->render->pixels[50 * 200 + 50] == 0xFFFF0000); /* (50,50) */
    assert(win->render->pixels[99 * 200 + 99] == 0xFFFF0000); /* (99,99) */
    /* right of the box, still inside body: body background (light gray) */
    assert(win->render->pixels[50 * 200 + 150] == 0xFFF3F3F3);

    /* theme switch via app_set_theme (the T-key path) repaints dark */
    assert(whaleui_app_set_theme(app, WHALEUI_THEME_DARK) == 0);
    assert(whaleui_render_frame(win->render, win->document) == 0);
    assert(win->render->pixels[50 * 200 + 150] == 0xFF202020);
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
    assert(win2->render->pixels[0] == 0xFF202020);             /* corner outside arc */
    assert(win2->render->pixels[2 * 200 + 2] == 0xFF202020);   /* just outside the arc */
    assert(win2->render->pixels[5 * 200 + 5] == 0xFFFF0000);   /* just inside the arc */
    whaleui_window_destroy(win2);

    /* rounded border: the 2px border follows the corner arcs */
    whaleui_window_t* win3 = whaleui_window_create(app, "round-border", 200, 150);
    assert(win3 != nullptr);
    assert(whaleui_window_load_html(win3,
        "<html><body><div id=\"b\" style=\"width:100px;height:100px;"
        "background-color:#ff0000;border-radius:10px;"
        "border:2px solid #0000ff;\"></div></body></html>") == 0);
    assert(whaleui_window_show(win3) == 0);
    assert(whaleui_render_frame(win3->render, win3->document) == 0);
    /* corner (1,1) sits outside the arc -> body background */
    assert(win3->render->pixels[1 * 200 + 1] == 0xFF202020);
    /* top border mid (50,1): inside the 2px ring -> blue */
    assert(win3->render->pixels[1 * 200 + 50] == 0xFF0000FF);
    /* inner area (50,20) -> red */
    assert(win3->render->pixels[20 * 200 + 50] == 0xFFFF0000);
    whaleui_window_destroy(win3);

    /* <select> dropdown interaction */
    {
        whaleui_window_t* w = whaleui_window_create(app, "select", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><select id=\"t\"><option value=\"a\">Alpha</option>"
            "<option value=\"b\">Beta</option></select></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);

        /* locate the select box via a fresh layout pass */
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200);
        whaleui_layout_node_t* sel = nullptr;
        for (auto& n : t->arena) {
            if (n.visible && n.el) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 6 && std::memcmp(name, "select", 6) == 0) {
                    sel = &n;
                    break;
                }
            }
        }
        assert(sel != nullptr);
        /* click the select -> opens */
        const char* val = nullptr;
        int rc = whaleui_render_handle_click(w->render,
                                             sel->border.x + 10, sel->border.y + 10,
                                             &val);
        assert(rc == 0);
        assert(w->render->open_select != nullptr);
        assert(w->render->open_select->el == sel->el);
        /* click the SECOND option (item height 26) -> chosen, value "b" */
        int item_center = sel->border.y + sel->border.h + 26 + 13;
        rc = whaleui_render_handle_click(w->render, sel->border.x + 10,
                                         item_center, &val);
        assert(rc == 1);
        assert(val != nullptr && std::strcmp(val, "b") == 0);
        assert(w->render->open_select == nullptr);
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }

    whaleui_app_destroy(app);
    return 0;
}
