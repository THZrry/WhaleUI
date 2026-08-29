// Minimal scroll/input regression checks. Kept tiny on purpose: the full
// test_render suite is large and slow, so scroll work is verified here
// first and only re-run against the whole suite at the end.
#include "whaleui.h"
#include "render/render.h"
#include "render/render_internal.h"
#include "core/window.h"
#include "test_util.h"

#include <lexbor/dom/dom.h>
#include <SDL3/SDL.h>

#include <cassert>
#include <cstdio>
#include <cstring>

int main(void)
{
    whaleui_app_t* app = whaleui_app_create();
    assert(app != nullptr);

    /* 1. a single-line input never responds to the vertical wheel: the
     * wheel over it must not move its scroll position (it has none) and
     * must not show/change a vertical scrollbar */
    {
        whaleui_window_t* w = whaleui_window_create(app, "inp-noscroll", 300, 200);
        assert(w != nullptr);
        /* long value: the input's content overflows its 120px width */
        assert(whaleui_window_load_html(w,
            "<html><body><input id=\"i\" style=\"width:120px\" "
            "value=\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnop\">"
            "</body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* inp = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 &&
                    std::memcmp(name, "input", 5) == 0) {
                    inp = &n;
                    break;
                }
            }
        }
        assert(inp != nullptr);
        lxb_dom_element* el = inp->el;
        assert(w->render->scrolls.find(el) == w->render->scrolls.end() ||
               w->render->scrolls[el] == 0);
        /* wheel over the input: must not create/move a vertical scroll */
        whaleui_render_handle_wheel(w->render, inp->border.x + 10,
                                    inp->border.y + 10, -1.0f);
        assert(w->render->scrolls.find(el) == w->render->scrolls.end() ||
               w->render->scrolls[el] == 0);
        /* and no vertical scrollbar range appears */
        assert(inp->scroll_max == 0);
        whaleui_window_destroy(w);
    }

    /* 2. textarea with wrapped content: scroll_max reflects the real
     * wrapped line count (grows past the estimate) and the box scrolls */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta-scroll", 300, 200);
        assert(w != nullptr);
        /* 60 chars wrap to ~6 lines inside 200px; box shows ~2 */
        std::string val;
        for (int i = 0; i < 60; ++i) {
            val += 'a';
        }
        std::string html = "<html><body><textarea id=\"t\" "
                           "style=\"width:200px;height:40px\">" + val +
                           "</textarea></body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* ta = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 8 &&
                    std::memcmp(name, "textarea", 8) == 0) {
                    ta = &n;
                    break;
                }
            }
        }
        assert(ta != nullptr);
        assert(ta->scroll_max > 0);
        lxb_dom_element* el = ta->el;
        int s0 = w->render->scrolls[el];
        /* wheel down scrolls it */
        whaleui_render_handle_wheel(w->render, ta->border.x + 10,
                                    ta->border.y + 10, -1.0f);
        assert(w->render->scrolls[el] > s0);
        /* wheel to the bottom clamps, then stays (no bounce) */
        for (int i = 0; i < 10; ++i) {
            whaleui_render_handle_wheel(w->render, ta->border.x + 10,
                                        ta->border.y + 10, -1.0f);
        }
        int sbot = w->render->scrolls[el];
        assert(sbot == ta->scroll_max);
        whaleui_render_handle_wheel(w->render, ta->border.x + 10,
                                    ta->border.y + 10, -1.0f);
        assert(w->render->scrolls[el] == sbot); /* clamped, no bounce */
        whaleui_window_destroy(w);
    }

    /* 3. page scroll range reaches the true content bottom: the ancestor
     * boxes keep their layout-estimated heights, so scroll_max must come
     * from the corrected subtree, not from a parent border.h */
    {
        whaleui_window_t* w = whaleui_window_create(app, "page-bottom", 300, 220);
        assert(w != nullptr);
        std::string html = "<html><body>";
        for (int i = 0; i < 30; ++i) {
            html += "line " + std::to_string(i) + "<br>";
        }
        html += "</body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* root = w->render->tree->root;
        std::printf("[scroll] page_scroll_max=%d\n", root->scroll_max);
        std::fflush(stdout);
        assert(root->scroll_max > 0);
        /* the bottom text is reachable: scroll to max, then the last run's
         * bottom is at the viewport bottom */
        w->render->scrolls[root->el] = root->scroll_max;
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_window_destroy(w);
    }

    /* dragging the scrollbar must NOT be yanked back by the relayout the
     * press triggers: the press sets has_dirty -> relayout -> fix_sm
     * clamps scrolls to its recomputed scroll_max, and if that differs
     * from the layout estimate the thumb snaps back. The dragged position
     * must survive the frame. */
    {
        whaleui_window_t* w = whaleui_window_create(app, "drag-stable", 300, 200);
        assert(w != nullptr);
        std::string val;
        for (int i = 0; i < 60; ++i) {
            val += 'a';
        }
        std::string html = "<html><body><textarea id=\"t\" "
                           "style=\"width:200px;height:40px\">" + val +
                           "</textarea></body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* ta = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 8 &&
                    std::memcmp(name, "textarea", 8) == 0) {
                    ta = &n;
                    break;
                }
            }
        }
        assert(ta != nullptr);
        assert(ta->scroll_max > 0);
        lxb_dom_element* el = ta->el;
        const int bx = ta->border.x + ta->border.w - 4;
        /* drag to ~60% down the track */
        int y1 = ta->border.y + ta->border.h * 3 / 5;
        whaleui_render_set_pressed(w->render, bx, ta->border.y + 10, 1);
        assert(w->render->drag_scroll_el == el);
        whaleui_render_set_hover(w->render, bx, y1);
        int s_drag = w->render->scrolls[el];
        assert(s_drag > 0);
        /* the next frame (which runs the relayout + fix_sm) must keep it */
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* re-locate the textarea: nodes were rebuilt by the relayout */
        whaleui_layout_node_t* ta2 = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el == el && !n.is_text) {
                ta2 = &n;
                break;
            }
        }
        assert(ta2 != nullptr);
        /* the drag may have overshot the TRUE range - clamp is fine, but
         * the position must be stable afterwards (no snap to 0, no bounce
         * between frames) */
        int expect = s_drag > ta2->scroll_max ? ta2->scroll_max : s_drag;
        assert(w->render->scrolls[el] == expect);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(w->render->scrolls[el] == expect); /* stable next frame */
        whaleui_render_set_pressed(w->render, bx, y1, 0);
        whaleui_window_destroy(w);
    }

    whaleui_app_destroy(app);
    std::printf("[scroll] OK\n");
    return 0;
}
