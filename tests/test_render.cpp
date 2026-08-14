// test_render: paint pipeline verification on the SDL_GPU path.
// Color parsing always runs; window-level assertions run only when a GPU
// backend is available (skipped on headless/RDP without a GPU driver).
#include "whaleui.h"
#include "render/render.h"
#include "render/gpu.h"
#include "core/window.h"
#include "layout/layout.h"

#include <lexbor/dom/dom.h>
#include <SDL3/SDL.h>

#include <cassert>
#include <cstdio>
#include <cstring>

int anim_runs(void); /* defined after main (window/GPU path) */

/* read one pixel back from the GPU composited target (R8G8B8A8) */
static unsigned int gpixel(whaleui_render_t* r, int x, int y)
{
    if (!r || !r->gpu) {
        return 0;
    }
    SDL_GPUDevice* dev = r->gpu->device;
    static SDL_GPUTransferBuffer* tb = nullptr;
    if (!tb) {
        SDL_GPUTransferBufferCreateInfo tbi = {};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tbi.size = 4;
        tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    }
    if (!tb) {
        return 0;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region = {};
    region.texture = r->gpu->target2;
    region.x = static_cast<Uint32>(x < 0 ? 0 : x);
    region.y = static_cast<Uint32>(y < 0 ? 0 : y);
    region.w = 1;
    region.h = 1;
    region.d = 1;
    SDL_GPUTextureTransferInfo ti = {};
    ti.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(cp, &region, &ti);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(dev, false, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    unsigned int px = 0;
    void* data = SDL_MapGPUTransferBuffer(dev, tb, true);
    if (data) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        px = 0xFF000000 | (static_cast<unsigned int>(p[0]) << 16) |
             (static_cast<unsigned int>(p[1]) << 8) | p[2];
        SDL_UnmapGPUTransferBuffer(dev, tb);
    }
    return px;
}

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
    assert(gpixel(win->render, 0, 0) == 0xFFFF0000);             /* (0,0) */
    assert(gpixel(win->render, 50, 50) == 0xFFFF0000); /* (50,50) */
    assert(gpixel(win->render, 99, 99) == 0xFFFF0000); /* (99,99) */
    /* right of the box, still inside body: body background (light gray) */
    assert(gpixel(win->render, 150, 50) == 0xFFF3F3F3);

    /* theme switch via app_set_theme (the T-key path) repaints dark */
    assert(whaleui_app_set_theme(app, WHALEUI_THEME_DARK) == 0);
    assert(whaleui_render_frame(win->render, win->document) == 0);
    assert(gpixel(win->render, 150, 50) == 0xFF202020);
    assert(gpixel(win->render, 50, 50) == 0xFFFF0000);

    whaleui_window_destroy(win);

    /* border-radius: corners outside the arc keep the body background */
    whaleui_window_t* win2 = whaleui_window_create(app, "round", 200, 150);
    assert(win2 != nullptr);
    assert(whaleui_window_load_html(win2,
        "<html><body><div id=\"r\" style=\"width:100px;height:100px;"
        "background-color:#ff0000;border-radius:10px;\"></div></body></html>") == 0);
    assert(whaleui_window_show(win2) == 0);
    assert(whaleui_render_frame(win2->render, win2->document) == 0);
    assert(gpixel(win2->render, 50, 50) == 0xFFFF0000); /* center inside */
    assert(gpixel(win2->render, 0, 0) == 0xFF202020);             /* corner outside arc */
    assert(gpixel(win2->render, 2, 2) == 0xFF202020);   /* just outside the arc */
    assert(gpixel(win2->render, 5, 5) == 0xFFFF0000);   /* just inside the arc */
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
    assert(gpixel(win3->render, 1, 1) == 0xFF202020);
    /* top border mid (50,1): inside the 2px ring -> blue */
    assert(gpixel(win3->render, 50, 1) == 0xFF0000FF);
    /* inner area (50,20) -> red */
    assert(gpixel(win3->render, 50, 20) == 0xFFFF0000);
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
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr, 1.0f);
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
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr, 1.0f);
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
        /* clicking past the right edge of the text puts the caret at the
         * very end (the last character can be selected); keep x off the
         * page scrollbar track (right 8px of the window) */
        whaleui_render_set_pressed(w->render, inp->content.x + 100,
                                   inp->content.y + 2, 1);
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->sel_anchor == 3 && w->render->sel_focus == 3);
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
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr, 1.0f);
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
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr, 1.0f);
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
        /* clamps at the content limit (no scrolling past the bottom) */
        whaleui_render_handle_wheel(w->render, 150, 100, -1000.0f);
        assert(w->render->scrolls[w->render->tree->root->el] ==
               w->render->tree->root->scroll_max);
        /* and at the top: cannot scroll above the first content */
        whaleui_render_handle_wheel(w->render, 150, 100, 1000.0f);
        assert(w->render->scrolls[w->render->tree->root->el] == 0);
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
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr, 1.0f);
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
        assert(gpixel(w->render, 150, 150) == 0xFF0000FF);
        /* wheel-down 40px: content moves UP, blue block starts at 110 */
        whaleui_render_handle_wheel(w->render, 150, 100, -1.0f);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(gpixel(w->render, 150, 150) == 0xFF0000FF); /* still blue */
        /* y=130 is inside the shifted blue block (110..260), not red */
        assert(gpixel(w->render, 150, 130) == 0xFF0000FF);
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
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr, 1.0f);
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
        /* the tiny box has no overflow room: the wheel clamps to 0, and
         * since it consumed the event the page below does not scroll */
        whaleui_render_handle_wheel(w->render, tiny->border.x + 5,
                                    tiny->border.y + 5, -1.0f);
        assert(w->render->scrolls[tiny->el] == 0);
        assert(w->render->scrolls.count(w->render->tree->root->el) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }

    /* scrollbar drag: pressing the track and dragging moves the scroll */
    {
        whaleui_window_t* w = whaleui_window_create(app, "drag", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><div style=\"height:800px;\"></div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(w->render->tree->root->scroll_max > 0);
        int track_x = w->render->tree->root->border.x +
                      w->render->tree->root->border.w - 4;
        /* press on the track starts a drag */
        whaleui_render_set_pressed(w->render, track_x, 60, 1);
        assert(w->render->drag_scroll_el == w->render->tree->root->el);
        /* drag to the bottom of the track -> scrolled to the end */
        whaleui_render_set_hover(w->render, track_x, 198);
        whaleui_render_set_pressed(w->render, 0, 0, 0);
        assert(w->render->drag_scroll_el == nullptr);
        assert(w->render->scrolls[w->render->tree->root->el] ==
               w->render->tree->root->scroll_max);
        assert(whaleui_render_frame(w->render, w->document) == 0);
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
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr, 1.0f);
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
    anim_runs();
    return 0;
}

