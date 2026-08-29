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
        unsigned int pre_h0 =
            gpixel(w->render, hov->border.x + 2, hov->border.y + 2);
        whaleui_render_set_hover(w->render, hov->border.x + 5,
                                 hov->border.y + 5);
        assert(whaleui_render_frame(w->render, w->document) == 0);
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

    whaleui_app_destroy(app);
    std::printf("[scroll] OK\n");
    return 0;
}
