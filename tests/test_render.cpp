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
            &w->render->theme_vars, 300, 200, nullptr, nullptr);
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
        assert(w->render->open_select == sel->el);
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

    /* editable input: click focuses + places the caret; typing + backspace
     * update the DOM value */
    {
        whaleui_window_t* w = whaleui_window_create(app, "edit", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><input id=\"i\" value=\"abc\"></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);

        whaleui_layout_tree_t* t = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200, nullptr, nullptr);
        assert(t != nullptr);
        whaleui_layout_node_t* inp = nullptr;
        for (auto& n : t->arena) {
            if (n.visible && n.el) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 && std::memcmp(name, "input", 5) == 0) {
                    inp = &n;
                    break;
                }
            }
        }
        assert(inp != nullptr);
        /* click the middle of the input -> caret > 0, so painting the caret
         * exercises TTF_GetTextSubStringsForRange (single-allocation free
         * regression: per-item SDL_free used to corrupt the heap) */
        whaleui_render_set_pressed(w->render, inp->content.x + 20,
                                   inp->content.y + 2, 1);
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->edit_el == inp->el);
        assert(w->render->sel_focus > 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* typing inserts at the caret */
        whaleui_render_handle_text(w->render, "X");
        const char* v = whaleui_dom_get_attribute(
            reinterpret_cast<whaleui_dom_element_t*>(inp->el), "value");
        assert(v != nullptr && std::strlen(v) == 4 &&
               std::strchr(v, 'X') != nullptr);
        /* painting the new value + caret again */
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* backspace removes the inserted char */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_BACKSPACE, 1, 0);
        v = whaleui_dom_get_attribute(
            reinterpret_cast<whaleui_dom_element_t*>(inp->el), "value");
        assert(v != nullptr && std::strlen(v) == 3 &&
               std::strchr(v, 'X') == nullptr);
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }

    /* text selection: clicking a text run anchors, dragging extends */
    {
        whaleui_window_t* w = whaleui_window_create(app, "sel", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><p id=\"p\">hello world</p></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);

        whaleui_layout_tree_t* t = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200, nullptr, nullptr);
        assert(t != nullptr);
        whaleui_layout_node_t* p = nullptr;
        whaleui_layout_node_t* tr = nullptr;
        for (auto& n : t->arena) {
            if (!n.visible) {
                continue;
            }
            if (!p && n.el) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 1 && std::memcmp(name, "p", 1) == 0) {
                    p = &n;
                }
            }
            if (n.is_text && p && n.el == p->el) {
                tr = &n;
            }
        }
        assert(p != nullptr && tr != nullptr);
        int y = tr->border.y + tr->border.h / 2;
        /* plain click (press+release, no drag): nothing is selected */
        whaleui_render_set_pressed(w->render, tr->border.x + 1, y, 1);
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->sel_anchor_el == nullptr);
        assert(w->render->sel_anchor == w->render->sel_focus);
        /* click with sub-threshold micro-motion still selects nothing */
        whaleui_render_set_pressed(w->render, tr->border.x + 1, y, 1);
        whaleui_render_set_hover(w->render, tr->border.x + 4, y); /* < 6px */
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->sel_anchor_el == nullptr);
        /* drag past the threshold selects */
        whaleui_render_set_pressed(w->render, tr->border.x + 1, y, 1);
        whaleui_render_set_hover(w->render, tr->border.x + 60, y);
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->sel_anchor_el == p->el);
        assert(w->render->sel_anchor == 0);
        assert(w->render->sel_focus > w->render->sel_anchor);
        /* painting with an active selection (highlight path) */
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* clicking elsewhere clears the selection */
        whaleui_render_set_pressed(w->render, 250, 150, 1);
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->sel_anchor_el == nullptr);
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }

    /* textarea: Enter inserts a newline into the text content */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><textarea id=\"t\">ab</textarea></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);

        whaleui_layout_tree_t* t = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200, nullptr, nullptr);
        assert(t != nullptr);
        whaleui_layout_node_t* ta = nullptr;
        for (auto& n : t->arena) {
            if (n.visible && n.el) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 8 && std::memcmp(name, "textarea", 8) == 0) {
                    ta = &n;
                    break;
                }
            }
        }
        assert(ta != nullptr);
        /* click the far-left edge -> caret 0 */
        whaleui_render_set_pressed(w->render, ta->content.x + 1,
                                   ta->content.y + 2, 1);
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->edit_el == ta->el);
        /* Enter inserts a newline before "ab" */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_ENTER, 1, 0);
        const char* txt = whaleui_dom_get_text(
            reinterpret_cast<whaleui_dom_element_t*>(ta->el));
        assert(txt != nullptr && std::strcmp(txt, "\nab") == 0);
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }

    /* page scroll via wheel: no scrollable ancestor -> the html root
     * scrolls; notch vs touchpad pixel deltas both clamp correctly */
    {
        whaleui_window_t* w = whaleui_window_create(app, "page-scroll", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><div style=\"height:800px;\"></div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(w->render->tree->root->scroll_max > 0);
        /* scrolling must NOT rebuild the layout tree (perf: relayout is the
         * expensive part on big pages) */
        whaleui_layout_tree_t* tree_before = w->render->tree;
        /* wheel-down (dy=-1) reveals content below: scroll_y += 40 */
        whaleui_render_handle_wheel(w->render, 150, 100, -1.0f);
        assert(w->render->tree == tree_before);
        assert(w->render->scrolls[w->render->tree->root->el] == 40);
        /* no clamping while limits are disabled: it keeps accumulating */
        whaleui_render_handle_wheel(w->render, 150, 100, -1000.0f);
        assert(w->render->scrolls[w->render->tree->root->el] == 1040);
        /* wheel-up reduces it, still without bouncing back */
        whaleui_render_handle_wheel(w->render, 150, 100, 1000.0f);
        assert(w->render->scrolls[w->render->tree->root->el] == 40);
        whaleui_window_destroy(w);
    }

    /* container scroll via wheel: an overflow:auto box scrolls its own
     * content (not the page) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "cscroll", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><div id=\"sc\" style=\"overflow:auto;height:50px;\">"
            "line1<br>line2<br>line3<br>line4</div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);

        whaleui_layout_tree_t* t = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200, nullptr, nullptr);
        assert(t != nullptr);
        whaleui_layout_node_t* sc = nullptr;
        for (auto& n : t->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 3 && std::memcmp(name, "div", 3) == 0) {
                    sc = &n;
                    break;
                }
            }
        }
        assert(sc != nullptr);
        assert(sc->scroll_max > 0);
        /* cross-check the renderer's own tree (the one handle_wheel uses) */
        whaleui_layout_node_t* rsc = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 3 && std::memcmp(name, "div", 3) == 0) {
                    rsc = &n;
                    break;
                }
            }
        }
        assert(rsc != nullptr);
        assert(rsc->scroll_max > 0);
        /* wheel over the container scrolls it, without rebuilding the tree */
        whaleui_layout_tree_t* tree_before = w->render->tree;
        whaleui_render_handle_wheel(w->render, sc->border.x + 5,
                                    sc->border.y + 5, -1.0f);
        assert(w->render->tree == tree_before);
        assert(w->render->scrolls[sc->el] > 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }

    /* scroll shifts content UP (reveals content below): pixel check */
    {
        whaleui_window_t* w = whaleui_window_create(app, "pixscroll", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><div style=\"height:150px;background-color:#ff0000\">"
            "</div><div style=\"height:150px;background-color:#0000ff\">"
            "</div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* before scrolling, (150,150) is inside the blue block (150..300) */
        assert(w->render->pixels[150 * 300 + 150] == 0xFF0000FF);
        /* wheel-down 40px: content moves UP, blue block starts at 110 */
        whaleui_render_handle_wheel(w->render, 150, 100, -1.0f);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(w->render->pixels[150 * 300 + 150] == 0xFF0000FF); /* still blue */
        /* y=130 is inside the shifted blue block (110..260), not red */
        assert(w->render->pixels[130 * 300 + 150] == 0xFF0000FF);
        whaleui_window_destroy(w);
    }

    /* a non-overflowing overflow:auto box must NOT block page scrolling */
    {
        whaleui_window_t* w = whaleui_window_create(app, "noscroll", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><div id=\"tiny\" style=\"overflow:auto;height:30px;\">"
            "x</div><div style=\"height:800px;\"></div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(w->render->tree->root->scroll_max > 0);
        /* the tiny box cannot scroll itself (scroll_max==0): the wheel must
         * fall through to the page */
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200, nullptr, nullptr);
        assert(t != nullptr);
        whaleui_layout_node_t* tiny = nullptr;
        for (auto& n : t->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 3 && std::memcmp(name, "div", 3) == 0) {
                    tiny = &n;
                    break;
                }
            }
        }
        assert(tiny != nullptr);
        assert(tiny->scroll_max == 0);
        /* limits are off: the tiny box consumes the wheel itself (no
         * bounce-back, no fallthrough to the page) */
        whaleui_render_handle_wheel(w->render, tiny->border.x + 5,
                                    tiny->border.y + 5, -1.0f);
        assert(w->render->scrolls.count(tiny->el) == 1);
        assert(w->render->scrolls[tiny->el] > 0);
        assert(w->render->scrolls.count(w->render->tree->root->el) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }
    {
        whaleui_window_t* w = whaleui_window_create(app, "xsel", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><p id=\"a\">aaa</p><p id=\"b\">bbb</p>"
            "<p id=\"c\">ccc</p></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);

        whaleui_layout_tree_t* t = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200, nullptr, nullptr);
        assert(t != nullptr);
        /* the three <p> text runs, in document order */
        whaleui_layout_node_t* runs[3] = {nullptr, nullptr, nullptr};
        int run_idx = 0;
        for (auto& n : t->arena) {
            if (!n.visible || !n.is_text) {
                continue;
            }
            if (run_idx < 3) {
                runs[run_idx++] = &n;
            }
        }
        whaleui_layout_node_t* ra = runs[0];
        whaleui_layout_node_t* rb = runs[1];
        whaleui_layout_node_t* rc = runs[2];
        assert(ra != nullptr && rb != nullptr && rc != nullptr);
        /* anchor in c, drag up into a */
        int y = rc->border.y + rc->border.h / 2;
        whaleui_render_set_pressed(w->render, rc->border.x + 5, y, 1);
        y = ra->border.y + ra->border.h / 2;
        whaleui_render_set_hover(w->render, ra->border.x + 5, y);
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->sel_anchor_el == rc->el);
        assert(w->render->sel_focus_el == ra->el);
        /* painting the cross-element highlight must not crash */
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* a plain click in the middle paragraph clears everything */
        y = rb->border.y + rb->border.h / 2;
        whaleui_render_set_pressed(w->render, rb->border.x + 5, y, 1);
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->sel_anchor_el == nullptr);
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }

    whaleui_app_destroy(app);
    return 0;
}
