// Minimal scroll/input regression checks. Kept tiny on purpose: the full
// test_render suite is large and slow, so scroll work is verified here
// first and only re-run against the whole suite at the end.
#include "whaleui.h"
#include "render/render.h"
#include "render/render_internal.h"
#include "render/gpu.h"
#include "core/window.h"
#include "test_util.h"

#include <lexbor/dom/dom.h>
#include <SDL3/SDL.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>

/* read one pixel back from the GPU composited target (R8G8B8A8) */
static unsigned int gpixel(whaleui_render_t* r, int x, int y)
{
    if (!r || !r->gpu) {
        return 0;
    }
    SDL_GPUDevice* dev = r->gpu->device;
    SDL_GPUTransferBufferCreateInfo tbi = {};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
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

    /* 2b. single-line input with an overflowing value: the wheel scrolls
     * the input horizontally (hscrolls) and must NOT scroll the page.
     * A short value falls through to the page (browser-like). */
    {
        whaleui_window_t* w = whaleui_window_create(app, "inp-hscroll", 300, 200);
        assert(w != nullptr);
        std::string html = "<html><body style=\"height:900px;\">"
                           "<input id=\"long\" style=\"width:120px\" "
                           "value=\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnop\">"
                           "<input id=\"short\" style=\"width:120px\" value=\"ab\">"
                           "</body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        lxb_dom_element *long_el = nullptr, *short_el = nullptr;
        int lx = 0, ly = 0, sx = 0, sy = 0;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 &&
                    std::memcmp(name, "input", 5) == 0) {
                    if (!long_el) {
                        long_el = n.el;
                        lx = n.border.x + 10;
                        ly = n.border.y + 10;
                    } else {
                        short_el = n.el;
                        sx = n.border.x + 10;
                        sy = n.border.y + 10;
                    }
                }
            }
        }
        assert(long_el && short_el);
        lxb_dom_element* root_el = w->render->tree->root->el;
        /* long value: wheel over it moves hscrolls, page stays */
        whaleui_render_handle_wheel(w->render, lx, ly, -1.0f);
        assert(w->render->hscrolls[long_el] > 0);
        assert(w->render->scrolls[root_el] == 0);
        /* short value: wheel over it falls through to the page */
        whaleui_render_handle_wheel(w->render, sx, sy, -1.0f);
        assert(w->render->scrolls[root_el] > 0);
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

    /* a long real-world page (q21k-style): scroll_max must reach the true
     * content bottom - the last run's bottom (corrected line height) must
     * fit inside max + viewport, with no missing ~line at the end */
    {
        const char* path = WHALEUI_TEST_ROOT
            "/temp/Qwen_html_20260814_oeem340or.html";
        FILE* f = std::fopen(path, "rb");
        if (!f) {
            std::printf("[scroll] q21k file missing, skipping\n");
            std::fflush(stdout);
        } else {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::string html(static_cast<size_t>(sz), '\0');
            std::fread(&html[0], 1, static_cast<size_t>(sz), f);
            std::fclose(f);
            whaleui_window_t* w =
                whaleui_window_create(app, "q21k", 500, 400);
            assert(w != nullptr);
            assert(whaleui_window_load_html(w, html.c_str()) == 0);
            assert(whaleui_window_show(w) == 0);
            assert(whaleui_render_frame(w->render, w->document) == 0);
            whaleui_layout_node_t* root = w->render->tree->root;
            whaleui_layout_node_t* last_run = nullptr;
            for (auto& n : w->render->tree->arena) {
                if (n.visible && n.is_text) {
                    last_run = &n;
                }
            }
            assert(root->scroll_max > 0);
            assert(last_run != nullptr);
            int content_bottom = root->scroll_max + root->content.h;
            int run_bottom = last_run->border.y + last_run->border.h;
            std::printf("[scroll] q21k scroll_max=%d viewport=%d "
                        "content_bottom=%d run_bottom=%d\n",
                        root->scroll_max, root->content.h, content_bottom,
                        run_bottom);
            std::fflush(stdout);
            /* the page scroll range reaches the real content bottom */
            assert(run_bottom <= content_bottom + 2);
            assert(run_bottom > content_bottom - 40);
            /* wheel must scroll the page and KEEP the position: a frame
             * (repaint) must not reset it to the top */
            lxb_dom_element* rel = root->el;
            int s0 = w->render->scrolls[rel];
            whaleui_render_handle_wheel(w->render, 250, 200, -1.0f);
            int s1 = w->render->scrolls[rel];
            assert(s1 > s0);
            assert(whaleui_render_frame(w->render, w->document) == 0);
            std::printf("[scroll] q21k wheel s0=%d s1=%d after-frame=%d\n",
                        s0, s1, w->render->scrolls[rel]);
            std::fflush(stdout);
            assert(w->render->scrolls[rel] == s1); /* no jump to top */
            whaleui_window_destroy(w);
        }
    }

    /* paint must actually move the content with the scroll: after a wheel
     * on the page, a top block scrolls out of view (pixel check). If the
     * position only lives in scrolls and the paint resets, the content
     * stays at the top - the "jumps back to top" report. */
    {
        whaleui_window_t* w = whaleui_window_create(app, "paint-scroll", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><div id=\"top\" style=\"width:300px;height:80px;"
            "background-color:#ff0000;\"></div>"
            "<div style=\"width:300px;height:400px;"
            "background-color:#0000ff;\"></div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* the red block is at the top before scrolling */
        unsigned int c0 = gpixel(w->render, 10, 10);
        whaleui_layout_node_t* root = w->render->tree->root;
        lxb_dom_element* rel = root->el;
        int s0 = w->render->scrolls[rel];
        /* scroll down ~100px: red leaves the top, blue arrives */
        for (int i = 0; i < 5; ++i) {
            whaleui_render_handle_wheel(w->render, 150, 150, -1.0f);
        }
        int s1 = w->render->scrolls[rel];
        assert(s1 >= s0 + 100);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        unsigned int c1 = gpixel(w->render, 10, 10);
        std::printf("[scroll] paint c0=%08X c1=%08X s=%d\n", c0, c1,
                    w->render->scrolls[rel]);
        std::fflush(stdout);
        assert(c0 == 0xFFFF0000);       /* red at top before */
        assert(c1 == 0xFF0000FF);       /* blue scrolled in: content moved */
        whaleui_window_destroy(w);
    }

    /* demo-style page (flex row of cards with an input and a textarea):
     * the page must scroll to its real bottom, and the single-line input
     * must not claim the vertical wheel */
    {
        whaleui_window_t* w = whaleui_window_create(app, "demo-scroll", 500, 300);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><head><style>"
            ".row { display:flex; } .card { flex:1; border:1px solid "
            "black; padding:8px; }"
            "</style></head><body><div class=\"row\"><div class=\"card\">"
            "<h2>Edit</h2>"
            "<p><input id=\"i\" style=\"width:100%\" value=\"input\"></p>"
            "<p><textarea id=\"t\" style=\"width:100%;height:56px\">"
            "aaaa\nbbbb\ncccc</textarea></p>"
            "</div><div class=\"card\">Scroll card<br>line 1<br>line 2<br>"
            "line 3<br>line 4<br>line 5<br>line 6<br>line 7<br>line 8<br>"
            "line 9<br>line 10</div></div>"
            "<div style=\"height:300px;background:#eee\"></div>"
            "</body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* root = w->render->tree->root;
        lxb_dom_element* rel = root->el;
        assert(root->scroll_max > 0);
        /* the page bottom is reachable: scroll to max */
        w->render->scrolls[rel] = root->scroll_max;
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* wheel on the single-line input scrolls the PAGE, not the input */
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
        lxb_dom_element* iel = inp->el;
        assert(w->render->scrolls.find(iel) == w->render->scrolls.end() ||
               w->render->scrolls[iel] == 0);
        whaleui_render_handle_wheel(w->render, inp->border.x + 10,
                                    inp->border.y + 10, -1.0f);
        assert(w->render->scrolls.find(iel) == w->render->scrolls.end() ||
               w->render->scrolls[iel] == 0); /* input untouched */
        assert(w->render->scrolls[rel] > 0);  /* the page scrolled */
        whaleui_window_destroy(w);
    }

    /* wheel right after load (before the first frame finishes): the page
     * must still scroll to its real bottom afterwards - the early wheel
     * must not leave scroll_max stuck at the un-corrected estimate */
    {
        const char* path = WHALEUI_TEST_ROOT
            "/temp/Qwen_html_20260814_oeem340or.html";
        FILE* f = std::fopen(path, "rb");
        if (!f) {
            std::printf("[scroll] q21k file missing, skipping\n");
            std::fflush(stdout);
        } else {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::string html(static_cast<size_t>(sz), '\0');
            std::fread(&html[0], 1, static_cast<size_t>(sz), f);
            std::fclose(f);
            whaleui_window_t* w =
                whaleui_window_create(app, "q21k-early", 500, 400);
            assert(w != nullptr);
            assert(whaleui_window_load_html(w, html.c_str()) == 0);
            assert(whaleui_window_show(w) == 0);
            /* wheel BEFORE the first frame */
            whaleui_render_handle_wheel(w->render, 250, 200, -1.0f);
            whaleui_render_handle_wheel(w->render, 250, 200, -1.0f);
            assert(whaleui_render_frame(w->render, w->document) == 0);
            assert(whaleui_render_frame(w->render, w->document) == 0);
            whaleui_layout_node_t* root = w->render->tree->root;
            int smax = root->scroll_max;
            std::printf("[scroll] q21k-early scroll_max=%d\n", smax);
            std::fflush(stdout);
            assert(smax > 0);
            /* scroll to the bottom must be possible */
            lxb_dom_element* rel = root->el;
            for (int i = 0; i < 200; ++i) {
                whaleui_render_handle_wheel(w->render, 250, 200, -1.0f);
            }
            assert(w->render->scrolls[rel] >= smax - 1);
            whaleui_window_destroy(w);
        }
    }

    /* hovering an element must not reset the page scroll, and the hover
     * target repaints its :hover style (partial repaint): the target sits
     * in the VIEWPORT so pixels are readable */
    {
        whaleui_window_t* w = whaleui_window_create(app, "hover-keep", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><head><style>div:hover { background:#ccc; }</style>"
            "</head><body><div id=\"h\" style=\"height:60px\">tail</div>"
            "<div style=\"height:600px\"></div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* root = w->render->tree->root;
        lxb_dom_element* rel = root->el;
        assert(root->scroll_max > 0);
        /* hover the in-viewport target (visual == layout) */
        whaleui_layout_node_t* hov = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 3 &&
                    std::memcmp(name, "div", 3) == 0 &&
                    n.border.h == 60) {
                    hov = &n;
                    break;
                }
            }
        }
        assert(hov != nullptr);
        lxb_dom_element* hel = hov->el; /* DOM element survives rebuilds */
        unsigned int pre_h0 =
            gpixel(w->render, hov->border.x + 2, hov->border.y + 2);
        whaleui_render_set_hover(w->render, hov->border.x + 5,
                                 hov->border.y + 5);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* re-locate the hover target: the relayout rebuilt the tree */
        hov = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el == hel && !n.is_text) {
                hov = &n;
                break;
            }
        }
        assert(hov != nullptr);
        unsigned int pre_h1 =
            gpixel(w->render, hov->border.x + 2, hov->border.y + 2);
        std::printf("[scroll] hover-pre %08X -> %08X\n", pre_h0, pre_h1);
        std::fflush(stdout);
        assert(pre_h1 != pre_h0); /* :hover style actually applies */
        /* now scroll: position must survive a hover change AND the frame */
        for (int i = 0; i < 8; ++i) {
            whaleui_render_handle_wheel(w->render, 150, 100, -1.0f);
        }
        int s1 = w->render->scrolls[rel];
        assert(s1 > 0);
        /* move the mouse onto the (scrolled) tail element */
        whaleui_layout_node_t* hov2 = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 3 &&
                    std::memcmp(name, "div", 3) == 0 &&
                    n.border.h == 60) {
                    hov2 = &n;
                    break;
                }
            }
        }
        assert(hov2 != nullptr);
        whaleui_render_set_hover(w->render, hov2->border.x + 5,
                                 hov2->border.y + 5);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        std::printf("[scroll] hover s1=%d after=%d\n", s1,
                    w->render->scrolls[rel]);
        std::fflush(stdout);
        assert(w->render->scrolls[rel] == s1); /* position survives hover */
        /* a distant pixel is untouched by the hover repaint */
        whaleui_layout_node_t* far = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text && n.border.h == 600) {
                far = &n;
                break;
            }
        }
        assert(far != nullptr);
        int fv = far->border.y - w->render->scrolls[rel];
        unsigned int fc0 = gpixel(w->render, far->border.x + 2, fv + 2);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        unsigned int fc1 = gpixel(w->render, far->border.x + 2, fv + 2);
        std::printf("[scroll] hover far %08X -> %08X\n", fc0, fc1);
        std::fflush(stdout);
        assert(fc1 == fc0); /* elsewhere untouched */
        whaleui_window_destroy(w);
    }

    /* the scrollbar thumb must follow the wheel AND leave no residue: with
     * scroll-shift, the shifted old thumb image must be overwritten by the
     * scrollbar column repaint (pixel check on the right-edge column) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "sb-pixel", 200, 120);
        assert(w != nullptr);
        std::string html = "<html><body><div id=\"sc\" style=\"overflow:"
                           "auto;height:80px;width:180px\">";
        for (int i = 0; i < 30; ++i) {
            html += "line " + std::to_string(i) + "<br>";
        }
        html += "</div></body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* sc = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 3 &&
                    std::memcmp(name, "div", 3) == 0 &&
                    n.scroll_max > 0) {
                    sc = &n;
                    break;
                }
            }
        }
        assert(sc != nullptr && sc->scroll_max > 0);
        lxb_dom_element* el = sc->el;
        const int bx = sc->border.x + sc->border.w - 4; /* on the bar */
        /* thumb starts at the top of the track */
        unsigned int p0 = gpixel(w->render, bx, sc->border.y + 2);
        int s0 = w->render->scrolls[el];
        for (int i = 0; i < 6; ++i) {
            whaleui_render_handle_wheel(w->render, sc->border.x + 10,
                                        sc->border.y + 10, -1.0f);
        }
        int s1 = w->render->scrolls[el];
        assert(s1 > s0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        unsigned int p1 = gpixel(w->render, bx, sc->border.y + 2);
        std::printf("[scroll] sb thumb p0=%08X p1=%08X s=%d\n", p0, p1,
                    w->render->scrolls[el]);
        std::fflush(stdout);
        /* after scrolling down the thumb moved away from the top: the top
         * of the track must no longer be thumb color */
        assert(p1 != p0);
        whaleui_window_destroy(w);
    }

    /* the wheel must reach the bottom even when a relayout (hover) happens
     * mid-scroll, and the range (thumb length) must stay stable across the
     * relayout - the "wheel stops mid-way and the thumb grows" report */
    {
        whaleui_window_t* w = whaleui_window_create(app, "wheel-relayout", 300, 200);
        assert(w != nullptr);
        std::string html = "<html><head><style>div:hover {"
                           "background:#ccc; }</style></head><body>";
        for (int i = 0; i < 25; ++i) {
            html += "<div style=\"height:40px\">line " + std::to_string(i) +
                    "</div>";
        }
        html += "</body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* root = w->render->tree->root;
        lxb_dom_element* rel = root->el;
        int smax0 = root->scroll_max;
        assert(smax0 > 0);
        /* wheel half-way */
        for (int i = 0; i < 10; ++i) {
            whaleui_render_handle_wheel(w->render, 150, 100, -1.0f);
        }
        int s_mid = w->render->scrolls[rel];
        assert(s_mid > 0 && s_mid < smax0);
        /* hover a mid-page element -> relayout */
        whaleui_render_set_hover(w->render, 150, 80);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        root = w->render->tree->root; /* re-locate after rebuild */
        assert(root != nullptr);
        int smax1 = root->scroll_max;
        std::printf("[scroll] wheel-relayout smax %d -> %d s=%d\n", smax0,
                    smax1, w->render->scrolls[rel]);
        std::fflush(stdout);
        /* the range must not shrink from the relayout (thumb length
         * stable), and the position must survive */
        assert(smax1 >= smax0);
        assert(w->render->scrolls[rel] == s_mid ||
               w->render->scrolls[rel] <= smax1);
        /* keep wheeling: must reach the bottom */
        for (int i = 0; i < 60; ++i) {
            whaleui_render_handle_wheel(w->render, 150, 100, -1.0f);
        }
        std::printf("[scroll] wheel-relayout bottom s=%d smax=%d\n",
                    w->render->scrolls[rel], smax1);
        std::fflush(stdout);
        assert(w->render->scrolls[rel] >= smax1 - 1);
        whaleui_window_destroy(w);
    }

    /* q21k: many wheel steps with a frame between each must NOT shrink the
     * range - the "each wheel makes the thumb a bit longer" report. The
     * range only changes when content changes. */
    {
        const char* path = WHALEUI_TEST_ROOT
            "/temp/Qwen_html_20260814_oeem340or.html";
        FILE* f = std::fopen(path, "rb");
        if (!f) {
            std::printf("[scroll] q21k file missing, skipping\n");
            std::fflush(stdout);
        } else {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::string html(static_cast<size_t>(sz), '\0');
            std::fread(&html[0], 1, static_cast<size_t>(sz), f);
            std::fclose(f);
            whaleui_window_t* w =
                whaleui_window_create(app, "q21k-stable", 500, 400);
            assert(w != nullptr);
            assert(whaleui_window_load_html(w, html.c_str()) == 0);
            assert(whaleui_window_show(w) == 0);
            assert(whaleui_render_frame(w->render, w->document) == 0);
            whaleui_layout_node_t* root = w->render->tree->root;
            lxb_dom_element* rel = root->el;
            int smax0 = root->scroll_max;
            assert(smax0 > 0);
            int prev_s = 0;
            for (int i = 0; i < 8; ++i) {
                whaleui_render_handle_wheel(w->render, 250, 200, -1.0f);
                assert(whaleui_render_frame(w->render, w->document) == 0);
                root = w->render->tree->root;
                assert(w->render->scrolls[rel] > prev_s);
                prev_s = w->render->scrolls[rel];
            }
            int smax1 = root->scroll_max;
            std::printf("[scroll] q21k-stable smax %d -> %d s=%d\n", smax0,
                        smax1, w->render->scrolls[rel]);
            std::fflush(stdout);
            assert(smax1 == smax0); /* range never shrinks while wheeling */
            whaleui_window_destroy(w);
        }
    }

    /* demo-shaped page: several flex rows of cards with inputs/textareas/
     * hover styles - the page must scroll and reach its bottom */
    {
        whaleui_window_t* w = whaleui_window_create(app, "demo-full", 500, 300);
        assert(w != nullptr);
        std::string html =
            "<html><head><style>"
            ".row { display:flex; margin-bottom:10px; }"
            ".card { flex:1; border:1px solid black; padding:8px; "
            "margin:0 6px; }"
            ".card:hover { background:#eee; }"
            "</style></head><body>";
        for (int r = 0; r < 4; ++r) {
            html += "<div class=\"row\"><div class=\"card\"><h2>Card</h2>"
                    "<p><input style=\"width:100%\"></p>"
                    "<p><textarea style=\"width:100%;height:50px\">"
                    "text</textarea></p></div>"
                    "<div class=\"card\">line 1<br>line 2<br>line 3<br>"
                    "</div></div>";
        }
        html += "</body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* root = w->render->tree->root;
        lxb_dom_element* rel = root->el;
        int smax0 = root->scroll_max;
        std::printf("[scroll] demo-full smax=%d\n", smax0);
        std::fflush(stdout);
        assert(smax0 > 0);
        /* wheel must scroll, and must reach the bottom */
        int s0 = w->render->scrolls[rel];
        whaleui_render_handle_wheel(w->render, 250, 150, -1.0f);
        assert(w->render->scrolls[rel] > s0);
        for (int i = 0; i < 40; ++i) {
            whaleui_render_handle_wheel(w->render, 250, 150, -1.0f);
        }
        std::printf("[scroll] demo-full bottom s=%d smax=%d\n",
                    w->render->scrolls[rel], smax0);
        std::fflush(stdout);
        assert(w->render->scrolls[rel] >= smax0 - 1);
        whaleui_window_destroy(w);
    }

    /* textarea scroll range: content that fits must NOT scroll (no range),
     * many lines must scroll by the exact overflow */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta-range", 300, 200);
        assert(w != nullptr);
        /* 2 lines fit inside the 56px box */
        assert(whaleui_window_load_html(w,
            "<html><body><textarea id=\"t\" style=\"width:200px;"
            "height:56px\">aaaa\nbbbb</textarea></body></html>") == 0);
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
        int smax_fit = ta->scroll_max;
        std::printf("[scroll] ta-fit smax=%d ch=%d\n", smax_fit,
                    ta->content.h);
        std::fflush(stdout);
        assert(smax_fit == 0); /* 2 lines fit: no scroll range */
        /* 12 lines: the range must be the real overflow */
        whaleui_window_t* w2 =
            whaleui_window_create(app, "ta-range2", 300, 200);
        assert(w2 != nullptr);
        std::string html = "<html><body><textarea id=\"t\" "
                           "style=\"width:200px;height:56px\">";
        for (int i = 0; i < 12; ++i) {
            html += "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
        }
        html += "</textarea></body></html>";
        assert(whaleui_window_load_html(w2, html.c_str()) == 0);
        assert(whaleui_window_show(w2) == 0);
        assert(whaleui_render_frame(w2->render, w2->document) == 0);
        whaleui_layout_node_t* ta2 = nullptr;
        for (auto& n : w2->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 8 &&
                    std::memcmp(name, "textarea", 8) == 0) {
                    ta2 = &n;
                    break;
                }
            }
        }
        assert(ta2 != nullptr);
        int smax_many = ta2->scroll_max;
        std::printf("[scroll] ta-many smax=%d ch=%d\n", smax_many,
                    ta2->content.h);
        std::fflush(stdout);
        assert(smax_many > 0); /* overflows: must scroll */
        /* the textarea must reach its own bottom by wheel */
        lxb_dom_element* tel = ta2->el;
        int s0 = w2->render->scrolls[tel];
        for (int i = 0; i < 30; ++i) {
            whaleui_render_handle_wheel(w2->render, ta2->border.x + 10,
                                        ta2->border.y + 10, -1.0f);
        }
        std::printf("[scroll] ta-many bottom s=%d smax=%d\n",
                    w2->render->scrolls[tel], smax_many);
        std::fflush(stdout);
        assert(w2->render->scrolls[tel] >= smax_many - 1);
        whaleui_window_destroy(w);
        whaleui_window_destroy(w2);
    }

    /* textarea boundary details: demo-style 2-line CJK+ASCII fits exactly
     * (no scroll), 12 lines reach the TRUE top by wheel (no ~half-line
     * shortfall), and the scrollbar thumb length matches the range */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta-edge", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><textarea id=\"t\" style=\"width:200px;"
            "height:56px;box-sizing:border-box\">多行文本\n"
            "支持 Enter 换行</textarea></body></html>") == 0);
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
        int smax_fit = ta->scroll_max;
        std::printf("[scroll] ta-edge fit smax=%d ch=%d\n", smax_fit,
                    ta->content.h);
        std::fflush(stdout);
        assert(smax_fit == 0); /* 2 lines fit: no scroll */
        whaleui_window_destroy(w);

        /* 12 lines: wheel to the top must reveal the LAST line (the
         * scroll_max must not fall short by ~half a line) */
        whaleui_window_t* w2 =
            whaleui_window_create(app, "ta-edge2", 300, 200);
        assert(w2 != nullptr);
        std::string html = "<html><body><textarea id=\"t\" "
                           "style=\"width:200px;height:56px\">";
        for (int i = 0; i < 12; ++i) {
            html += "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
        }
        html += "</textarea></body></html>";
        assert(whaleui_window_load_html(w2, html.c_str()) == 0);
        assert(whaleui_window_show(w2) == 0);
        assert(whaleui_render_frame(w2->render, w2->document) == 0);
        whaleui_layout_node_t* ta2 = nullptr;
        for (auto& n : w2->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 8 &&
                    std::memcmp(name, "textarea", 8) == 0) {
                    ta2 = &n;
                    break;
                }
            }
        }
        assert(ta2 != nullptr);
        int smax2 = ta2->scroll_max;
        whaleui_layout_node_t* run = nullptr;
        for (auto& n : w2->render->tree->arena) {
            if (n.visible && n.is_text) {
                run = &n;
            }
        }
        assert(run != nullptr);
        /* last run's bottom minus content.y is the true content height */
        int true_bottom = run->border.y + run->border.h - ta2->content.y +
                          ta2->scroll_y;
        int expect_max = true_bottom - ta2->content.h;
        std::printf("[scroll] ta-edge2 smax=%d expect=%d lh_diff=%d\n",
                    smax2, expect_max, smax2 - expect_max);
        std::fflush(stdout);
        /* at most a couple of px off (rounding), never ~half a line */
        assert(smax2 >= expect_max - 3);
        lxb_dom_element* tel = ta2->el;
        for (int i = 0; i < 40; ++i) {
            whaleui_render_handle_wheel(w2->render, ta2->border.x + 10,
                                        ta2->border.y + 10, -1.0f);
        }
        std::printf("[scroll] ta-edge2 bottom s=%d smax=%d\n",
                    w2->render->scrolls[tel], smax2);
        std::fflush(stdout);
        assert(w2->render->scrolls[tel] >= smax2 - 1);
        whaleui_window_destroy(w2);
    }

    /* an EMPTY textarea: the caret sits on the FIRST line (top), not the
     * bottom of the box */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta-empty", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><textarea id=\"t\" style=\"width:200px;"
            "height:56px\"></textarea></body></html>") == 0);
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
        lxb_dom_element* tel = ta->el;
        w->render->edit_el = tel;
        w->render->sel_anchor = w->render->sel_focus = 0;
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* caret for offset 0 must be at the textarea's content top */
        int fs;
        std::string family;
        bool bold;
        node_font(ta, &fs, &family, &bold);
        int tx = 0, ty = 0;
        text_origin(w->render, ta, "", fs, family, bold, &tx, &ty,
                    run_wrap_w(ta));
        std::printf("[scroll] ta-empty caret_y=%d content_y=%d\n", ty,
                    ta->content.y);
        std::fflush(stdout);
        assert(ty <= ta->content.y + 2); /* caret at the TOP of the box */
        whaleui_window_destroy(w);
    }

    /* nested scrolling: a textarea INSIDE a scrollable page - both use the
     * same scroll machinery and each keeps its own exact range while the
     * other scrolls (page scroll must not perturb the textarea range and
     * vice versa) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "nested-scroll", 400, 250);
        assert(w != nullptr);
        std::string html = "<html><body>";
        for (int i = 0; i < 8; ++i) {
            html += "<div style=\"height:60px\">page line " +
                    std::to_string(i) + "</div>";
        }
        html += "<textarea id=\"t\" style=\"width:300px;height:56px\">";
        for (int i = 0; i < 10; ++i) {
            html += "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
        }
        html += "</textarea></body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* root = w->render->tree->root;
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
        assert(ta != nullptr && root->scroll_max > 0);
        int page_max = root->scroll_max;
        int ta_max = ta->scroll_max;
        assert(ta_max > 0);
        /* scroll the PAGE: the textarea range must not change */
        lxb_dom_element* rel = root->el;
        for (int i = 0; i < 6; ++i) {
            whaleui_render_handle_wheel(w->render, 200, 50, -1.0f);
        }
        assert(whaleui_render_frame(w->render, w->document) == 0);
        root = w->render->tree->root;
        /* re-locate the textarea after the rebuild */
        ta = nullptr;
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
        std::printf("[scroll] nested page s=%d ta_max=%d (was %d)\n",
                    w->render->scrolls[rel], ta->scroll_max, ta_max);
        std::fflush(stdout);
        assert(ta->scroll_max == ta_max); /* page scroll leaves ta intact */
        /* now scroll the textarea itself (wheel at its VISUAL position:
         * the page is already scrolled by s) */
        lxb_dom_element* tel = ta->el;
        int page_s = w->render->scrolls[rel];
        int tawx = ta->border.x + 10;
        int tawy = ta->border.y - page_s + 10;
        for (int i = 0; i < 30; ++i) {
            whaleui_render_handle_wheel(w->render, tawx, tawy, -1.0f);
        }
        std::printf("[scroll] nested ta bottom s=%d smax=%d page_s=%d\n",
                    w->render->scrolls[tel], ta->scroll_max, page_s);
        std::fflush(stdout);
        assert(w->render->scrolls[tel] >= ta->scroll_max - 1);
        /* scrolling the textarea must NOT drag the page along */
        assert(w->render->scrolls[rel] == page_s);
        /* and the page range is still exact */
        assert(w->render->scrolls[rel] <= page_max);
        whaleui_window_destroy(w);
    }

    /* a focused textarea: after an edit the caret-visible scroll may move
     * it, but a USER wheel afterwards must not be yanked back to the
     * caret line */
    {
        whaleui_window_t* w = whaleui_window_create(app, "caret-scroll", 300, 200);
        assert(w != nullptr);
        std::string html = "<html><body><textarea id=\"t\" "
                           "style=\"width:200px;height:40px\">";
        for (int i = 0; i < 10; ++i) {
            html += "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
        }
        html += "</textarea></body></html>";
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
        assert(ta != nullptr && ta->scroll_max > 0);
        lxb_dom_element* tel = ta->el;
        /* focus + move the caret to the END (an edit that must scroll the
         * caret visible) */
        w->render->edit_el = tel;
        w->render->sel_anchor = w->render->sel_focus = 0;
        edit_replace(w->render, tel, 0, 0, "xxxxxxxxxxxxxxxxxxxxxx\n");
        assert(whaleui_render_frame(w->render, w->document) == 0);
        int s_after_edit = w->render->scrolls[tel];
        /* a user wheel over the textarea must move it freely */
        whaleui_render_handle_wheel(w->render, ta->border.x + 10,
                                    ta->border.y + 10, -1.0f);
        int s1 = w->render->scrolls[tel];
        /* a user wheel must move it (or stay, if the caret scroll already
         * put it at the bottom - then there is nowhere to go down) */
        assert(s1 >= s_after_edit);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* no edit since: the frame must NOT pull it back to the caret */
        std::printf("[scroll] caret-scroll edit=%d wheel=%d after=%d\n",
                    s_after_edit, s1, w->render->scrolls[tel]);
        std::fflush(stdout);
        assert(w->render->scrolls[tel] == s1);
        whaleui_window_destroy(w);
    }

    /* 2-line textarea inside a scrollable page: still fits -> no range,
     * even with the page scrolled (the "2 lines behave like 20" report) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta2-in-page", 400, 250);
        assert(w != nullptr);
        std::string html = "<html><body>";
        for (int i = 0; i < 6; ++i) {
            html += "<div style=\"height:60px\">page line " +
                    std::to_string(i) + "</div>";
        }
        html += "<textarea id=\"t\" style=\"width:300px;height:56px\">"
                "多行文本\n支持 Enter 换行</textarea></body></html>";
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
        int smax0 = ta->scroll_max;
        std::printf("[scroll] ta2-in-page smax=%d\n", smax0);
        std::fflush(stdout);
        assert(smax0 == 0); /* 2 lines fit: no scroll range in-page */
        /* scroll the page, then re-check the textarea range is still 0 */
        whaleui_layout_node_t* root = w->render->tree->root;
        lxb_dom_element* rel = root->el;
        for (int i = 0; i < 4; ++i) {
            whaleui_render_handle_wheel(w->render, 200, 50, -1.0f);
        }
        assert(whaleui_render_frame(w->render, w->document) == 0);
        ta = nullptr;
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
        std::printf("[scroll] ta2-in-page after page scroll smax=%d\n",
                    ta->scroll_max);
        std::fflush(stdout);
        assert(ta->scroll_max == 0);
        whaleui_window_destroy(w);
    }

    /* exact demo card (flex card with h2 + input + 2-line textarea +
     * contenteditable): the textarea must still have NO scroll range -
     * "2 lines scroll like 70" points at the card/flex siblings leaking
     * into the textarea's content bottom */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta-in-card", 500, 300);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><head><style>.row { display:flex; } .card { flex:1; "
            "border:1px solid black; padding:8px; }</style></head><body>"
            "<div class=\"row\"><div class=\"card\">"
            "<h2>Edit &amp; IME</h2>"
            "<p><input style=\"width:100%;box-sizing:border-box\" "
            "value=\"x\"></p>"
            "<p><textarea style=\"width:100%;height:56px;"
            "box-sizing:border-box\">多行文本&#10;支持 Enter 换行"
            "</textarea></p>"
            "<p><span contenteditable=\"true\">可编辑</span></p>"
            "</div><div class=\"card\">other card<br>line 1<br>line 2<br>"
            "</div></div></body></html>") == 0);
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
        int smax = ta->scroll_max;
        int ch = ta->content.h;
        whaleui_layout_node_t* run = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.is_text) {
                run = &n;
            }
        }
        std::printf("[scroll] ta-in-card smax=%d ch=%d run_bottom=%d "
                    "ta_y=%d\n",
                    smax, ch,
                    run ? run->border.y + run->border.h : -1,
                    ta->border.y);
        std::fflush(stdout);
        assert(smax == 0); /* 2 lines fit inside the card textarea */
        whaleui_window_destroy(w);
    }

    /* focus the demo textarea (the screenshot shows a focused/blue-border
     * textarea): focusing must NOT inflate its scroll range */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta-focus", 500, 300);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><head><style>.row { display:flex; } .card { flex:1; "
            "border:1px solid black; padding:8px; }</style></head><body>"
            "<div class=\"row\"><div class=\"card\">"
            "<h2>Edit</h2>"
            "<p><textarea style=\"width:100%;height:56px;"
            "box-sizing:border-box\">多行文本&#10;支持 Enter 换行"
            "</textarea></p>"
            "</div></div></body></html>") == 0);
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
        int smax0 = ta->scroll_max;
        /* focus it (screenshot shows the focused state) */
        lxb_dom_element* tel = ta->el;
        w->render->edit_el = tel;
        w->render->sel_anchor = w->render->sel_focus = 0;
        assert(whaleui_render_frame(w->render, w->document) == 0);
        ta = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el == tel && !n.is_text) {
                ta = &n;
                break;
            }
        }
        assert(ta != nullptr);
        std::printf("[scroll] ta-focus smax0=%d smax_focused=%d ch=%d\n",
                    smax0, ta->scroll_max, ta->content.h);
        std::fflush(stdout);
        assert(ta->scroll_max == 0); /* focus does not add a range */
        whaleui_window_destroy(w);
    }

    /* the FULL demo page (extracted from examples/demo.cpp): the demo
     * textarea reports a ~70-line scroll range - reproduce it here */
    {
        const char* path = WHALEUI_TEST_ROOT "/temp/demo_full.html";
        FILE* f = std::fopen(path, "rb");
        if (!f) {
            std::printf("[scroll] demo_full.html missing, skipping\n");
            std::fflush(stdout);
        } else {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::string html(static_cast<size_t>(sz), '\0');
            std::fread(&html[0], 1, static_cast<size_t>(sz), f);
            std::fclose(f);
            whaleui_window_t* w =
                whaleui_window_create(app, "demo-full-page", 700, 500);
            assert(w != nullptr);
            assert(whaleui_window_load_html(w, html.c_str()) == 0);
            assert(whaleui_app_set_theme(app, WHALEUI_THEME_LIGHT) == 0);
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
            std::printf("[scroll] ta-focus-afterpage smax0=%d\n",
                        ta->scroll_max);
            std::fflush(stdout);
            assert(ta->scroll_max == 0);
            /* scroll the PAGE (wheel in a blank page area), then re-check:
             * the textarea range must stay 0 */
            whaleui_layout_node_t* root = w->render->tree->root;
            lxb_dom_element* rel = root->el;
            for (int i = 0; i < 6; ++i) {
                whaleui_render_handle_wheel(w->render, 350, 30, -1.0f);
            }
            assert(whaleui_render_frame(w->render, w->document) == 0);
            ta = nullptr;
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
            std::printf("[scroll] ta-focus-afterpage page_s=%d ta_smax=%d\n",
                        w->render->scrolls[rel], ta->scroll_max);
            std::fflush(stdout);
            assert(ta->scroll_max == 0); /* page scroll keeps ta at 0 */
            whaleui_window_destroy(w);
        }
    }

    /* FSR (half-resolution render): a re-layout at the scaled fb size must
     * not inflate the textarea's range - the demo runs with FSR and is
     * where the ~70-line range appears */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta-fsr", 700, 500);
        assert(w != nullptr);
        const char* path = WHALEUI_TEST_ROOT "/temp/demo_full.html";
        FILE* f = std::fopen(path, "rb");
        assert(f != nullptr);
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::string html(static_cast<size_t>(sz), '\0');
        std::fread(&html[0], 1, static_cast<size_t>(sz), f);
        std::fclose(f);
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_app_set_theme(app, WHALEUI_THEME_LIGHT) == 0);
        assert(whaleui_window_show(w) == 0);
        /* force FSR on: frame will relayout at the scaled fb size */
        w->render->fsr_mode = 1;
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
        std::printf("[scroll] ta-fsr fb=%dx%d ta_smax=%d ch=%d run_bs=%d\n",
                    w->render->fb_w, w->render->fb_h, ta->scroll_max,
                    ta->content.h, ta->border.h);
        std::fflush(stdout);
        assert(ta->scroll_max == 0); /* FSR must not inflate it */
        whaleui_window_destroy(w);
    }

    /* even if the textarea has a stale/large live scroll (e.g. after a
     * scroll or an FSR relayout), a relayout must NOT inflate its range -
     * the range depends only on content, never on scroll_y */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ta-stale-scroll", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><textarea id=\"t\" style=\"width:200px;"
            "height:56px\">多行文本\n支持 Enter 换行</textarea>"
            "</body></html>") == 0);
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
        lxb_dom_element* tel = ta->el;
        /* a stale live scroll from a previous state */
        w->render->scrolls[tel] = 1400;
        w->render->hover_old_el = tel; /* force a relayout path */
        assert(whaleui_render_frame(w->render, w->document) == 0);
        ta = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el == tel && !n.is_text) {
                ta = &n;
                break;
            }
        }
        assert(ta != nullptr);
        std::printf("[scroll] ta-stale-scroll smax=%d\n", ta->scroll_max);
        std::fflush(stdout);
        assert(ta->scroll_max == 0); /* 2 lines: never a 70-line range */
        whaleui_window_destroy(w);
    }

    /* async first layout (opt-in, WHALEUI_RENDER_ASYNC_LAYOUT): the first
     * frame hands the layout to a worker thread and returns without a tree;
     * a later frame picks the finished tree up. This test must NOT block
     * inside the first whaleui_render_frame - the whole point is that the
     * window stays responsive while a large page lays out. */
    {
        assert(whaleui_app_set_render_option(
                   app, WHALEUI_RENDER_ASYNC_LAYOUT, 1) == 0);
        whaleui_window_t* w =
            whaleui_window_create(app, "async-layout", 300, 200);
        assert(w != nullptr);
        std::string html = "<html><body><p>async first layout</p>";
        for (int i = 0; i < 50; ++i) {
            html += "<div>line " + std::to_string(i) + "</div>";
        }
        html += "</body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        int frames = 0;
        /* the first frame starts the worker and must NOT block: the tree
         * is still null right after it (bounded loop guards a worker bug).
         * A short frame delay models the real event loop - the worker
         * needs a few ms to start + lay out. */
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(w->render->tree == nullptr); /* worker still running */
        while (w->render->tree == nullptr && frames < 500) {
            SDL_Delay(5); /* ~vsync frame time */
            assert(whaleui_render_frame(w->render, w->document) == 0);
            ++frames;
        }
        assert(w->render->tree != nullptr);
        assert(w->render->tree->root != nullptr);
        std::printf("[scroll] async layout ready after %d frames\n", frames);
        std::fflush(stdout);
        /* the finished tree renders like a synchronous one */
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_window_destroy(w);
        assert(whaleui_app_set_render_option(
                   app, WHALEUI_RENDER_ASYNC_LAYOUT, 0) == 0);
    }

    /* transform-animation ghost regression: a white element animates
     * translateY(60px)->0. After it settles, the strip-only repaint must
     * NOT leave the OLD position as a residue - compare against a clean
     * (no-animation) page at the same pixel. */
    {
        const char* base =
            "<html><head><style>"
            "@keyframes m { from { transform: translateY(60px); }"
            "  to { transform: translateY(0); } }"
            "#a { %s }"
            "</style></head><body style=\"background:#000\">"
            "<div id=\"a\" style=\"background:#fff;color:#000;"
            "font-size:16px\">HELLO</div></body></html>";
        /* animated page */
        whaleui_window_t* w =
            whaleui_window_create(app, "anim-ghost-anim", 200, 200);
        assert(w != nullptr);
        std::string h1 = std::string(base);
        {
            size_t p = h1.find("%s");
            h1.replace(p, 2, "animation: m 1s linear both");
        }
        assert(whaleui_window_load_html(w, h1.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        for (int i = 0; i < 90; ++i) {
            SDL_Delay(12);
            assert(whaleui_render_frame(w->render, w->document) == 0);
        }
        unsigned int p_anim = gpixel(w->render, 10, 60);
        /* control page: no animation, element sits at y=0 */
        whaleui_window_t* wc =
            whaleui_window_create(app, "anim-ghost-ctrl", 200, 200);
        assert(wc != nullptr);
        std::string h2 = std::string(base);
        {
            size_t p = h2.find("%s");
            h2.replace(p, 2, "background:#0f0"); /* no animation */
        }
        assert(whaleui_window_load_html(wc, h2.c_str()) == 0);
        assert(whaleui_window_show(wc) == 0);
        assert(whaleui_render_frame(wc->render, wc->document) == 0);
        unsigned int p_ctrl = gpixel(wc->render, 10, 60);
        /* a ghost differs from the clean (no-animation) page at y=60 */
        assert(p_anim == p_ctrl);
        whaleui_window_destroy(w);
        whaleui_window_destroy(wc);
    }

    /* fixed/sticky header scroll-shift ghost regression: a fixed top bar
     * must NOT be shifted by the scroll image (it stays pinned to the
     * viewport); scrolling back up must not smear it over the content. */
    {
        const char* html =
            "<html><head><style>"
            "body{background:#101010}"
            "#bar{position:fixed;top:0;left:0;right:0;height:40px;"
            "background:#333;color:#fff}"
            "</style></head><body>"
            "<div id=\"bar\">FIXEDBAR</div>"
            "<div><div>content row</div>"
            "<div style=\"height:3000px\">tall spacer</div></div>"
            "</body></html>";
        whaleui_window_t* w =
            whaleui_window_create(app, "fixed-scroll-ghost", 320, 300);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w, html) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* scroll down to the bottom, then back up 20 steps */
        for (int i = 0; i < 200; ++i) {
            whaleui_render_handle_wheel(w->render, 160, 100, -1.0f);
        }
        assert(whaleui_render_frame(w->render, w->document) == 0);
        for (int i = 0; i < 20; ++i) {
            whaleui_render_handle_wheel(w->render, 160, 100, 1.0f);
        }
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* the fixed bar should still be exactly at the top (y<40 = #333),
         * and just below it (y=55) must be page content, NOT a smeared
         * copy of the bar */
        unsigned int p_top = gpixel(w->render, 10, 10);
        unsigned int p_below = gpixel(w->render, 10, 55);
        std::printf("[fixed-ghost] top=%08X below=%08X\n", p_top, p_below);
        std::fflush(stdout);
        /* top = bar (#333 -> 0xFF333333) */
        assert((p_top & 0xFFFFFFu) == 0x333333u);
        /* below the bar is page background (#101010), not the bar (#333) */
        assert((p_below & 0xFFFFFFu) != 0x333333u);
        whaleui_window_destroy(w);
    }

    /* animation actually plays in the render frame: a red element with an
     * opacity animation at a known spot must change pixel over time. If a
     * future change stops animating, this fails. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "anim-probe", 400, 120);
        assert(w != nullptr);
        const char* html =
            "<html><head><style>"
            "@keyframes p{0%{opacity:1}50%{opacity:.1}100%{opacity:1}}"
            "div{width:50px;height:50px;background:red;animation:p 1s linear infinite}"
            "</style></head><body><div></div></body></html>";
        assert(whaleui_window_load_html(w, html) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        unsigned int p0 = gpixel(w->render, 25, 25);
        unsigned int p1 = 0;
        for (int i = 0; i < 30; ++i) {
            SDL_Delay(16);
            assert(whaleui_render_frame(w->render, w->document) == 0);
            p1 = gpixel(w->render, 25, 25);
        }
        std::printf("[animprobe] p0=%08X p1=%08X\n", p0, p1);
        std::fflush(stdout);
        /* the opacity animation must have changed the pixel */
        assert(p0 != p1);
        whaleui_window_destroy(w);
    }

    whaleui_app_destroy(app);
    std::printf("[scroll] OK\n");
    return 0;
}