/* paint-only animation: the fast path keeps repainting and the element
 * fades in across frames (page-load animation actually runs) */
int anim_runs(void)
{
    whaleui_app_t* app = whaleui_app_create();
    if (!app) {
        return 0;
    }
    whaleui_window_t* w = whaleui_window_create(app, "anim-test", 200, 150);
    if (!w) {
        whaleui_app_destroy(app);
        return 0;
    }
    const char* html =
        "<html><head><style>@keyframes fade { 0% { opacity: 0; } "
        "100% { opacity: 1; } } #b { animation: fade 1500ms linear; "
        "width:100px;height:100px;background-color:#ff0000; }</style></head>"
        "<body><div id=\"b\"></div></body></html>";
    if (whaleui_window_load_html(w, html) != 0) {
        whaleui_window_destroy(w);
        whaleui_app_destroy(app);
        return 0;
    }
    if (whaleui_window_show(w) != 0) {
        whaleui_window_destroy(w);
        whaleui_app_destroy(app);
        return 0;
    }
    assert(whaleui_render_frame(w->render, w->document) == 0);
    unsigned int f0 = gpixel(w->render, 50, 50);
    /* let the animation advance ~400ms, then the box must be visibly
     * fading in (background underneath + red channel rising) */
    SDL_Delay(400);
    assert(whaleui_render_frame(w->render, w->document) == 0);
    unsigned int f1 = gpixel(w->render, 50, 50);
    unsigned int r0 = (f0 >> 16) & 0xFF, r1 = (f1 >> 16) & 0xFF;
    assert(f1 != f0);
    assert(r1 > r0); /* red channel grows as opacity climbs */
    whaleui_window_destroy(w);
    whaleui_app_destroy(app);
    return 1;
}
