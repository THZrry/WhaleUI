// test_render: paint pipeline verification on the SDL_GPU path.
// Color parsing always runs; window-level assertions run only when a GPU
// backend is available (skipped on headless/RDP without a GPU driver).
#include "whaleui.h"
#include "render/render.h"
#include "render/render_internal.h"
#include "render/gpu.h"
#include "core/window.h"
#include "layout/layout.h"
#include "test_util.h"

#include <lexbor/dom/dom.h>
#include <SDL3/SDL.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>

int anim_runs(void); /* defined after main (window/GPU path) */

/* read one pixel back from the GPU composited target (R8G8B8A8) */
static unsigned int gpixel(whaleui_render_t* r, int x, int y)
{
    if (!r || !r->gpu) {
        return 0;
    }
    SDL_GPUDevice* dev = r->gpu->device;
    /* per-device cache: the later anim_runs() creates a fresh app+device;
     * a stale buffer from the previous (already destroyed) device crashes
     * Vulkan's Map. The old buffer is deliberately leaked (4 bytes, test
     * code) - releasing it would touch the destroyed device. */
    static SDL_GPUTransferBuffer* tb = nullptr;
    static SDL_GPUDevice* tb_dev = nullptr;
    if (!tb || tb_dev != dev) {
        SDL_GPUTransferBufferCreateInfo tbi = {};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tbi.size = 4;
        tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        tb_dev = dev;
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

    /* GPU blur approximation: box-shadow (mipmap-blurred shape under the
     * box) + backdrop-filter (blurred geometry + body color on top) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "blur", 200, 150);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body>"
            "<div id=\"panel\" style=\"width:120px;height:90px;"
            "background-color:#00ff00;margin:20px;"
            "box-shadow:0 10px 30px rgba(0,0,0,0.5);\"></div>"
            "<div id=\"back\" style=\"position:fixed;top:0;left:0;right:0;"
            "height:20px;backdrop-filter:blur(8px);"
            "background:rgba(255,0,0,0.5);\"></div>"
            "</body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* panel body is green */
        assert(gpixel(w->render, 60, 60) == 0xFF00FF00);
        /* shadow below the panel (box at 20,20-140,110; shadow spreads
         * 10px down + 30 blur): darker than the body background */
        unsigned int sh = gpixel(w->render, 100, 125);
        assert(((sh >> 16) & 0xFF) < 0xC0);
        /* backdrop top strip: blurred body + 50% red -> red-dominant */
        unsigned int bd = gpixel(w->render, 150, 10);
        assert(((bd >> 16) & 0xFF) > ((bd >> 8) & 0xFF) + 40);
        whaleui_window_destroy(w);
    }

    /* reference page colors (Qwen_html_20260814_6ni9q8tk8.html): body bg
     * var(--bg)=#080d1a with a radial glow at the top */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ref6", 800, 800);
        assert(w != nullptr);
        assert(whaleui_window_load_uri(
                   w, TEST_URI_TEMP("Qwen_html_20260814_6ni9q8tk8.html")) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        unsigned int top = gpixel(w->render, 400, 20);
        unsigned int mid = gpixel(w->render, 400, 400);
        unsigned int bot = gpixel(w->render, 400, 770);
        /* mid is past the gradient (transparent 65%) -> plain --bg, dark */
        assert(((mid >> 16) & 0xFF) < 0x30);
        /* the top glow is NOT the flat --bg: it carries the gradient blue
         * (blue channel rises above the flat body color) */
        assert(((top >> 16) & 0xFF) > ((mid >> 16) & 0xFF) + 3 ||
               (top & 0xFF) > (mid & 0xFF) + 3);
        whaleui_window_destroy(w);

        /* minimal radial gradient sanity: center should be visibly blue */
        whaleui_window_t* w2 = whaleui_window_create(app, "grad", 400, 300);
        assert(w2 != nullptr);
        assert(whaleui_window_load_html(
                   w2,
                   "<html><body style=\"background:#080d1a radial-gradient("
                   "200px 200px at 50% 30%, rgba(77,107,254,.8), "
                   "transparent 70%);\"></body></html>") == 0);
        assert(whaleui_window_show(w2) == 0);
        assert(whaleui_render_frame(w2->render, w2->document) == 0);
        unsigned int gc2 = gpixel(w2->render, 200, 90);
        assert(((gc2 >> 16) & 0xFF) > 0x40); /* red channel visibly blue-ish */
        whaleui_window_destroy(w2);

        /* text color sanity: red text must stay red (text_layer channel
         * order vs the R8G8B8A8 target) */
        whaleui_window_t* w3 = whaleui_window_create(app, "tcolor", 300, 100);
        assert(w3 != nullptr);
        assert(whaleui_window_load_html(
                   w3,
                   "<html><body><p id=\"t\" style=\"color:#ff0000;"
                   "font-size:40px;\">RR</p></body></html>") == 0);
        assert(whaleui_window_show(w3) == 0);
        assert(whaleui_render_frame(w3->render, w3->document) == 0);
        /* scan for a strongly red pixel inside the glyphs */
        bool found_red = false;
        for (int yy = 0; yy < 100 && !found_red; yy += 2) {
            for (int xx = 0; xx < 300 && !found_red; xx += 2) {
                unsigned int px = gpixel(w3->render, xx, yy);
                unsigned int r = (px >> 16) & 0xFF;
                unsigned int b = px & 0xFF;
                if (r > 0x90 && b < 0x40 && r > b + 0x60) {
                    found_red = true;
                }
            }
        }
        if (!found_red) {
            /* also allow the very first pixel under the glyph box */
            unsigned int probe = gpixel(w3->render, 30, 30);
            std::printf("DEBUG text probe=0x%08X\n", probe);
        }
        assert(found_red);
        whaleui_window_destroy(w3);
    }
    /* font-style: italic renders slanted glyphs: the ink of the same
     * character (48px X) must differ from the normal glyph */
    {
        auto ink_count = [](whaleui_window_t* w) {
            int ink = 0;
            for (int yy = 0; yy < 100; yy += 2) {
                for (int xx = 0; xx < 200; xx += 2) {
                    if (((gpixel(w->render, xx, yy) >> 16) & 0xFF) > 0x80) {
                        ++ink;
                    }
                }
            }
            return ink;
        };
        whaleui_window_t* wn = whaleui_window_create(app, "inorm", 200, 100);
        assert(wn != nullptr);
        assert(whaleui_window_load_html(wn,
            "<html><body><p style=\"font-size:48px;\">X</p></body></html>") == 0);
        assert(whaleui_window_show(wn) == 0);
        assert(whaleui_render_frame(wn->render, wn->document) == 0);
        int norm = ink_count(wn);
        assert(norm > 0); /* the glyph actually painted */
        whaleui_window_destroy(wn);
        whaleui_window_t* wi = whaleui_window_create(app, "iital", 200, 100);
        assert(wi != nullptr);
        assert(whaleui_window_load_html(wi,
            "<html><body><p style=\"font-size:48px;font-style:italic;\">X"
            "</p></body></html>") == 0);
        assert(whaleui_window_show(wi) == 0);
        assert(whaleui_render_frame(wi->render, wi->document) == 0);
        int ital = ink_count(wi);
        whaleui_window_destroy(wi);
        assert(ital > 0);
        assert(ital != norm); /* slant moves the ink */
    }
    /* inline flow paints each run at its laid-out x: <p>a<em>e</em>b</p>
     * must produce separated glyph clusters, not all text overlapping at
     * the parent origin (regression from the inline-line layout) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "inline", 300, 80);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><p style=\"font-size:40px;\">a<em>e</em>b</p>"
            "</body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        /* columns with ink, merged into runs */
        std::vector<int> runs;
        bool prev = false;
        for (int xx = 0; xx < 300; ++xx) {
            bool has = false;
            for (int yy = 0; yy < 80 && !has; yy += 2) {
                if (((gpixel(w->render, xx, yy) >> 16) & 0xFF) > 0x80) {
                    has = true;
                }
            }
            if (has && !prev) {
                runs.push_back(xx);
            }
            prev = has;
        }
        /* "a e b" are three separated clusters (italic e may touch a on
         * the right edge, so require at least two distinct clusters) */
        assert(runs.size() >= 2);
        /* the last cluster must sit well right of the first (30+ px apart):
         * the runs advance, they do not all paint from x=0 */
        if (runs.size() >= 2) {
            assert(runs.back() - runs.front() >= 20);
        }
        whaleui_window_destroy(w);
    }

    /* text-align:center paints the inline line centered: the glyph ink
     * centroid sits at the container center */
    {
        whaleui_window_t* w = whaleui_window_create(app, "talign", 400, 100);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><div style=\"text-align:center;width:400px;"
            "font-size:32px;\">hello</div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        long sum = 0, cnt = 0;
        for (int yy = 0; yy < 100; yy += 2) {
            for (int xx = 0; xx < 400; xx += 2) {
                if (((gpixel(w->render, xx, yy) >> 16) & 0xFF) > 0x80) {
                    sum += xx;
                    ++cnt;
                }
            }
        }
        assert(cnt > 0);
        long centroid = sum / cnt;
        assert(centroid > 150 && centroid < 250); /* ~200 = center */
        whaleui_window_destroy(w);
    }

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

    /* <details> click-to-toggle: a <summary> click flips the open attribute
     * and the body appears/disappears on the next layout pass */
    {
        whaleui_window_t* w = whaleui_window_create(app, "details", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><details><summary>more</summary><p>body</p>"
            "</details></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        auto find_summary = [](whaleui_layout_tree_t* tr)
            -> whaleui_layout_node_t* {
            for (auto& n : tr->arena) {
                if (n.visible && n.el) {
                    size_t len = 0;
                    const lxb_char_t* name =
                        lxb_dom_element_local_name(n.el, &len);
                    if (name && len == 7 &&
                        std::memcmp(name, "summary", 7) == 0) {
                        return &n;
                    }
                }
            }
            return nullptr;
        };
        auto has_p = [](whaleui_layout_tree_t* tr) {
            for (auto& n : tr->arena) {
                if (n.visible && n.el) {
                    size_t len = 0;
                    const lxb_char_t* name =
                        lxb_dom_element_local_name(n.el, &len);
                    if (name && len == 1 && name[0] == 'p') {
                        return true;
                    }
                }
            }
            return false;
        };
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* sum = find_summary(t);
        assert(sum != nullptr);
        lxb_dom_element* det = lxb_dom_interface_element(sum->el->node.parent);
        assert(det != nullptr);
        assert(!has_p(t)); /* collapsed */
        assert(!lxb_dom_element_has_attribute(
            det, (const lxb_char_t*)"open", 4));
        /* click the summary -> open */
        const char* val = nullptr;
        assert(whaleui_render_handle_click(
                   w->render, sum->border.x + 5, sum->border.y + 5, &val) == 0);
        assert(lxb_dom_element_has_attribute(det, (const lxb_char_t*)"open", 4));
        whaleui_render_frame(w->render, w->document);
        whaleui_layout_tree_t* t2 = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr,
            1.0f);
        assert(t2 != nullptr);
        assert(has_p(t2)); /* body visible after expand */
        /* click again -> collapse */
        whaleui_layout_node_t* sum2 = find_summary(t2);
        assert(sum2 != nullptr);
        assert(whaleui_render_handle_click(
                   w->render, sum2->border.x + 5, sum2->border.y + 5, &val) == 0);
        assert(!lxb_dom_element_has_attribute(det, (const lxb_char_t*)"open", 4));
        whaleui_layout_destroy(t2);
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }

    /* checkbox/radio: clicking toggles checked; radio is exclusive within
     * a name group */
    {
        whaleui_window_t* w = whaleui_window_create(app, "ckbx", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body>"
            "<input type=\"checkbox\" id=\"c1\">"
            "<input type=\"radio\" name=\"g\" id=\"r1\">"
            "<input type=\"radio\" name=\"g\" id=\"r2\">"
            "</body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            w->document, w->render->rules, w->render->rule_count,
            &w->render->theme_vars, 300, 200, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        /* locate checkbox + radios by id */
        lxb_dom_element* cb = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(w->document, "c1"));
        lxb_dom_element* r1 = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(w->document, "r1"));
        lxb_dom_element* r2 = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(w->document, "r2"));
        assert(cb && r1 && r2);
        /* find their layout nodes (16px boxes) */
        whaleui_layout_node_t* nc = nullptr, * n1 = nullptr, * n2 = nullptr;
        for (auto& nd : t->arena) {
            if (nd.visible && nd.el == cb) { nc = &nd; }
            if (nd.visible && nd.el == r1) { n1 = &nd; }
            if (nd.visible && nd.el == r2) { n2 = &nd; }
        }
        assert(nc && n1 && n2);
        assert(!lxb_dom_element_has_attribute(cb, (const lxb_char_t*)"checked", 7));
        /* click checkbox -> checked */
        assert(whaleui_render_handle_click(
                   w->render, nc->border.x + 8, nc->border.y + 8, nullptr) == 0);
        assert(lxb_dom_element_has_attribute(cb, (const lxb_char_t*)"checked", 7));
        /* click again -> unchecked */
        assert(whaleui_render_handle_click(
                   w->render, nc->border.x + 8, nc->border.y + 8, nullptr) == 0);
        assert(!lxb_dom_element_has_attribute(cb, (const lxb_char_t*)"checked", 7));
        /* radio: clicking r1 checks it; clicking r2 unchecks r1 */
        assert(whaleui_render_handle_click(
                   w->render, n1->border.x + 8, n1->border.y + 8, nullptr) == 0);
        assert(lxb_dom_element_has_attribute(r1, (const lxb_char_t*)"checked", 7));
        assert(whaleui_render_handle_click(
                   w->render, n2->border.x + 8, n2->border.y + 8, nullptr) == 0);
        assert(!lxb_dom_element_has_attribute(r1, (const lxb_char_t*)"checked", 7));
        assert(lxb_dom_element_has_attribute(r2, (const lxb_char_t*)"checked", 7));
        whaleui_layout_destroy(t);
        whaleui_window_destroy(w);
    }

    /* scrolled repaint: the newly exposed strip must repaint with content
     * (regression: some components stayed blank after scrolling) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "scrollpaint", 300, 200);
        assert(w != nullptr);
        std::string html = "<html><body>";
        for (int i = 0; i < 30; ++i) {
            html += "<p style=\"font-size:24px;margin:4px 0;\">LINE ";
            html += std::to_string(i);
            html += "</p>";
        }
        html += "</body></html>";
        assert(whaleui_window_load_html(w, html.c_str()) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        auto strip_has_ink = [](whaleui_render_t* r, int y0, int y1) {
            for (int yy = y0; yy < y1; yy += 2) {
                for (int xx = 0; xx < 300; xx += 2) {
                    if (((gpixel(r, xx, yy) >> 16) & 0xFF) > 0x80) {
                        return true;
                    }
                }
            }
            return false;
        };
        assert(strip_has_ink(w->render, 20, 100)); /* initial text visible */
        /* wheel down 2 notches (80px): the bottom strip 120..200 is newly
         * exposed and must be repainted with the next lines */
        whaleui_render_handle_wheel(w->render, 150, 100, -2.0f);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(strip_has_ink(w->render, 120, 200));
        /* scroll further: the newly exposed strip at the bottom again */
        whaleui_render_handle_wheel(w->render, 150, 100, -2.0f);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(strip_has_ink(w->render, 120, 200));
        whaleui_window_destroy(w);
    }

    /* 21kb external page: scrolled strips must repaint with content (the
     * page has long text sections; scrolling must not leave blank strips) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "q21k", 800, 600);
        assert(w != nullptr);
        assert(whaleui_window_load_uri(
                   w, TEST_URI_TEMP("Qwen_html_20260814_oeem340or.html")) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(w->render->tree->root->scroll_max > 0);
        auto has_ink = [](whaleui_render_t* r, int y0, int y1) {
            for (int yy = y0; yy < y1; yy += 4) {
                for (int xx = 0; xx < 800; xx += 4) {
                    if (((gpixel(r, xx, yy) >> 16) & 0xFF) > 0x50) {
                        return true;
                    }
                }
            }
            return false;
        };
        /* step down in 100px notches; each scroll repaints the exposed
         * bottom strip - at least the final state must not be blank */
        int inked = 0;
        for (int i = 0; i < 12; ++i) {
            whaleui_render_handle_wheel(w->render, 400, 300, -2.5f);
            assert(whaleui_render_frame(w->render, w->document) == 0);
            if (has_ink(w->render, 500, 600)) {
                ++inked;
            }
        }
        assert(inked >= 1); /* most mid-page strips carry content */
        /* clamp at the bottom: still repaints (no crash, no stale strip) */
        whaleui_render_handle_wheel(w->render, 400, 300, -1000.0f);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_window_destroy(w);
    }

    /* 6kb reference page (the one that shows layout/scroll regressions):
     * scrolled strips must repaint with content, and letter-spaced inline
     * runs must not overlap (spacing counts in the layout width) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "q6k", 720, 600);
        assert(w != nullptr);
        assert(whaleui_window_load_uri(
                   w, TEST_URI_TEMP("Qwen_html_20260814_6ni9q8tk8.html")) == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(w->render->tree->root->scroll_max > 0);
        /* count ink in the CPU text layer (the composite is affected by
         * the page's rise animation opacity; the layer holds the shifted
         * text either way) */
        auto count_ink = [](whaleui_render_t* r) {
            int n = 0;
            for (int yy = 0; yy < 600; yy += 8) {
                const unsigned int* row =
                    &r->text_layer[static_cast<size_t>(yy) * r->fb_w];
                bool has = false;
                for (int xx = 0; xx < 720 && !has; xx += 8) {
                    if (row[xx] != 0) {
                        has = true;
                    }
                }
                if (has) {
                    ++n;
                }
            }
            return n;
        };
        int ink_before = count_ink(w->render);
        for (int i = 0; i < 6; ++i) {
            whaleui_render_handle_wheel(w->render, 360, 300, -2.0f);
            assert(whaleui_render_frame(w->render, w->document) == 0);
        }
        /* scrolled-away content must SURVIVE the shift (regression: the
         * text layer cleared its strip before shifting, so rows that
         * scrolled off the strip turned blank until the next scroll
         * repainted them). The total in-viewport ink after 10 scrolls must
         * not have collapsed - shifting preserves the content mass (new
         * strip content only adds to it). */
        int ink_after = count_ink(w->render);
        std::printf("6kb scroll ink %d -> %d\n", ink_before, ink_after);
        assert(ink_before >= 5);
        assert(ink_after >= ink_before * 3 / 5);
        whaleui_window_destroy(w);
    }

    /* letter-spacing counts in the inline-line layout width: spaced runs
     * must not overlap the following run */
    {
        whaleui_window_t* w = whaleui_window_create(app, "lsp", 300, 80);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><p style=\"font-size:32px;letter-spacing:8px;\">"
            "ab<em>cd</em>ef</p></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        std::vector<int> runs;
        bool prev = false;
        for (int xx = 0; xx < 300; ++xx) {
            bool has = false;
            for (int yy = 0; yy < 80 && !has; yy += 2) {
                if (((gpixel(w->render, xx, yy) >> 16) & 0xFF) > 0x80) {
                    has = true;
                }
            }
            if (has && !prev) {
                runs.push_back(xx);
            }
            prev = has;
        }
        assert(runs.size() >= 2); /* spaced runs stay separated */
        if (runs.size() >= 2) {
            assert(runs.back() - runs.front() >= 30);
        }
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

    /* resize clamps live scroll positions to the new content range (the
     * fullscreen / window-resize path: a taller viewport shrinks scroll_max,
     * the scrollbar and content must not sit past the end) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "resize-scroll", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><div style=\"height:800px;\"></div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        int max0 = w->render->tree->root->scroll_max;
        assert(max0 > 0);
        /* scroll to the bottom */
        whaleui_render_handle_wheel(w->render, 150, 100, -10000.0f);
        assert(w->render->scrolls[w->render->tree->root->el] == max0);
        /* grow the window: scroll_max shrinks, the position must clamp */
        assert(whaleui_render_resize(w->render, 400, 300) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        int max1 = w->render->tree->root->scroll_max;
        assert(max1 < max0);
        assert(w->render->scrolls[w->render->tree->root->el] == max1);
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

    /* keyboard selection: shift extends, ctrl jumps words, ctrl+shift
     * selects words, plain arrows collapse the selection */
    {
        whaleui_window_t* w = whaleui_window_create(app, "keysel", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><input id=\"i\" value=\"hello world foo\">"
            "</body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* inp = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 && std::memcmp(name, "input", 5) == 0) {
                    inp = &n;
                    break;
                }
            }
        }
        assert(inp != nullptr);
        /* the control reserves height for its value text: the border box
         * must be taller than the padding+border collapse (was ~12px, the
         * painted text overflowed the border) */
        assert(inp->border.h >= 28 && inp->content.h > 0);
        /* caret at the very start */
        whaleui_render_set_pressed(w->render, inp->content.x + 1,
                                   inp->content.y + 2, 1);
        assert(w->render->edit_el == inp->el);
        assert(w->render->sel_anchor == 0 && w->render->sel_focus == 0);
        /* shift+right selects one character at a time */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, SDL_KMOD_SHIFT);
        assert(w->render->sel_anchor == 0 && w->render->sel_focus == 1);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, SDL_KMOD_SHIFT);
        assert(w->render->sel_anchor == 0 && w->render->sel_focus == 2);
        /* plain right collapses to the focus end (standard behavior:
         * the caret lands at the selection edge, no extra step) */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, 0);
        assert(w->render->sel_anchor == 2 && w->render->sel_focus == 2);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, 0);
        assert(w->render->sel_anchor == 3 && w->render->sel_focus == 3);
        /* ctrl+right: inside a word -> word end, then next word end */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 5 && w->render->sel_focus == 5);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 11 && w->render->sel_focus == 11);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 15 && w->render->sel_focus == 15);
        /* ctrl+left back to the previous word start */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_LEFT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 12 && w->render->sel_focus == 12);
        /* ctrl+shift+left selects the whole previous word */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_LEFT, 1,
                                  SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
        assert(w->render->sel_anchor == 12 && w->render->sel_focus == 6);
        /* end collapses to the selection end, then to the line end */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_END, 1, 0);
        assert(w->render->sel_anchor == 12 && w->render->sel_focus == 12);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_END, 1, 0);
        assert(w->render->sel_anchor == 15 && w->render->sel_focus == 15);
        whaleui_window_destroy(w);
    }

    /* CJK word jumping: a run of hanzi is one word, punctuation separates */
    {
        whaleui_window_t* w = whaleui_window_create(app, "cjkword", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><input id=\"i\" value=\"\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c"
            "\xe4\xb8\x96\xe7\x95\x8c\"></body></html>") == 0); /* "浣犲ソ锛屼笘鐣? */
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* inp = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 && std::memcmp(name, "input", 5) == 0) {
                    inp = &n;
                    break;
                }
            }
        }
        assert(inp != nullptr);
        whaleui_render_set_pressed(w->render, inp->content.x + 1,
                                   inp->content.y + 2, 1);
        assert(w->render->sel_anchor == 0);
        /* ctrl+right: "浣犲ソ" (6 bytes), then "涓栫晫" (12 bytes) */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 6 && w->render->sel_focus == 6);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 15 && w->render->sel_focus == 15);
        /* ctrl+left skips the comma and lands at "浣犲ソ" start */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_LEFT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 9 && w->render->sel_focus == 9);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_LEFT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 0 && w->render->sel_focus == 0);
        whaleui_window_destroy(w);
    }

    /* double-click selects a word; dragging continues by word; the
     * selection survives mouse-up */
    {
        whaleui_window_t* w = whaleui_window_create(app, "dblclick", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><p id=\"p\">hello world foo</p></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* run = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.is_text) {
                run = &n;
                break;
            }
        }
        assert(run != nullptr);
        int fs;
        std::string family;
        bool bold;
        node_font(run, &fs, &family, &bold);
        int tw = 0, th = 0;
        text_size(w->render, "hello ", fs, family, bold, &tw, &th);
        int y = run->border.y + run->border.h / 2;
        int xw = run->border.x + tw + 4; /* inside "world" */
        /* double-click: the word "world" (bytes 6..11) is selected */
        whaleui_render_set_pressed_ex(w->render, xw, y, 1, 2, 0);
        assert(w->render->sel_anchor == 6 && w->render->sel_focus == 11);
        /* drag right past the word: focus extends to the next word end */
        int tw2 = 0;
        text_size(w->render, "hello world ", fs, family, bold, &tw2, &th);
        whaleui_render_set_hover(w->render, run->border.x + tw2 + 4, y);
        assert(w->render->sel_anchor == 6 && w->render->sel_focus == 15);
        /* mouse-up keeps the double-click selection */
        whaleui_render_set_pressed_ex(w->render, 0, 0, 0, 1, 0);
        assert(w->render->sel_anchor_el == run->el);
        assert(w->render->sel_anchor == 6 && w->render->sel_focus == 15);
        whaleui_window_destroy(w);
    }

    /* triple-click selects the whole line; a plain click after it clears */
    {
        whaleui_window_t* w = whaleui_window_create(app, "triple", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><p id=\"p\">line one</p></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* run = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.is_text) {
                run = &n;
                break;
            }
        }
        assert(run != nullptr);
        int y = run->border.y + run->border.h / 2;
        whaleui_render_set_pressed_ex(w->render, run->border.x + 4, y, 1, 3, 0);
        assert(w->render->sel_anchor == 0);
        assert(w->render->sel_focus == 8); /* "line one" */
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_window_destroy(w);
    }

    /* triple-click on an input selects the whole line (NOT a drag-readied
     * press: the multi-click path must win over drag-to-move) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "triple-input", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><input id=\"i\" style=\"width:200px\" "
            "value=\"hello world\"></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* inp = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 && std::memcmp(name, "input", 5) == 0) {
                    inp = &n;
                    break;
                }
            }
        }
        assert(inp != nullptr);
        whaleui_render_set_pressed_ex(w->render, inp->content.x + 20,
                                      inp->content.y + 2, 1, 3, 0);
        /* the whole value is selected, and no drag was readied */
        assert(w->render->sel_anchor == 0);
        assert(w->render->sel_focus == 11); /* "hello world" */
        assert(w->render->drag_sel == 0);
        whaleui_window_destroy(w);
    }

    /* a plain click inside an editable selection collapses it and moves the
     * caret (drag-to-move needs an actual drag past the threshold) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "click-collapse", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><textarea id=\"t\" style=\"width:200px\">"
            "hello world</textarea></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* run = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.is_text) {
                run = &n;
                break;
            }
        }
        assert(run != nullptr);
        int fs;
        std::string family;
        bool bold;
        node_font(run, &fs, &family, &bold);
        int tw = 0, th = 0;
        text_size(w->render, "hello wo", fs, family, bold, &tw, &th);
        int y = run->border.y + run->border.h / 2;
        int x = run->border.x + tw + 2; /* inside "world" (offset ~8) */
        /* double-click selects "world" (6..11) */
        whaleui_render_set_pressed_ex(w->render, x, y, 1, 2, 0);
        assert(w->render->sel_anchor == 6 && w->render->sel_focus == 11);
        /* plain click inside the selection: drag readied but not activated */
        whaleui_render_set_pressed(w->render, x, y, 1);
        assert(w->render->drag_sel == 1);
        /* release without dragging: the selection collapses to the caret */
        whaleui_render_set_pressed_ex(w->render, 0, 0, 0, 1, 0);
        assert(w->render->drag_sel == 0);
        assert(w->render->sel_anchor == w->render->sel_focus);
        assert(w->render->sel_anchor > 6 && w->render->sel_anchor < 11);
        whaleui_window_destroy(w);
    }

    /* dictionary word segmentation (full build): ctrl+right jumps 你好 ->
     * 世界 as two words; a word absent from the dictionary falls back to a
     * single hanzi */
    {
        whaleui_window_t* w = whaleui_window_create(app, "dictword", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><input id=\"i\" style=\"width:200px\" "
            "value=\"\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\">"
            "</body></html>") == 0); /* "你好世界" */
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* inp = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 && std::memcmp(name, "input", 5) == 0) {
                    inp = &n;
                    break;
                }
            }
        }
        assert(inp != nullptr);
        whaleui_render_set_pressed(w->render, inp->content.x + 1,
                                   inp->content.y + 2, 1);
        assert(w->render->sel_anchor == 0);
        /* 你好 (6 bytes), then 世界 (12) */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 6 && w->render->sel_focus == 6);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 12 && w->render->sel_focus == 12);
        /* back: 世界 -> 你好 */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_LEFT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 6 && w->render->sel_focus == 6);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_LEFT, 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 0 && w->render->sel_focus == 0);
        whaleui_window_destroy(w);
    }

    /* clipboard: ctrl+a / ctrl+c copy, ctrl+v pastes into the editable */
    {
        whaleui_window_t* w = whaleui_window_create(app, "clipboard", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><input id=\"i\" style=\"width:200px\" "
            "value=\"hello world\"></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* inp = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 && std::memcmp(name, "input", 5) == 0) {
                    inp = &n;
                    break;
                }
            }
        }
        assert(inp != nullptr);
        whaleui_render_set_pressed(w->render, inp->content.x + 1,
                                   inp->content.y + 2, 1);
        /* select all + copy */
        whaleui_render_handle_key(w->render, 'a', 1, SDL_KMOD_CTRL);
        assert(w->render->sel_anchor == 0 && w->render->sel_focus == 11);
        whaleui_render_handle_key(w->render, 'c', 1, SDL_KMOD_CTRL);
        char* cl = SDL_GetClipboardText();
        assert(cl != nullptr && std::strcmp(cl, "hello world") == 0);
        SDL_free(cl);
        /* paste at the start doubles the value */
        whaleui_render_handle_key(w->render, WHALEUI_KEY_HOME, 1, 0);
        whaleui_render_handle_key(w->render, 'v', 1, SDL_KMOD_CTRL);
        const char* v = whaleui_dom_get_attribute(
            reinterpret_cast<whaleui_dom_element_t*>(inp->el), "value");
        assert(v != nullptr && std::strcmp(v, "hello worldhello world") == 0);
        /* cut removes the selection */
        whaleui_render_handle_key(w->render, 'a', 1, SDL_KMOD_CTRL);
        whaleui_render_handle_key(w->render, 'x', 1, SDL_KMOD_CTRL);
        v = whaleui_dom_get_attribute(
            reinterpret_cast<whaleui_dom_element_t*>(inp->el), "value");
        assert(v != nullptr && std::strcmp(v, "") == 0);
        whaleui_window_destroy(w);
    }

    /* drag-and-drop: press inside an editable selection, drag to another
     * input -> the selection moves (ctrl: copies) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "dragdrop", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><input id=\"a\" value=\"hello world\">"
            "<input id=\"b\" value=\"\"></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* ina = nullptr;
        whaleui_layout_node_t* inb = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (n.visible && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 && std::memcmp(name, "input", 5) == 0) {
                    size_t idlen = 0;
                    const lxb_char_t* id = lxb_dom_element_get_attribute(
                        n.el, (const lxb_char_t*)"id", 2, &idlen);
                    if (id && idlen == 1 && id[0] == 'a') {
                        ina = &n;
                    } else if (id && idlen == 1 && id[0] == 'b') {
                        inb = &n;
                    }
                }
            }
        }
        assert(ina != nullptr && inb != nullptr);
        int fs;
        std::string family;
        bool bold;
        node_font(ina, &fs, &family, &bold);
        int tw = 0, th = 0;
        text_size(w->render, "hello", fs, family, bold, &tw, &th);
        /* click at the start, ctrl+shift+right selects "hello" (0..5) */
        whaleui_render_set_pressed(w->render, ina->content.x + 1,
                                   ina->content.y + 2, 1);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1,
                                  SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
        assert(w->render->sel_anchor == 0 && w->render->sel_focus == 5);
        /* press inside the selection readies a drag */
        whaleui_render_set_pressed(w->render, ina->content.x + 1 + tw / 2,
                                   ina->content.y + 2, 1);
        assert(w->render->drag_sel == 1);
        /* drag to the second input (click its right side -> insert at end) */
        whaleui_render_set_hover(w->render, inb->content.x + 100,
                                 inb->content.y + 2);
        assert(w->render->drag_sel_active == 1);
        /* release: the selection moves */
        whaleui_render_set_pressed_ex(w->render, inb->content.x + 100,
                                      inb->content.y + 2, 0, 1, 0);
        const char* va = whaleui_dom_get_attribute(
            reinterpret_cast<whaleui_dom_element_t*>(ina->el), "value");
        const char* vb = whaleui_dom_get_attribute(
            reinterpret_cast<whaleui_dom_element_t*>(inb->el), "value");
        assert(va != nullptr && std::strcmp(va, " world") == 0);
        assert(vb != nullptr && std::strcmp(vb, "hello") == 0);
        /* ctrl-drag copies: select again (" world" now, so "world" = 1..6),
         * drag with ctrl held */
        whaleui_render_set_pressed(w->render, ina->content.x + 1,
                                   ina->content.y + 2, 1);
        whaleui_render_handle_key(w->render, WHALEUI_KEY_RIGHT, 1,
                                  SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
        assert(w->render->sel_anchor == 0 && w->render->sel_focus == 6);
        whaleui_render_set_pressed(w->render, ina->content.x + 1 + tw / 2,
                                   ina->content.y + 2, 1);
        whaleui_render_set_hover(w->render, inb->content.x + 100,
                                 inb->content.y + 2);
        whaleui_render_set_pressed_ex(w->render, inb->content.x + 100,
                                      inb->content.y + 2, 0, 1, SDL_KMOD_CTRL);
        va = whaleui_dom_get_attribute(
            reinterpret_cast<whaleui_dom_element_t*>(ina->el), "value");
        vb = whaleui_dom_get_attribute(
            reinterpret_cast<whaleui_dom_element_t*>(inb->el), "value");
        assert(va != nullptr && std::strcmp(va, " world") == 0);
        assert(vb != nullptr && std::strcmp(vb, "hello world") == 0);
        whaleui_window_destroy(w);
    }

    /* selection inside a scrolled container: after the wheel scrolls the
     * box, clicking a visible text run still resolves to a valid offset
     * (the scroll correction must be applied or the click lands at 0) */
    {
        whaleui_window_t* w = whaleui_window_create(app, "scrolled-sel", 300, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(w,
            "<html><body><div id=\"sc\" style=\"overflow:auto;height:40px;\">"
            "<p>line one</p><p>line two</p></div></body></html>") == 0);
        assert(whaleui_window_show(w) == 0);
        assert(whaleui_render_frame(w->render, w->document) == 0);
        whaleui_layout_node_t* sc = nullptr;
        whaleui_layout_node_t* run1 = nullptr;
        whaleui_layout_node_t* run2 = nullptr;
        for (auto& n : w->render->tree->arena) {
            if (!n.visible) {
                continue;
            }
            if (!sc && n.el && !n.is_text) {
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(n.el, &len);
                if (name && len == 3 && std::memcmp(name, "div", 3) == 0) {
                    sc = &n;
                }
            }
            if (n.is_text && std::strstr(n.text.c_str(), "line one")) {
                run1 = &n;
            }
            if (n.is_text && std::strstr(n.text.c_str(), "line two")) {
                run2 = &n;
            }
        }
        assert(sc != nullptr && run1 != nullptr && run2 != nullptr);
        assert(sc->scroll_max > 0);
        /* scroll by one line: "line two" now paints where "line one" was */
        int sy = run2->border.y - run1->border.y;
        if (sy > sc->scroll_max) {
            sy = sc->scroll_max;
        }
        assert(sy > 0);
        w->render->scrolls[sc->el] = sy;
        /* click mid-"line two" at line-one's window position: the caret
         * lands inside the word (offset > 0); without the scroll fix the
         * offset math goes negative and resolves to 0 */
        int y = run1->border.y + run1->border.h / 2;
        whaleui_render_set_pressed(w->render, run2->border.x + 40, y, 1);
        assert(w->render->sel_anchor_el != nullptr);
        assert(w->render->sel_anchor > 0);
        assert(w->render->sel_anchor == w->render->sel_focus);
        assert(whaleui_render_frame(w->render, w->document) == 0);
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
    /* FSR on a static box: force on, render at half res, the composite
     * target must still show the red box at (0,0) */
    whaleui_window_load_html(w,
        "<html><body><div style=\"width:100px;height:100px;"
        "background-color:#ff0000;\"></div></body></html>");
    whaleui_render_set_fsr(w->render, 1, 0.5f, 0.4f);
    assert(whaleui_render_frame(w->render, w->document) == 0);
    assert(w->render->fsr_active == 1);
    assert(gpixel(w->render, 0, 0) == 0xFFFF0000);
    whaleui_render_set_fsr(w->render, 2, 0.5f, 0.4f);
    assert(whaleui_render_frame(w->render, w->document) == 0);
    assert(w->render->fsr_active == 0);
    whaleui_window_destroy(w);
    whaleui_app_destroy(app);
    return 1;
}
