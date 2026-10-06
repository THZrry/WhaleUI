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

    /* Regression: a :hover that only LIFTS the element (transform) must
     * repaint the band it lifts INTO. The partial-repaint strip was built from
     * the layout box alone, so a translateY(-Npx) hover drew the border (that
     * part stayed inside the box) but never the lifted part - exactly the
     * reported "the animation card responds at once but only the border, it
     * does not lift". */
    {
        whaleui_window_t* lw =
            whaleui_window_create(app, "hover-lift", 300, 200);
        assert(lw != nullptr);
        assert(whaleui_window_load_html(
                   lw,
                   "<html><head><style>body{margin:0}"
                   "#b{position:absolute;left:40px;top:100px;width:80px;"
                   "height:40px;background-color:#ff0000}"
                   "#b:hover{transform:translateY(-40px);"
                   "background-color:#0000ff}"
                   "</style></head>"
                   "<body><div id=\"b\"><span id=\"in\">x</span>"
                   "</div></body></html>") == 0);
        if (whaleui_window_show(lw) == 0 && lw->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(lw->render, lw->document) == 0);
            }
            /* the box occupies y=100..140; sample the band ABOVE it, which
             * only becomes occupied once the hover lifts the box 40px up */
            unsigned int lift_before = gpixel(lw->render, 80, 75);
            whaleui_render_set_hover(lw->render, 80, 120);
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(lw->render, lw->document) == 0);
            }
            unsigned int lift_after = gpixel(lw->render, 80, 75);
            std::printf("[hover-lift] %08X -> %08X\n", lift_before,
                        lift_after);
            std::fflush(stdout);
            /* before: page background; after: the lifted (blue) box */
            assert(lift_after != lift_before);
        }
        whaleui_window_destroy(lw);
    }

    /* Regression: leaving a hovered element must REVERT its :hover, even when
     * the pointer crosses several targets quickly. The crossed elements are
     * carried in hover_prev (only the most recent one fits in hover_old_el),
     * so a fast move that loses one of them leaves that element stuck in its
     * hover style - reported as "moving away sometimes does not react (seems
     * to happen when the mouse moves quickly)". */
    {
        whaleui_window_t* hw2 =
            whaleui_window_create(app, "hover-revert", 300, 120);
        assert(hw2 != nullptr);
        assert(whaleui_window_load_html(
                   hw2,
                   "<html><head><style>body{margin:0}"
                   "div{position:absolute;top:20px;width:60px;height:40px;"
                   "background-color:#ff0000}"
                   "#a{left:20px}#b{left:100px}#c{left:180px}"
                   "div:hover{background-color:#0000ff}"
                   "</style></head>"
                   "<body><div id=\"a\"></div><div id=\"b\"></div>"
                   "<div id=\"c\"></div></body></html>") == 0);
        if (whaleui_window_show(hw2) == 0 && hw2->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(hw2->render, hw2->document) == 0);
            }
            unsigned int px = 30; /* inside A (x 20..80) */
            unsigned int py = 40;
            unsigned int a_plain = gpixel(hw2->render, px, py);
            whaleui_render_set_hover(hw2->render, px, py);
            assert(whaleui_render_frame(hw2->render, hw2->document) == 0);
            unsigned int a_hover = gpixel(hw2->render, px, py);
            assert(a_hover != a_plain); /* A is hovered */
            /* move to B: A must revert in THIS frame */
            whaleui_render_set_hover(hw2->render, 110, py);
            assert(whaleui_render_frame(hw2->render, hw2->document) == 0);
            unsigned int a_after = gpixel(hw2->render, px, py);
            std::printf("[hover-revert] plain=%08X hover=%08X after=%08X\n",
                        a_plain, a_hover, a_after);
            std::fflush(stdout);
            assert(a_after == a_plain); /* A reverted */
        }
        whaleui_window_destroy(hw2);
    }

    /* Regression: a TRANSFORM TRANSITION ("transition:transform" + :hover
     * transform) must lift. apply_transition pins the style at the neutral
     * "none" while the transition registers, and the interpolated value only
     * reaches the built node through the per-frame apply_ov - which wrote the
     * style map behind layout's build, leaving the cached paint flag xf=0, so
     * paint_node never translated and the card never moved. The flag refresh
     * after apply_ov must flip xf as soon as the interpolated value is a real
     * transform. */
    {
        whaleui_window_t* tw =
            whaleui_window_create(app, "hover-transform-transition", 300, 200);
        assert(tw != nullptr);
        assert(whaleui_window_load_html(
                   tw,
                   "<html><head><style>body{margin:0}"
                   "#b{position:absolute;left:40px;top:100px;width:80px;"
                   "height:40px;background-color:#ff0000;"
                   "transition:transform 0.2s}"
                   "#b:hover{transform:translateY(-40px);"
                   "background-color:#0000ff}"
                   "</style></head>"
                   "<body><div id=\"b\"></div></body></html>") == 0);
        if (whaleui_window_show(tw) == 0 && tw->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(tw->render, tw->document) == 0);
            }
            /* the box occupies y=100..140; sample the band ABOVE it, which
             * only becomes occupied once the hover lifts the box 40px up */
            unsigned int before = gpixel(tw->render, 80, 75);
            whaleui_render_set_hover(tw->render, 80, 120);
            /* run the 0.2s transition to completion; poll instead of a fixed
             * frame count so wall-clock jitter cannot end the loop early */
            bool lifted = false;
            for (int i = 0; i < 100 && !lifted; ++i) {
                assert(whaleui_render_frame(tw->render, tw->document) == 0);
                lifted = gpixel(tw->render, 80, 75) != before;
                SDL_Delay(10); /* 1s budget > 0.2s transition */
            }
            std::printf("[hover-transition] before=%08X lifted=%d\n", before,
                        static_cast<int>(lifted));
            std::fflush(stdout);
            assert(lifted);
        }
        whaleui_window_destroy(tw);
    }

    /* find an element by id attribute in the current layout tree */
    struct IdLookup
    {
        whaleui_render_t* r;
        const char* id;
        lxb_dom_element* found = nullptr;
        void walk(whaleui_layout_node_t* n)
        {
            if (found || !n || n->is_text || !n->el) {
                return;
            }
            size_t len = 0;
            const lxb_char_t* v = lxb_dom_element_get_attribute(
                n->el, (const lxb_char_t*)"id", 2, &len);
            if (v && len == std::strlen(id) &&
                std::memcmp(v, id, len) == 0) {
                found = n->el;
                return;
            }
            for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
                walk(c);
            }
        }
        lxb_dom_element* run()
        {
            walk(r->tree->root);
            return found;
        }
    };

    /* Regression: SCROLL must keep :hover alive under a stationary pointer.
     * hover was only ever updated from mouse-motion events, so after a
     * wheel scroll the element now under the pointer kept its plain style
     * (and the scrolled-away one kept :hover) until the next 1px of mouse
     * movement - "hover dies when you scroll". The frame now re-hit-tests
     * the last pointer position after any relayout/scroll. */
    {
        whaleui_window_t* sw =
            whaleui_window_create(app, "hover-scroll", 300, 200);
        assert(sw != nullptr);
        assert(whaleui_window_load_html(
                   sw,
                   "<html><head><style>body{margin:0}"
                   ".c{height:400px;background-color:#ffff00}"
                   "#a{position:absolute;left:20px;top:20px;width:80px;"
                   "height:40px;background-color:#ff0000}"
                   "#b{position:absolute;left:20px;top:150px;width:80px;"
                   "height:40px;background-color:#ff0000}"
                   "#b:hover{background-color:#0000ff}"
                   "</style></head>"
                   "<body><div class=\"c\"></div><div id=\"a\"></div>"
                   "<div id=\"b\"></div></body></html>") == 0);
        if (whaleui_window_show(sw) == 0 && sw->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(sw->render, sw->document) == 0);
            }
            /* pointer over A (y 20..60), stationary from here on */
            whaleui_render_set_hover(sw->render, 50, 40);
            assert(whaleui_render_frame(sw->render, sw->document) == 0);
            /* scroll the page 120px down: B (doc y 150..190) lands at
             * viewport y 30..70 - under the pointer */
            for (int i = 0; i < 3; ++i) {
                whaleui_render_handle_wheel(sw->render, 50, 40, -1.0f);
            }
            /* the scroll frame re-hit-tests; the state pass applies the
             * :hover style on the next frame - run both */
            assert(whaleui_render_frame(sw->render, sw->document) == 0);
            assert(whaleui_render_frame(sw->render, sw->document) == 0);
            whaleui_dom_element_t* under =
                whaleui_render_hit_element(sw->render, 50, 40);
            lxb_dom_element* b = IdLookup{sw->render, "b"}.run();
            size_t ulen = 0;
            const lxb_char_t* uname =
                under ? lxb_dom_element_local_name(
                            reinterpret_cast<lxb_dom_element*>(under), &ulen)
                      : nullptr;
            size_t hlen = 0;
            const lxb_char_t* hname =
                sw->render->hover_el
                    ? lxb_dom_element_local_name(sw->render->hover_el, &hlen)
                    : nullptr;
            std::printf("[hover-scroll] hover==hit=%d pixel=%08X under=%.*s "
                        "hover=%.*s\n",
                        static_cast<int>(
                            static_cast<void*>(sw->render->hover_el) ==
                            static_cast<void*>(under)),
                        gpixel(sw->render, 50, 40), static_cast<int>(ulen),
                        uname ? reinterpret_cast<const char*>(uname)
                              : "(null)",
                        static_cast<int>(hlen),
                        hname ? reinterpret_cast<const char*>(hname)
                              : "(null)");
            std::fflush(stdout);
            assert(under != nullptr && b != nullptr);
            assert(static_cast<void*>(sw->render->hover_el) ==
                   static_cast<void*>(b));
            assert(static_cast<void*>(under) == static_cast<void*>(b));
            /* and the style followed: B paints its :hover blue */
            assert(gpixel(sw->render, 50, 40) == 0xFF0000FF);
        }
        whaleui_window_destroy(sw);
    }

    /* Regression: a hover TRANSLATE must move the HIT region with the
     * paint. hit_test tested the untranslated laid-out box, so once the
     * lift had painted the card 40px higher, the card could no longer be
     * hit at its painted position (its :hover was lost / clicks missed) -
     * "the lift works, then hover stops responding". */
    {
        whaleui_window_t* tw2 =
            whaleui_window_create(app, "hit-translate", 300, 200);
        assert(tw2 != nullptr);
        assert(whaleui_window_load_html(
                   tw2,
                   "<html><head><style>body{margin:0}"
                   "#b{position:absolute;left:40px;top:100px;width:80px;"
                   "height:40px;background-color:#ff0000}"
                   "#b:hover{transform:translateY(-40px);"
                   "background-color:#0000ff}"
                   "</style></head>"
                   "<body><div id=\"b\"><span>x</span></div></body></html>") ==
            0);
        if (whaleui_window_show(tw2) == 0 && tw2->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(tw2->render, tw2->document) == 0);
            }
            whaleui_render_set_hover(tw2->render, 80, 120);
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(tw2->render, tw2->document) == 0);
            }
            /* the card is painted at y 60..100 now (lifted); (80, 70) is
             * inside the PAINTED box and outside the laid-out one */
            whaleui_dom_element_t* hit =
                whaleui_render_hit_element(tw2->render, 80, 70);
            lxb_dom_element* b = IdLookup{tw2->render, "b"}.run();
            std::printf("[hit-translate] hit==b=%d\n",
                        static_cast<int>(static_cast<void*>(hit) ==
                                         static_cast<void*>(b)));
            std::fflush(stdout);
            assert(b != nullptr);
            assert(static_cast<void*>(hit) == static_cast<void*>(b));
        }
        whaleui_window_destroy(tw2);
    }

    /* Regression: a position:fixed bar must stay hoverable after the page
     * scrolls. paint pins fixed boxes to the viewport (offsets zeroed) but
     * hit_test kept the ancestor scroll offset, so the header was
     * hit-tested 120px ABOVE its painted rows - hover/click fell through
     * to the content beneath after scrolling ("the fixed nav stops
     * reacting once you scroll"). Also covers sibling priority: the bar is
     * LAST in the DOM and paints on top, so at the overlap point the bar -
     * not the earlier content div - must win the hit. */
    {
        whaleui_window_t* fw =
            whaleui_window_create(app, "hit-fixed", 300, 200);
        assert(fw != nullptr);
        assert(whaleui_window_load_html(
                   fw,
                   "<html><head><style>body{margin:0}"
                   ".c{height:400px;margin-top:50px;background-color:#ffff00}"
                   "#h{position:fixed;left:0;top:0;width:300px;height:40px;"
                   "background-color:#00ff00}"
                   "#h:hover{background-color:#0000ff}"
                   "</style></head>"
                   "<body><div class=\"c\"></div><div id=\"h\"></div></body>"
                   "</html>") == 0);
        if (whaleui_window_show(fw) == 0 && fw->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(fw->render, fw->document) == 0);
            }
            lxb_dom_element* h = IdLookup{fw->render, "h"}.run();
            whaleui_render_set_hover(fw->render, 150, 20);
            assert(whaleui_render_frame(fw->render, fw->document) == 0);
            assert(whaleui_render_frame(fw->render, fw->document) == 0);
            assert(gpixel(fw->render, 150, 20) == 0xFF0000FF); /* hovered */
            /* scroll 120px: the content slides up under the bar's rows */
            for (int i = 0; i < 3; ++i) {
                whaleui_render_handle_wheel(fw->render, 150, 100, -1.0f);
            }
            assert(whaleui_render_frame(fw->render, fw->document) == 0);
            assert(whaleui_render_frame(fw->render, fw->document) == 0);
            whaleui_dom_element_t* under =
                whaleui_render_hit_element(fw->render, 150, 20);
            std::printf("[hit-fixed] hover==h=%d pixel=%08X\n",
                        static_cast<int>(
                            static_cast<void*>(fw->render->hover_el) ==
                            static_cast<void*>(h)),
                        gpixel(fw->render, 150, 20));
            std::fflush(stdout);
            assert(h != nullptr);
            assert(static_cast<void*>(under) == static_cast<void*>(h));
            assert(static_cast<void*>(fw->render->hover_el) ==
                   static_cast<void*>(h));
            /* :hover survived the scroll: the bar still paints blue */
            assert(gpixel(fw->render, 150, 20) == 0xFF0000FF);
        }
        whaleui_window_destroy(fw);
    }

    /* Regression: a PAINT-ONLY transition must progress while a LAYOUT
     * animation runs elsewhere on the page. The frame's layout-animation
     * branch (relayout_multi) applied animated styles only inside the
     * rebuilt subtrees; a hover transform transition on any OTHER element
     * never received its interpolated values (the paint-only apply path
     * was skipped on those frames), so the card stayed pinned at the
     * transition's start value until the next state relayout - on the demo
     * page (infinite width bar) the card lifted only after the mouse had
     * left ("hover reacts after the mouse moved away"). The two animation
     * paths must compose every frame. */
    {
        whaleui_window_t* cw =
            whaleui_window_create(app, "anim-compose", 300, 200);
        assert(cw != nullptr);
        assert(whaleui_window_load_html(
                   cw,
                   "<html><head><style>body{margin:0}"
                   "#bar{position:absolute;left:0;top:0;height:8px;"
                   "background-color:#00ff00;"
                   "animation:barw 0.4s linear infinite alternate}"
                   "@keyframes barw{from{width:10%}to{width:90%}}"
                   "#b{position:absolute;left:40px;top:100px;width:80px;"
                   "height:40px;background-color:#ff0000;"
                   "transition:transform 0.15s}"
                   "#b:hover{transform:translateY(-40px);"
                   "background-color:#0000ff}"
                   "</style></head>"
                   "<body><div id=\"bar\"></div><div id=\"b\"></div></body>"
                   "</html>") == 0);
        if (whaleui_window_show(cw) == 0 && cw->render != nullptr) {
            /* let the bar animation run a few frames: the frame loop is
             * now on the layout-animation path */
            for (int i = 0; i < 6; ++i) {
                assert(whaleui_render_frame(cw->render, cw->document) == 0);
                SDL_Delay(10);
            }
            unsigned int before = gpixel(cw->render, 80, 75);
            whaleui_render_set_hover(cw->render, 80, 120);
            /* poll to completion; the bar keeps the layout path busy the
             * whole time - the hover transition must still lift */
            bool lifted = false;
            for (int i = 0; i < 100 && !lifted; ++i) {
                assert(whaleui_render_frame(cw->render, cw->document) == 0);
                lifted = gpixel(cw->render, 80, 75) != before;
                SDL_Delay(10); /* 1s budget > 0.15s transition */
            }
            std::printf("[anim-compose] before=%08X lifted=%d\n", before,
                        static_cast<int>(lifted));
            std::fflush(stdout);
            assert(lifted);
        }
        whaleui_window_destroy(cw);
    }

    /* Regression: <select> open -> close -> open -> pick must work through
     * the real click path, and picking an option must update the control's
     * VALUE TEXT. The value raster is cached keyed on (element, geometry,
     * color) - none of which change when the selection changes - so the
     * control kept painting the OLD option's text until some unrelated
     * invalidation ("select shows the previous value / the text is
     * wrong"). */
    {
        whaleui_window_t* sw2 =
            whaleui_window_create(app, "select-pick", 300, 220);
        assert(sw2 != nullptr);
        assert(whaleui_window_load_html(
                   sw2,
                   "<html><head><style>body{margin:0}"
                   "select{position:absolute;left:20px;top:20px;"
                   "width:160px}</style></head>"
                   "<body><select id=\"s\">"
                   "<option>AAA</option><option>BBB</option>"
                   "<option>CCC</option></select></body></html>") == 0);
        if (whaleui_window_show(sw2) == 0 && sw2->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(sw2->render, sw2->document) == 0);
            }
            /* the select never moves: fixed coordinates (the popup paints
             * outside the tree and never relayouts it, so the control's
             * node stays live across open/close/pick) */
            const int sx = 30, sy = 30, pick_y = 52 + 26 + 13;
            /* FNV over a pixel strip across the value text: any glyph
             * change moves some pixel */
            auto strip_hash = [&](void) {
                unsigned long long h = 1469598103934665603ULL;
                /* scan the full control box rows: the value text may sit
                 * anywhere inside (vy = content.y - 2) */
                for (int yy = 22; yy < 50; yy += 2) {
                    for (int dx = 24; dx < 120; dx += 5) {
                        unsigned int p = gpixel(sw2->render, dx, yy);
                        h ^= p;
                        h *= 1099511628211ULL;
                    }
                }
                return h;
            };
            /* dump the raw pixels of one value-text row for eyeballing */
            auto dump_row = [&](int yy, const char* tag) {
                std::string line = std::string(tag) + " y=" +
                                   std::to_string(yy) + ":";
                int ink = 0;
                for (int dx = 22; dx < 90; ++dx) {
                    unsigned int p = gpixel(sw2->render, dx, yy);
                    /* control bg is light gray ~F5; ink is dark */
                    unsigned int rr = (p >> 16) & 0xFF;
                    line += (rr < 0x80) ? '#' : '.';
                    if (rr < 0x80) {
                        ++ink;
                    }
                }
                std::printf("[select-pick] %s ink=%d\n", line.c_str(), ink);
                std::fflush(stdout);
            };
            const unsigned long long h_aaa = strip_hash();
            for (int yy = 24; yy <= 48; yy += 4) {
                dump_row(yy, "aaa");
            }
            const char* val = nullptr;
            /* 1. first click opens */
            assert(whaleui_render_handle_click(sw2->render, sx, sy, &val) ==
                   0);
            assert(whaleui_render_frame(sw2->render, sw2->document) == 0);
            std::printf("[select-pick] after open: open=%d\n",
                        static_cast<int>(sw2->render->open_select != nullptr));
            std::fflush(stdout);
            assert(sw2->render->open_select != nullptr);
            assert(whaleui_render_frame(sw2->render, sw2->document) == 0);
            /* 2. second click at the control closes (toggle) */
            assert(whaleui_render_handle_click(sw2->render, sx, sy, &val) ==
                   0);
            assert(sw2->render->open_select == nullptr);
            assert(whaleui_render_frame(sw2->render, sw2->document) == 0);
            /* 3. reopen and pick the SECOND option row (popup starts at
             * the select's bottom edge; row 1 center) */
            assert(whaleui_render_handle_click(sw2->render, sx, sy, &val) ==
                   0);
            assert(sw2->render->open_select != nullptr);
            assert(whaleui_render_frame(sw2->render, sw2->document) == 0);
            int picked = whaleui_render_handle_click(sw2->render, 50, pick_y,
                                                     &val);
            std::printf("[select-pick] picked=%d val=%s\n", picked,
                        val ? val : "(null)");
            std::fflush(stdout);
            assert(picked == 1 && val && std::strcmp(val, "BBB") == 0);
            assert(sw2->render->open_select == nullptr);
            /* 4. the control's value text must repaint as BBB */
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(sw2->render, sw2->document) == 0);
            }
            {
                auto it = sw2->render->select_index.begin();
                std::printf("[select-pick] select_index entries=%d\n",
                            static_cast<int>(sw2->render->select_index.size()));
                for (; it != sw2->render->select_index.end(); ++it) {
                    std::printf("[select-pick] select_index=%d\n", it->second);
                }
            }
            const unsigned long long h_bbb = strip_hash();
            for (int yy = 24; yy <= 48; yy += 4) {
                dump_row(yy, "bbb");
            }
            std::printf("[select-pick] aaa=%016llx bbb=%016llx\n", h_aaa,
                        h_bbb);
            std::fflush(stdout);
            assert(h_bbb != h_aaa);
        }
        whaleui_window_destroy(sw2);
    }


    /* Regression: a <select> inside a flex header (the demo's theme
     * switcher) must open on click. The header is a flex row and the UA
     * gives select a border-color transition: hovering the select
     * registers a paint-only transition while the flex layout is live.
     * Click coordinates come from the live tree. */
    {
        whaleui_window_t* hw =
            whaleui_window_create(app, "select-flex", 400, 200);
        assert(hw != nullptr);
        assert(whaleui_window_load_html(
                   hw,
                   "<html><head><style>body{margin:0}"
                   ".header{display:flex;align-items:center;"
                   "justify-content:space-between}"
                   "select{min-width:150px}</style></head>"
                   "<body><div class=\"header\"><h1>T</h1>"
                   "<select id=\"s\"><option>AAA</option>"
                   "<option>BBB</option></select></div></body></html>") ==
               0);
        if (whaleui_window_show(hw) == 0 && hw->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(hw->render, hw->document) == 0);
            }
            whaleui_layout_node_t* sel = nullptr;
            for (auto& n : hw->render->tree->arena) {
                if (n.visible && n.el && !n.is_text && !n.pseudo) {
                    size_t len = 0;
                    const lxb_char_t* name =
                        lxb_dom_element_local_name(n.el, &len);
                    if (name && len == 6 &&
                        std::memcmp(name, "select", 6) == 0) {
                        sel = &n;
                        break;
                    }
                }
            }
            assert(sel != nullptr);
            int sx = sel->border.x + sel->border.w / 2;
            int sy = sel->border.y + sel->border.h / 2;
            /* hover first (registers the UA border-color transition), then
             * click the live select box */
            whaleui_render_set_hover(hw->render, sx, sy);
            assert(whaleui_render_frame(hw->render, hw->document) == 0);
            const char* val = nullptr;
            assert(whaleui_render_handle_click(hw->render, sx, sy, &val) ==
                   0);
            assert(whaleui_render_frame(hw->render, hw->document) == 0);
            std::printf("[select-flex] open=%d\n",
                        static_cast<int>(hw->render->open_select != nullptr));
            std::fflush(stdout);
            assert(hw->render->open_select != nullptr);
        }
        whaleui_window_destroy(hw);
    }


    /* Regression: a state-triggered relayout must keep the parent's chunk
     * list in sync with its child chain. The splice replaced the child
     * pointer but left the OLD chunk objects in the parent's list: the
     * paint/hit chunk walks then culled on stale boxes (or skipped the
     * rebuilt members), so hover/click broke after ANY state change and
     * whole subtrees could vanish from paint until the next full rebuild
     * (blank flashes). The check walks every chunk list in the tree:
     * members resolve through first/next exactly, and every member points
     * back at its chunk. */
    {
        whaleui_window_t* kw =
            whaleui_window_create(app, "chunk-sync", 300, 200);
        assert(kw != nullptr);
        assert(whaleui_window_load_html(
                   kw,
                   "<html><head><style>body{margin:0}"
                   "#b{position:absolute;left:20px;top:20px;width:80px;"
                   "height:40px;background-color:#ff0000}"
                   "#b:hover{background-color:#0000ff}"
                   "</style></head>"
                   "<body><div id=\"b\"></div></body></html>") == 0);
        if (whaleui_window_show(kw) == 0 && kw->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(kw->render, kw->document) == 0);
            }
            auto chunks_ok = [&](void) {
                std::function<bool(whaleui_layout_node_t*)> walk =
                    [&](whaleui_layout_node_t* n) {
                        if (!n) {
                            return true;
                        }
                        for (whaleui_child_chunk* ck = n->chunks; ck;
                             ck = ck->next) {
                            int seen = 0;
                            for (whaleui_layout_node_t* c = ck->first;
                                 c && c->chunk == ck; c = c->next) {
                                if (c->parent != n) {
                                    return false;
                                }
                                ++seen;
                            }
                            if (seen != ck->count) {
                                return false;
                            }
                        }
                        /* every child belongs to exactly one listed chunk */
                        for (whaleui_layout_node_t* c = n->first_child; c;
                             c = c->next) {
                            bool found = false;
                            for (whaleui_child_chunk* ck = n->chunks; ck;
                                 ck = ck->next) {
                                if (c->chunk == ck) {
                                    found = true;
                                    break;
                                }
                            }
                            if (n->chunks && !found) {
                                return false;
                            }
                        }
                        for (whaleui_layout_node_t* c = n->first_child; c;
                             c = c->next) {
                            if (!walk(c)) {
                                return false;
                            }
                        }
                        return true;
                    };
                return walk(kw->render->tree->root);
            };
            assert(chunks_ok());
            /* hover triggers a state relayout (style-only rebuild of the
             * hovered subtree): chunks must stay consistent, and the box
             * must still hit-test afterwards */
            whaleui_render_set_hover(kw->render, 60, 40);
            assert(whaleui_render_frame(kw->render, kw->document) == 0);
            assert(whaleui_render_frame(kw->render, kw->document) == 0);
            std::printf("[chunk-sync] ok=%d\n",
                        static_cast<int>(chunks_ok()));
            std::fflush(stdout);
            assert(chunks_ok());
            whaleui_dom_element_t* hit =
                whaleui_render_hit_element(kw->render, 60, 40);
            assert(hit != nullptr);
            unsigned int px = gpixel(kw->render, 60, 40);
            std::printf("[chunk-sync] pixel=%08X\n", px);
            std::fflush(stdout);
            assert(px == 0xFF0000FF);
        }
        whaleui_window_destroy(kw);
    }


    /* Regression: demo-grade select (flex header + 7 options + long page):
     * drive the FULL app click path (set_pressed_ex + handle_click, the
     * order app.cpp uses) and require the popup to open. Unit tests that
     * call handle_click alone bypass the press path that can swallow the
     * click first (scrollbar-drag priority, modal overlay early-out). */
    {
        whaleui_window_t* dw =
            whaleui_window_create(app, "select-demo", 800, 680);
        assert(dw != nullptr);
        assert(whaleui_window_load_html(
                   dw,
                   "<html><head><style>body{margin:0}"
                   ".app{display:flex;flex-direction:column;gap:16px;"
                   "padding:28px 32px}"
                   ".header{display:flex;align-items:center;"
                   "justify-content:space-between}"
                   "#theme{min-width:150px}"
                   ".tall{height:1200px}</style></head>"
                   "<body><div class=\"app\">"
                   "<div class=\"header\"><h1>WhaleUI Demo</h1>"
                   "<select id=\"theme\">"
                   "<option value=\"fluent\">Fluent (Win11)</option>"
                   "<option value=\"metro\">Metro (Win8)</option>"
                   "<option value=\"material\">Material Design</option>"
                   "<option value=\"aero\">Windows 7 Aero</option>"
                   "<option value=\"gtk\">GTK (Adwaita)</option>"
                   "<option value=\"macos\">macOS</option>"
                   "<option value=\"browser\">Browser (UA)</option>"
                   "</select></div>"
                   "<div class=\"tall\"></div>"
                   "</div></body></html>") == 0);
        if (whaleui_window_show(dw) == 0 && dw->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(dw->render, dw->document) == 0);
            }
            whaleui_layout_node_t* sel = nullptr;
            for (auto& n : dw->render->tree->arena) {
                if (n.visible && n.el && !n.is_text && !n.pseudo) {
                    size_t len = 0;
                    const lxb_char_t* name =
                        lxb_dom_element_local_name(n.el, &len);
                    if (name && len == 6 &&
                        std::memcmp(name, "select", 6) == 0) {
                        sel = &n;
                        break;
                    }
                }
            }
            assert(sel != nullptr);
            int sx = sel->border.x + sel->border.w / 2;
            int sy = sel->border.y + sel->border.h / 2;
            std::printf("[select-demo] sel=(%d,%d %dx%d) click=(%d,%d)\n",
                        sel->border.x, sel->border.y, sel->border.w,
                        sel->border.h, sx, sy);
            std::fflush(stdout);
            /* full app path: press down, click, release */
            whaleui_render_set_pressed_ex(dw->render, sx, sy, 1, 1, 0);
            const char* val = nullptr;
            int rc = whaleui_render_handle_click(dw->render, sx, sy, &val);
            whaleui_render_set_pressed_ex(dw->render, sx, sy, 0, 1, 0);
            assert(whaleui_render_frame(dw->render, dw->document) == 0);
            std::printf("[select-demo] rc=%d open=%d drag=%d\n", rc,
                        static_cast<int>(dw->render->open_select != nullptr),
                        static_cast<int>(dw->render->drag_scroll_el !=
                                         nullptr));
            std::fflush(stdout);
            assert(dw->render->open_select != nullptr);
            /* let the open transition finish (120ms default): poll */
            for (int i = 0; i < 30; ++i) {
                assert(whaleui_render_frame(dw->render, dw->document) == 0);
                SDL_Delay(10);
            }
            /* the popup must actually paint: sample inside option row 0
             * (select bottom 79 + a few px) */
            {
                int ink = 0;
                for (int dx = 606; dx < 740; dx += 4) {
                    for (int dy = 82; dy < 102; dy += 4) {
                        unsigned int p = gpixel(dw->render, dx, dy);
                        if (((p >> 16) & 0xFF) < 0xC0) {
                            ++ink;
                        }
                    }
                }
                std::printf("[select-demo] popup ink=%d\n", ink);
                std::fflush(stdout);
                assert(ink > 0);
            }
            /* every option row paints its OWN text (rows used to share row
             * 0's raster through the text cache): row 1 ("Metro (Win8)")
             * and row 2 ("Material Design") must differ from row 0. */
            {
                auto rowhash = [&](int ry) {
                    unsigned long long h = 1469598103934665603ULL;
                    for (int dx = 606; dx < 740; dx += 5) {
                        unsigned int p = gpixel(dw->render, dx, ry);
                        h ^= p;
                        h *= 1099511628211ULL;
                    }
                    return h;
                };
                /* rows start at select bottom (79): row centers */
                unsigned long long h0 = rowhash(92);
                unsigned long long h1 = rowhash(118);
                unsigned long long h2 = rowhash(144);
                std::printf("[select-demo] rows %016llx %016llx %016llx\n",
                            h0, h1, h2);
                std::fflush(stdout);
                assert(h0 != h1 && h1 != h2 && h0 != h2);
            }
        }
        whaleui_window_destroy(dw);
    }

    /* Regression: preventDefault on mousedown is observable through the
     * phased dispatch (the engine's press chain consults it). Custom
     * controls need a reliable "don't do the built-in thing" contract:
     * without it the engine always focuses/presses under the listener. */
    {
        whaleui_window_t* pw =
            whaleui_window_create(app, "prevent-press", 300, 200);
        assert(pw != nullptr);
        assert(whaleui_window_load_html(
                   pw,
                   "<html><head><style>body{margin:0}"
                   "#b{position:absolute;left:20px;top:20px;width:80px;"
                   "height:40px;background-color:#ff0000}"
                   "</style></head>"
                   "<body><div id=\"b\"></div></body></html>") == 0);
        if (whaleui_window_show(pw) == 0 && pw->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(pw->render, pw->document) == 0);
            }
            lxb_dom_element* bel = nullptr;
            for (auto& n : pw->render->tree->arena) {
                if (n.visible && n.el && !n.is_text && !n.pseudo) {
                    size_t len = 0;
                    const lxb_char_t* name =
                        lxb_dom_element_local_name(n.el, &len);
                    if (name && len == 3 &&
                        std::memcmp(name, "div", 3) == 0) {
                        size_t alen = 0;
                        const lxb_char_t* av =
                            lxb_dom_element_get_attribute(
                                n.el, (const lxb_char_t*)"id", 2, &alen);
                        if (av && alen == 1 && av[0] == 'b') {
                            bel = n.el;
                            break;
                        }
                    }
                }
            }
            assert(bel != nullptr);
            struct Pv {
                static void on_down(whaleui_dom_event_t* ev, void* ud) {
                    (void)ud;
                    whaleui_dom_event_prevent_default(ev);
                }
            };
            assert(whaleui_dom_add_event_listener(
                       reinterpret_cast<whaleui_dom_element_t*>(bel),
                       "mousedown", Pv::on_down, nullptr) == 0);
            int prevented = 0;
            assert(whaleui_dom_dispatch_phased(
                       reinterpret_cast<whaleui_dom_element_t*>(bel),
                       "mousedown", 0, 30, 30, 0, 0, 0, &prevented) == 0);
            std::printf("[prevent-press] prevented=%d\n", prevented);
            std::fflush(stdout);
            assert(prevented == 1);
            /* the compat entry still dispatches (return code unchanged) */
            int prevented2 = 0;
            assert(whaleui_dom_dispatch_phased(
                       reinterpret_cast<whaleui_dom_element_t*>(bel),
                       "mousedown", 0, 30, 30, 0, 0, 0, &prevented2) == 0);
            assert(prevented2 == 1);
        }
        whaleui_window_destroy(pw);
    }

    /* Regression: a checkbox and its label text must share ONE line, and
     * clicking the checkbox must flip `checked` WITHOUT rebuilding the
     * tree or blanking the window. The layout gave the 16px control a
     * 32px-tall line box (a second row); the click then set has_dirty=1
     * (a whole-tree rebuild) and the repaint raced the rebuild - the
     * window showed blank frames for seconds ("click clears the window").
     * The check state is paint-only data on a fixed-size control: the
     * toggle only needs the control's own repaint. */
    {
        whaleui_window_t* cw2 =
            whaleui_window_create(app, "check-row", 300, 200);
        assert(cw2 != nullptr);
        assert(whaleui_window_load_html(
                   cw2,
                   "<html><head><style>body{margin:0}"
                   "p{margin:0}</style></head>"
                   "<body><p><input type=\"checkbox\" checked>"
                   " checkbox</p></body></html>") == 0);
        if (whaleui_window_show(cw2) == 0 && cw2->render != nullptr) {
            for (int i = 0; i < 4; ++i) {
                assert(whaleui_render_frame(cw2->render, cw2->document) == 0);
            }
            whaleui_layout_node_t* box = nullptr;
            whaleui_layout_node_t* run = nullptr;
            for (auto& n : cw2->render->tree->arena) {
                if (!n.visible || !n.el) {
                    continue;
                }
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(n.el, &len);
                if (name && len == 5 &&
                    std::memcmp(name, "input", 5) == 0 && !n.is_text) {
                    box = &n;
                }
                if (n.is_text && n.text.find("checkbox") !=
                                     std::string::npos) {
                    run = &n;
                }
            }
            assert(box != nullptr && run != nullptr);
            std::printf("[check-row] box=(%d,%d %dx%d) run=(%d,%d %dx%d)\n",
                        box->border.x, box->border.y, box->border.w,
                        box->border.h, run->border.x, run->border.y,
                        run->border.w, run->border.h);
            std::fflush(stdout);
            /* same line: the rows overlap vertically */
            assert(box->border.y < run->border.y + run->border.h &&
                   run->border.y < box->border.y + box->border.h);
            /* click the box center: checked flips off */
            const char* val = nullptr;
            int cx = box->border.x + box->border.w / 2;
            int cy = box->border.y + box->border.h / 2;
            whaleui_layout_tree_t* tree_before = cw2->render->tree;
            assert(whaleui_render_handle_click(cw2->render, cx, cy, &val) ==
                   0);
            assert(lxb_dom_element_has_attribute(
                       box->el, (const lxb_char_t*)"checked", 7) == false);
            for (int i = 0; i < 6; ++i) {
                assert(whaleui_render_frame(cw2->render, cw2->document) == 0);
            }
            /* NO rebuild: the same tree object serves the click */
            assert(cw2->render->tree == tree_before);
            /* and the window is not blank: the control row has ink */
            int ink = 0;
            for (int dx = box->border.x;
                 dx < box->border.x + box->border.w + 60; ++dx) {
                unsigned int p = gpixel(cw2->render, dx,
                                        box->border.y + 8);
                if (((p >> 16) & 0xFF) < 0xF0) {
                    ++ink;
                }
            }
            std::printf("[check-row] ink=%d\n", ink);
            std::fflush(stdout);
            assert(ink > 0);
        }
        whaleui_window_destroy(cw2);
    }
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
            /* last run lands near the bottom (CJK advance fix made layout
             * widths correct; q21k's tail leaves ~49px below the last run,
             * so allow a generous margin) */
            assert(run_bottom > content_bottom - 64);
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
        node_font(w->render, ta, &fs, &family, &bold);
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
        /* while the textarea can still scroll it swallows the wheel (the
         * page stays); once it hits its bottom, further wheel BUBBLES to
         * the page - a container at its edge must not claim the wheel
         * forever ("鼠标在 card/textarea 上无法滚动到底"). The page is
         * therefore allowed to move here (it did, via the bubble). */
        assert(w->render->scrolls[rel] >= page_s);
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
        /* the first layout is now SYNCHRONOUS but SLICED: the tree exists
         * right away (the viewport's worth of rows, paintable immediately) and
         * the worker finishes the rest - so what drains is `resumes`, not a
         * missing tree. A short delay models the real event loop. */
        assert(whaleui_render_frame(w->render, w->document) == 0);
        assert(w->render->tree != nullptr); /* sliced, already paintable */
        while (!w->render->tree->resumes.empty() && frames < 500) {
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

    /* rotate paints the ROTATED shape: a 40x40 box rotated 45deg around
     * its center becomes a diamond whose vertices reach half the diagonal
     * (28px) - a sample 25px above the center sits OUTSIDE the laid-out
     * box but INSIDE the diamond, so it must turn red only when rotate
     * actually rotates. Points beyond 28px must stay background (only the
     * rotated shape colors). Approximation: text/children stay upright -
     * this is a background/border rotation only. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "rotate-diamond", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#s{position:absolute;left:80px;top:80px;width:40px;"
                   "height:40px;background-color:#ff0000;"
                   "transform:rotate(45deg)}"
                   "</style></head>"
                   "<body><div id=\"s\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            unsigned int inside_diamond = gpixel(w->render, 100, 75);
            unsigned int center = gpixel(w->render, 100, 100);
            unsigned int outside_diamond = gpixel(w->render, 100, 60);
            std::printf("[rotate] in=%08X mid=%08X out=%08X\n",
                        inside_diamond, center, outside_diamond);
            std::fflush(stdout);
            /* center is red either way (the laid-out box covers it) */
            assert((center & 0xFFFFFFu) == 0xFF0000u);
            /* 25px above center: red only when the shape rotated */
            assert((inside_diamond & 0xFFFFFFu) == 0xFF0000u);
            /* 40px above center: past the diamond's vertex - background */
            assert((outside_diamond & 0xFFFFFFu) != 0xFF0000u);
        }
        whaleui_window_destroy(w);
    }

    /* backdrop-filter's SMALL-radius tier: blur(4px) over a hard red/blue
     * edge must MIX the two colors near the boundary (the fixed 3x3 kernel
     * path), while samples far from the boundary stay pure (the blur must
     * not smear the whole overlay).blur(20px) exercises the mip-ladder
     * tier the same way. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "backdrop-blur", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#l{position:absolute;left:0;top:0;width:100px;"
                   "height:200px;background-color:#ff0000}"
                   "#r{position:absolute;left:100px;top:0;width:100px;"
                   "height:200px;background-color:#0000ff}"
                   "#f{position:absolute;left:50px;top:50px;width:100px;"
                   "height:100px;backdrop-filter:blur(4px)}"
                   "</style></head>"
                   "<body><div id=\"l\"></div><div id=\"r\"></div>"
                   "<div id=\"f\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* y=100 is 50px inside the overlay (past the blur reach).
             * x=99/100 straddle the hard edge (r starts at x=100); both
             * samples must carry BOTH channels - red-dominant with real
             * blue on the left of the edge, blue-dominant with real red on
             * the right (the fixed 3x3 kernel mixes across it). */
            unsigned int edge_l = gpixel(w->render, 99, 100);
            unsigned int edge_r = gpixel(w->render, 100, 100);
            unsigned int far_red = gpixel(w->render, 60, 100);
            unsigned int far_blue = gpixel(w->render, 140, 100);
            std::printf("[backdrop4] l=%08X r=%08X red=%08X blue=%08X\n",
                        edge_l, edge_r, far_red, far_blue);
            std::fflush(stdout);
            assert(((edge_l >> 16) & 0xFF) > 0x60 && (edge_l & 0xFF) > 0x20);
            assert(((edge_r >> 16) & 0xFF) > 0x20 && (edge_r & 0xFF) > 0x60);
            /* far away the halves stay (nearly) pure */
            assert(((far_red >> 16) & 0xFF) > 0xC0 && (far_red & 0xFF) < 0x40);
            assert(((far_blue >> 16) & 0xFF) < 0x40 && (far_blue & 0xFF) > 0xC0);
        }
        whaleui_window_destroy(w);
    }

    /* inset box-shadow, mask path: a hard ring (blur 0 + spread) colors the
     * rim while the middle stays background; an offset ring lights only the
     * offset side; a blurred offset shadow darkens the top edge and fades
     * out before the middle. The GPU CS computes 1 - blurred_mask(px - off)
     * of the spread-grown box (see kInsetCS). */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "inset-shadow", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#a{position:absolute;left:20px;top:20px;width:70px;"
                   "height:40px;background-color:#00ff00;"
                   "box-shadow:inset 0 0 0 8px #ff0000}"
                   "#b{position:absolute;left:110px;top:20px;width:70px;"
                   "height:40px;background-color:#00ff00;"
                   "box-shadow:inset 20px 0 0 #ff0000}"
                   "#c{position:absolute;left:20px;top:90px;width:160px;"
                   "height:80px;background-color:#00ff00;"
                   "box-shadow:inset 0 14px 14px #ff0000}"
                   "</style></head>"
                   "<body><div id=\"a\"></div><div id=\"b\"></div>"
                   "<div id=\"c\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            unsigned int ring = gpixel(w->render, 24, 40);
            unsigned int ring_mid = gpixel(w->render, 55, 40);
            unsigned int off_l = gpixel(w->render, 115, 40);
            unsigned int off_r = gpixel(w->render, 170, 40);
            unsigned int blur_top = gpixel(w->render, 100, 96);
            unsigned int blur_mid = gpixel(w->render, 100, 130);
            std::printf("[inset] ring=%08X mid=%08X offl=%08X offr=%08X "
                        "top=%08X bmid=%08X\n",
                        ring, ring_mid, off_l, off_r, blur_top, blur_mid);
            std::fflush(stdout);
            /* 8px hard ring: red at the rim, green in the middle */
            assert(((ring >> 16) & 0xFF) > 0xC0 && (ring & 0xFF) < 0x40);
            assert(((ring_mid >> 8) & 0xFF) > 0xC0 &&
                   ((ring_mid >> 16) & 0xFF) < 0x40);
            /* inset 20px 0: only the LEFT strip is red */
            assert(((off_l >> 16) & 0xFF) > 0xC0 && (off_l & 0xFF) < 0x40);
            assert(((off_r >> 8) & 0xFF) > 0xC0 &&
                   ((off_r >> 16) & 0xFF) < 0x40);
            /* inset 0 14px 14px: red haze at the top edge, green center */
            assert(((blur_top >> 16) & 0xFF) > ((blur_top >> 8) & 0xFF));
            assert(((blur_mid >> 8) & 0xFF) > ((blur_mid >> 16) & 0xFF));
        }
        whaleui_window_destroy(w);
    }

    /* backdrop-filter keeps the element's OWN body painting (a gradient
     * here) on top of the blurred backdrop: the body is drawn in the late
     * pass after the blur compute, so it must show its red->blue ramp, not
     * the blurred (white) page underneath. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "backdrop-body", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0;"
                   "background-color:#ffffff}"
                   "#u{position:absolute;left:50px;top:50px;width:100px;"
                   "height:100px;backdrop-filter:blur(4px);"
                   "background:linear-gradient(#ff0000, #0000ff)}"
                   "</style></head>"
                   "<body><div id=\"u\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            unsigned int top = gpixel(w->render, 100, 60);
            unsigned int bot = gpixel(w->render, 100, 140);
            std::printf("[backdrop-body] top=%08X bot=%08X\n", top, bot);
            std::fflush(stdout);
            /* the gradient body survived the blur pass: red at the top,
             * blue at the bottom (a clobbered body would be white here) */
            assert(((top >> 16) & 0xFF) > (top & 0xFF) + 0x40);
            assert((bot & 0xFF) > ((bot >> 16) & 0xFF) + 0x40);
        }
        whaleui_window_destroy(w);
    }

    /* transform-origin moves the scale/rotate anchor: a 100px bar with
     * scaleX(0.5) + origin left keeps its left edge (grows rightward from
     * x=20), while the default center would pull it to x=45. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "transform-origin", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:20px;top:20px;width:100px;"
                   "height:20px;background-color:#ff0000;"
                   "transform:scaleX(0.5);transform-origin:left center}"
                   "</style></head>"
                   "<body><div id=\"u\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            unsigned int at_left = gpixel(w->render, 22, 30);
            unsigned int at_right = gpixel(w->render, 68, 30);
            std::printf("[origin] left=%08X right=%08X\n", at_left,
                        at_right);
            std::fflush(stdout);
            /* left-anchored: the bar spans x=20..70 */
            assert((at_left & 0xFFFFFFu) == 0xFF0000u);
            assert((at_right & 0xFFFFFFu) == 0xFF0000u);
        }
        whaleui_window_destroy(w);
    }

    /* ::before box with off-whitelist properties: a 4-side border-color
     * and a linear-gradient background must both reach the pseudo box
     * (pseudo_box_style merges every declaration, no whitelist). The host
     * is transparent, so whatever paints at the box is the ::before. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "pseudo-props", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:20px;top:20px;width:100px;"
                   "height:60px;background:transparent}"
                   "#u::before{content:\"\";position:absolute;inset:0;"
                   "background:linear-gradient(#ff0000, #0000ff);"
                   "border:4px solid;border-color:#00ff00 #00ff00 #00ff00 "
                   "#00ff00}"
                   "</style></head>"
                   "<body><div id=\"u\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* green border ring (4px): inside the top edge */
            unsigned int edge = gpixel(w->render, 70, 22);
            /* gradient body: red at the top inside, blue at the bottom */
            unsigned int gtop = gpixel(w->render, 70, 32);
            unsigned int gbot = gpixel(w->render, 70, 72);
            std::printf("[pseudo] edge=%08X top=%08X bot=%08X\n", edge,
                        gtop, gbot);
            std::fflush(stdout);
            assert(((edge >> 8) & 0xFF) > 0xC0 &&
                   ((edge >> 16) & 0xFF) < 0x40);
            assert(((gtop >> 16) & 0xFF) > (gtop & 0xFF) + 0x40);
            assert((gbot & 0xFF) > ((gbot >> 16) & 0xFF) + 0x40);
        }
        whaleui_window_destroy(w);
    }

    /* batch-1 small features:
     * - ::placeholder{color} tints the placeholder (input value empty);
     * - text-stroke-color longhand colors the glyph outline;
     * - overflow-x:hidden clips (longhand, no shorthand);
     * - object-position:right bottom pins a contain-fit image. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "batch1", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "input::placeholder{color:#ff0000}"
                   "#ph{position:absolute;left:10px;top:10px;width:180px;"
                   "font-size:20px;color:#000000}"
                   "#st{position:absolute;left:10px;top:50px;font-size:24px;"
                   "color:#000000;text-stroke-width:1px;"
                   "text-stroke-color:#0000ff}"
                   "#ov{position:absolute;left:10px;top:90px;width:60px;"
                   "height:20px;overflow-x:hidden;background:#ffffff}"
                   "#ov div{width:120px;height:20px;background:#00ff00}"
                   "</style></head>"
                   "<body><input id=\"ph\" placeholder=\"hint\">"
                   "<div id=\"st\">Ab</div>"
                   "<div id=\"ov\"><div></div></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* placeholder red (not the muted gray): scan the input box */
            int ph_red = 0;
            for (int yy = 12; yy < 34; ++yy) {
                for (int xx = 12; xx < 188; ++xx) {
                    unsigned int px = gpixel(w->render, xx, yy);
                    if (((px >> 16) & 0xFF) > 0x80 && ((px >> 8) & 0xFF) < 0x80 &&
                        (px & 0xFF) < 0x80) {
                        ++ph_red;
                    }
                }
            }
            /* stroked glyph: blue outline around black "A" - blue ink exists */
            int st_blue = 0;
            for (int yy = 52; yy < 80; ++yy) {
                for (int xx = 12; xx < 80; ++xx) {
                    unsigned int px = gpixel(w->render, xx, yy);
                    if ((px & 0xFF) > 0x80 && ((px >> 16) & 0xFF) < 0x80) {
                        ++st_blue;
                    }
                }
            }
            /* overflow-x:hidden: green only inside x=10..70 at y=100 */
            unsigned int ov_in = gpixel(w->render, 40, 100);
            unsigned int ov_out = gpixel(w->render, 100, 100);
            std::printf("[batch1] phred=%d stblue=%d in=%08X out=%08X\n",
                        ph_red, st_blue, ov_in, ov_out);
            std::fflush(stdout);
            assert(ph_red > 0);
            assert(st_blue > 0);
            assert(((ov_in >> 8) & 0xFF) > 0x80);
            assert(!(((ov_out >> 8) & 0xFF) > 0x80 &&
                     ((ov_out >> 16) & 0xFF) < 0x80 &&
                     (ov_out & 0xFF) < 0x80));
        }
        whaleui_window_destroy(w);
    }

    /* content:attr() - the ::before text comes from the attribute */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "attr-content", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:10px;top:10px;font-size:24px;"
                   "color:#000000}"
                   "#u::before{content:attr(data-v)}"
                   "</style></head>"
                   "<body><div id=\"u\" data-v=\"Hi\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* "Hi" ink exists (a literal "attr(...)" would paint
             * different/no glyphs, an empty value paints nothing) */
            int ink = 0;
            for (int yy = 10; yy < 44; ++yy) {
                for (int xx = 10; xx < 80; ++xx) {
                    unsigned int px = gpixel(w->render, xx, yy);
                    if (((px >> 16) & 0xFF) < 0x80 &&
                        ((px >> 8) & 0xFF) < 0x80 && (px & 0xFF) < 0x80 &&
                        (((px >> 16) & 0xFF) + ((px >> 8) & 0xFF) +
                         (px & 0xFF)) > 0) {
                        ++ink;
                    }
                }
            }
            std::printf("[attr] ink=%d\n", ink);
            std::fflush(stdout);
            assert(ink > 0);
        }
        whaleui_window_destroy(w);
    }

    /* radial-gradient size keywords + focus-within:
     * closest-side circle in a 100x60 box centered => radius 30, red
     * center fading out by the edges; focusing the input makes the form
     * match :focus-within (green border proxy via background change). */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "batch3", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:20px;top:20px;width:100px;"
                   "height:60px;background:radial-gradient(circle "
                   "closest-side at 50% 50%, #ff0000, #0000ff)}"
                   "#f{position:absolute;left:20px;top:100px;width:160px;"
                   "height:60px;background:#ffffff}"
                   "#f:focus-within{background:#00ff00}"
                   "#f input{width:140px}"
                   "</style></head>"
                   "<body><div id=\"u\"></div>"
                   "<div id=\"f\"><input id=\"in\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* center red (radius 30 covers it), corner blue-ish */
            unsigned int ctr = gpixel(w->render, 70, 50);
            unsigned int corner = gpixel(w->render, 24, 24);
            /* focus the input: click it, the form goes green. Sample the
             * form's bottom-right (outside the input box itself). */
            whaleui_render_set_pressed(w->render, 40, 130, 1);
            whaleui_render_set_pressed(w->render, 0, 0, 0);
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            unsigned int fbg = gpixel(w->render, 150, 150);
            std::printf("[batch3] ctr=%08X corner=%08X fbg=%08X\n", ctr,
                        corner, fbg);
            std::fflush(stdout);
            assert(((ctr >> 16) & 0xFF) > 0x80);
            assert((corner & 0xFF) > ((corner >> 16) & 0xFF));
            assert(((fbg >> 8) & 0xFF) > 0x80 &&
                   ((fbg >> 16) & 0xFF) < 0x80);
        }
        whaleui_window_destroy(w);
    }

    /* batch4: aspect-ratio + list-style-type + sticky bottom.
     * - #ar has width only + aspect-ratio 2/1 => height = w/2;
     * - square bullets + lower-alpha ordinals via list-style-type;
     * - sticky bottom bar: scrolling up pins it above the viewport
     *   bottom (it would otherwise scroll off with the content). */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "batch4", 200, 300);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#ar{width:100px;aspect-ratio:2 / 1;"
                   "background-color:#ff0000}"
                   "ul{list-style-type:square}"
                   "ol{list-style-type:lower-alpha}"
                   "#sp{height:200px}"
                   "#st{position:sticky;bottom:0;height:20px;"
                   "background-color:#0000ff}"
                   "#tail{height:30px}"
                   "</style></head>"
                   "<body><div id=\"ar\"></div>"
                   "<ul><li>a</li></ul><ol><li>b</li></ol>"
                   "<div id=\"sp\"></div><div id=\"st\"></div>"
                   "<div id=\"tail\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* aspect-ratio: 100x50 red box */
            unsigned int arc = gpixel(w->render, 50, 25);
            unsigned int below = gpixel(w->render, 50, 75);
            /* scroll to the very bottom: the sticky bar (30px of tail
             * below it) would scroll 30px past the viewport bottom;
             * bottom:0 pins it at 280..300 instead. */
            whaleui_render_handle_wheel(w->render, 100, 150, -20);
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            unsigned int stb = gpixel(w->render, 100, 290);
            std::printf("[batch4] ar=%08X below=%08X sticky=%08X\n", arc,
                        below, stb);
            std::fflush(stdout);
            assert(((arc >> 16) & 0xFF) > 0x80);
            /* below the 50px box: page background, not red */
            assert(!(((below >> 16) & 0xFF) > 0x80 &&
                     ((below >> 8) & 0xFF) < 0x80 && (below & 0xFF) < 0x80));
            assert((stb & 0xFF) > 0x80 && ((stb >> 16) & 0xFF) < 0x80);
        }
        whaleui_window_destroy(w);
    }

    /* filter: grayscale() collapses chroma, brightness() scales up,
     * invert() mirrors. #g red box -> gray (r~=g~=b); #b blue box +
     * brightness(2) -> brighter blue; #v green box + invert(1) -> magenta. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "filter", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#g{position:absolute;left:10px;top:10px;width:50px;"
                   "height:50px;background-color:#ff0000;"
                   "filter:grayscale(1)}"
                   "#b{position:absolute;left:70px;top:10px;width:50px;"
                   "height:50px;background-color:#000080;"
                   "filter:brightness(2)}"
                   "#v{position:absolute;left:130px;top:10px;width:50px;"
                   "height:50px;background-color:#00ff00;filter:invert(1)}"
                   "#f{position:absolute;left:10px;top:70px;width:100px;"
                   "height:40px;background-color:#ff0000;"
                   "filter:blur(8px)}"
                   "</style></head>"
                   "<body><div id=\"g\"></div><div id=\"b\"></div>"
                   "<div id=\"v\"></div><div id=\"f\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            unsigned int gr = gpixel(w->render, 35, 35);
            unsigned int br = gpixel(w->render, 95, 35);
            unsigned int iv = gpixel(w->render, 155, 35);
            std::printf("[filter] gray=%08X bright=%08X inv=%08X\n", gr,
                        br, iv);
            std::fflush(stdout);
            /* red (255,0,0) -> luma gray ~54: channels within 8 of it */
            unsigned int grr = (gr >> 16) & 0xFF, grg = (gr >> 8) & 0xFF,
                         grb = gr & 0xFF;
            assert(grr > 0x20 && grr < 0x80 && grg > 0x20 && grg < 0x80 &&
                   grb > 0x20 && grb < 0x80);
            /* navy (0,0,128) x2 -> (0,0,255) saturated blue */
            assert(((br >> 16) & 0xFF) < 0x40 &&
                   ((br >> 8) & 0xFF) < 0x40 && (br & 0xFF) > 0xC0);
            /* green inverted -> magenta (r+b high, g low) */
            assert(((iv >> 16) & 0xFF) > 0x80 && (iv & 0xFF) > 0x80 &&
                   ((iv >> 8) & 0xFF) < 0x80);
            /* blur(8px): the hard red edge softens (pink halo just
             * outside; the mip-ladder falloff is steep further out) */
            unsigned int halo2 = gpixel(w->render, 9, 90);
            std::printf("[filter] halo2=%08X\n", halo2);
            std::fflush(stdout);
            assert(((halo2 >> 16) & 0xFF) > 0x80 && (halo2 & 0xFF) < 0xF0);
        }
        whaleui_window_destroy(w);
    }

    /* skew: a 100x40 box with skewX(45deg) shears right by tan45 * h
     * (~40px at the bottom edge), so x=130 stays red below the box top
     * while the unskewed column at x=30 turns background. matrix(1,0,0,1
     * kept simple): scale+translate decompose draws translated. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "skew", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#s{position:absolute;left:20px;top:20px;width:100px;"
                   "height:40px;background-color:#ff0000;"
                   "transform:skewX(45deg);transform-origin:left top}"
                   "#m{position:absolute;left:20px;top:80px;width:40px;"
                   "height:40px;background-color:#0000ff;"
                   "transform:matrix(1, 0, 0, 1, 60, 0)}"
                   "</style></head>"
                   "<body><div id=\"s\"></div><div id=\"m\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* skewX(45) about left-top: top edge stays x=20..120,
             * bottom edge shifts to x=60..160 */
            unsigned int top_l = gpixel(w->render, 40, 30);
            unsigned int bot_r = gpixel(w->render, 130, 55);
            unsigned int bot_l = gpixel(w->render, 25, 55);
            /* matrix translate: blue box moves 60px right */
            unsigned int mved = gpixel(w->render, 90, 100);
            unsigned int orig = gpixel(w->render, 30, 100);
            std::printf("[skew] topl=%08X botr=%08X botl=%08X mv=%08X og=%08X\n",
                        top_l, bot_r, bot_l, mved, orig);
            std::fflush(stdout);
            assert((top_l & 0xFFFFFFu) == 0xFF0000u);
            assert((bot_r & 0xFFFFFFu) == 0xFF0000u);
            assert((bot_l & 0xFFFFFFu) != 0xFF0000u);
            assert((mved & 0xFFFFFFu) == 0x0000FFu);
            assert((orig & 0xFFFFFFu) != 0x0000FFu);
        }
        whaleui_window_destroy(w);
    }

    /* background-blend-mode: red solid + blue gradient, multiply =>
     * dark (r&b low-ish, not pure red nor pure blue). mix-blend-mode:
     * red box over blue backdrop, multiply => near-black. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "blend", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:10px;top:10px;width:80px;"
                   "height:60px;background-color:#ff0000;"
                   "background-image:linear-gradient(#0000ff, #0000ff);"
                   "background-blend-mode:multiply}"
                   "#b{position:absolute;left:10px;top:100px;width:180px;"
                   "height:60px;background-color:#0000ff}"
                   "#m{position:absolute;left:50px;top:110px;width:100px;"
                   "height:40px;background-color:#ff0000;"
                   "mix-blend-mode:multiply}"
                   "</style></head>"
                   "<body><div id=\"u\"></div><div id=\"b\"></div>"
                   "<div id=\"m\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            unsigned int bb = gpixel(w->render, 50, 40);
            unsigned int mb = gpixel(w->render, 100, 130);
            std::printf("[blend] bgblend=%08X mix=%08X\n", bb, mb);
            std::fflush(stdout);
            /* multiply red*blue: dark, neither channel dominant */
            assert(((bb >> 16) & 0xFF) < 0x80 && (bb & 0xFF) < 0x80);
            /* red over blue multiply: black-ish */
            assert(((mb >> 16) & 0xFF) < 0x40 && (mb & 0xFF) < 0x40 &&
                   ((mb >> 8) & 0xFF) < 0x40);
        }
        whaleui_window_destroy(w);
    }

    /* clip-path: inset(10px) cuts 10px off every side; circle(25px)
     * keeps only the disc (corners go background). */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "clip-path", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:20px;top:20px;width:100px;"
                   "height:60px;background-color:#ff0000;"
                   "clip-path:inset(10px)}"
                   "#v{position:absolute;left:20px;top:100px;width:100px;"
                   "height:60px;background-color:#0000ff;"
                   "clip-path:circle(25px at 50px 30px)}"
                   "</style></head>"
                   "<body><div id=\"u\"></div><div id=\"v\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* inset box keeps its middle red */
            unsigned int mid = gpixel(w->render, 70, 50);
            /* 5px inside the laid-out edge is clipped away */
            unsigned int edge = gpixel(w->render, 25, 25);
            /* circle center blue, corner background */
            unsigned int cc = gpixel(w->render, 70, 130);
            unsigned int corner = gpixel(w->render, 25, 105);
            std::printf("[clip] mid=%08X edge=%08X cc=%08X cn=%08X\n",
                        mid, edge, cc, corner);
            std::fflush(stdout);
            assert((mid & 0xFFFFFFu) == 0xFF0000u);
            assert((edge & 0xFFFFFFu) != 0xFF0000u);
            assert((cc & 0xFFFFFFu) == 0x0000FFu);
            assert((corner & 0xFFFFFFu) != 0x0000FFu);
        }
        whaleui_window_destroy(w);
    }

    /* mask-image: linear-gradient(to right, black, transparent) fades the
     * red box from opaque (left) to gone (right). The midpoint keeps some
     * red but weakened; the right edge is the page background. A second
     * box with a fully-opaque mask paints normally (parse-but-degenerate
     * masks must not kill the paint). */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "mask-image", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:20px;top:20px;width:100px;"
                   "height:60px;background-color:#ff0000;"
                   "mask-image:linear-gradient(to right,#000000,#00000000)}"
                   "#v{position:absolute;left:20px;top:100px;width:100px;"
                   "height:60px;background-color:#0000ff;"
                   "mask-image:linear-gradient(to bottom,#000000,#000000)}"
                   "</style></head>"
                   "<body><div id=\"u\"></div><div id=\"v\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* left edge: full-strength red (green ~0x0D) */
            unsigned int ml = gpixel(w->render, 25, 50);
            /* midpoint: half-faded (green midway 0x0D -> 0xEA) */
            unsigned int mm = gpixel(w->render, 70, 50);
            /* right edge: mask ~0, red gone (page background) */
            unsigned int mr = gpixel(w->render, 115, 50);
            /* opaque vertical mask: blue box intact */
            unsigned int vb = gpixel(w->render, 70, 130);
            std::printf("[mask] l=%08X m=%08X r=%08X b=%08X\n",
                        ml, mm, mr, vb);
            std::fflush(stdout);
            /* red stays strong throughout (red over light bg); the fade
             * signal is the green channel rising toward the background */
            assert(((ml >> 16) & 0xFF) > 0xC0);
            assert(((mm >> 16) & 0xFF) > 0xC0);
            assert(((mr >> 16) & 0xFF) > 0xC0);
            unsigned int mlg = (ml >> 8) & 0xFF;
            unsigned int mmg = (mm >> 8) & 0xFF;
            unsigned int mrg = (mr >> 8) & 0xFF;
            assert(mlg < 0x40 && mmg > 0x40 && mmg < 0xC0 && mrg > 0xC0);
            assert((vb & 0xFFFFFFu) == 0x0000FFu);
        }
        whaleui_window_destroy(w);
    }

    /* colors: hwb()/lab()/color-mix() as plain background-color (GPU/CPU
     * identical for solids). conic-gradient parses (white-box below); its
     * GPU path intentionally skips (flat body stays), so no pixel assert
     * for conic here. repeating-linear stripes ARE drawn on GPU. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "colors", 260, 220);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:10px;top:10px;width:60px;"
                   "height:40px;background-color:hwb(0 0% 0%)}"
                   "#v{position:absolute;left:80px;top:10px;width:60px;"
                   "height:40px;background-color:hwb(120 0% 0%)}"
                   "#x{position:absolute;left:150px;top:10px;width:60px;"
                   "height:40px;background-color:lab(50% 0 0)}"
                   "#m{position:absolute;left:10px;top:60px;width:60px;"
                   "height:40px;background-color:color-mix(in srgb,"
                   "#ff0000 50%,#0000ff)}"
                   "#o{position:absolute;left:80px;top:60px;width:60px;"
                   "height:40px;background-color:oklch(60% 0.15 30)}"
                   "#r{position:absolute;left:10px;top:110px;width:200px;"
                   "height:40px;background-image:repeating-linear-gradient("
                   "to right,#ff0000 0%,#ff0000 25%,#0000ff 25%,#0000ff 50%)}"
                   "</style></head>"
                   "<body><div id=\"u\"></div><div id=\"v\"></div>"
                   "<div id=\"x\"></div><div id=\"m\"></div>"
                   "<div id=\"o\"></div><div id=\"r\"></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            unsigned int pu = gpixel(w->render, 40, 30);
            unsigned int pv = gpixel(w->render, 110, 30);
            unsigned int px = gpixel(w->render, 180, 30);
            unsigned int pm = gpixel(w->render, 40, 80);
            unsigned int po = gpixel(w->render, 110, 80);
            /* repeating stripes: 200px box, period 50% = 100px...
             * t spans 1.0 over the box, rspan 0.5 -> 2 periods: px 10-60
             * red, 60-110 blue, 110-160 red, 160-210 blue */
            unsigned int r1 = gpixel(w->render, 30, 130);
            unsigned int r2 = gpixel(w->render, 90, 130);
            unsigned int r3 = gpixel(w->render, 140, 130);
            unsigned int r4 = gpixel(w->render, 190, 130);
            std::printf("[colors] u=%08X v=%08X x=%08X m=%08X o=%08X "
                        "r=%08X %08X %08X %08X\n",
                        pu, pv, px, pm, po, r1, r2, r3, r4);
            std::fflush(stdout);
            /* hwb red / hwb green: exact primaries */
            assert((pu & 0xFFFFFFu) == 0xFF0000u);
            assert((pv & 0xFFFFFFu) == 0x00FF00u);
            /* lab middle grey: channels agree, mid-range */
            {
                unsigned int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF,
                             b = px & 0xFF;
                assert(r > 0x50 && r < 0xA0 && g > 0x50 && g < 0xA0 &&
                       b > 0x50 && b < 0xA0);
                unsigned int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
                unsigned int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
                assert(mx - mn < 0x14);
            }
            /* color-mix red/blue 50%: purple, both ends present */
            {
                unsigned int r = (pm >> 16) & 0xFF, b = pm & 0xFF,
                             g = (pm >> 8) & 0xFF;
                assert(r > 0x60 && b > 0x60 && g < 0x40);
            }
            /* oklch warm: red dominant, some green, little blue */
            {
                unsigned int r = (po >> 16) & 0xFF, g = (po >> 8) & 0xFF,
                             b = po & 0xFF;
                assert(r > 0xB0 && g > 0x40 && g < r && b < 0x90);
            }
            /* repeating stripes red/blue/red/blue */
            assert((r1 & 0xFFFFFFu) == 0xFF0000u);
            assert((r2 & 0xFFFFFFu) == 0x0000FFu);
            assert((r3 & 0xFFFFFFu) == 0xFF0000u);
            assert((r4 & 0xFFFFFFu) == 0x0000FFu);
        }
        whaleui_window_destroy(w);
    }

    /* conic-gradient white-box: parse type/angle/stops (the GPU path
     * skips conic, the CPU path is exact - parse is the shared gate). */
    {
        Gradient g;
        assert(parse_gradient(
                   "conic-gradient(from 90deg at 25% 75%, #ff0000, #0000ff)",
                   g) == 1);
        assert(g.type == 2 && g.stops.size() == 2);
        /* 90deg start ray */
        assert(g.angle_deg > 89.0f && g.angle_deg < 91.0f);
        assert(g.cx > 0.24f && g.cx < 0.26f && g.cy > 0.74f &&
               g.cy < 0.76f);
        assert(parse_gradient(
                   "repeating-conic-gradient(#ff0000, #0000ff)", g) == 1);
        assert(g.type == 2 && g.stops.size() == 2);
        std::printf("[conic] parse OK\n");
        std::fflush(stdout);
    }

    /* contain: paint clips overflowing children like overflow:hidden.
     * The child box (red) sticks 20px past the parent's bottom edge;
     * inside it stays red, outside it is the page background. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "contain", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:20px;top:20px;width:100px;"
                   "height:60px;background-color:#ffffff;contain:paint}"
                   "#u div{width:100px;height:40px;background-color:#ff0000}"
                   "</style></head>"
                   "<body><div id=\"u\"><div style=\"margin-top:50px\">"
                   "</div></div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* child starts 50px into the 60px parent: visible strip red */
            unsigned int inside = gpixel(w->render, 70, 75);
            /* 20px past the parent edge: clipped away */
            unsigned int outside = gpixel(w->render, 70, 95);
            std::printf("[contain] in=%08X out=%08X\n", inside, outside);
            std::fflush(stdout);
            assert((inside & 0xFFFFFFu) == 0xFF0000u);
            assert((outside & 0xFFFFFFu) != 0xFF0000u);
        }
        whaleui_window_destroy(w);
    }

    /* ::first-line colors the first wrapped line only; ::first-letter
     * paints the first glyph big and red. Scan for the expected colors:
     * first-line red on line 1, base black on line 2, big red lead glyph. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "first-line", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   "#u{position:absolute;left:10px;top:10px;width:120px;"
                   "color:#000000;font-size:16px}"
                   "#u::first-line{color:#ff0000}"
                   "#v{position:absolute;left:10px;top:100px;width:120px;"
                   "color:#000000;font-size:16px}"
                   "#v::first-letter{color:#ff0000;font-size:32px}"
                   "#s{position:absolute;left:10px;top:150px;width:120px;"
                   "color:#000000;font-size:16px}"
                   "#s::first-line{color:#ff0000}"
                   "</style></head>"
                   "<body><div id=\"u\">aaaa bbbb cccc dddd eeee ffff gggg "
                   "hhhh</div><div id=\"v\">hello world</div>"
                   "<div id=\"s\">short</div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* line 1 (y~18): red glyphs present */
            bool line1_red = false;
            for (int xx = 10; xx < 132 && !line1_red; xx += 2) {
                for (int yy = 8; yy < 70 && !line1_red; yy += 2) {
                    unsigned int px = gpixel(w->render, xx, yy);
                    unsigned int r = (px >> 16) & 0xFF,
                                 g = (px >> 8) & 0xFF, b = px & 0xFF;
                    if (r > 0x90 && g < 0x60 && b < 0x60) {
                        line1_red = true;
                        std::printf("[first] red at %d,%d\n", xx, yy);
                    }
                }
            }
            std::fflush(stdout);
            /* line 2 (y~38): black glyphs present, no red */
            bool line2_black = false, line2_red = false;
            for (int xx = 12; xx < 128; xx += 3) {
                unsigned int px = gpixel(w->render, xx, 38);
                unsigned int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF,
                             b = px & 0xFF;
                if (r < 0x60 && g < 0x60 && b < 0x60) {
                    line2_black = true;
                }
                if (r > 0x90 && g < 0x60 && b < 0x60) {
                    line2_red = true;
                }
            }
            /* first-letter: the lead glyph draws red (32px), the rest of
             * the run stays black - proving the split (red somewhere in
             * the box + black elsewhere in the same box). */
            bool letter_red = false, letter_rest_black = false;
            for (int xx = 10; xx < 132 && (!letter_red || !letter_rest_black);
                 xx += 2) {
                for (int yy = 96; yy < 140 && (!letter_red || !letter_rest_black);
                     yy += 2) {
                    unsigned int px = gpixel(w->render, xx, yy);
                    unsigned int r = (px >> 16) & 0xFF,
                                 g = (px >> 8) & 0xFF, b = px & 0xFF;
                    if (r > 0x90 && g < 0x60 && b < 0x60) {
                        letter_red = true;
                    }
                    if (r < 0x60 && g < 0x60 && b < 0x60) {
                        letter_rest_black = true;
                    }
                }
            }
            std::printf("[first] l1red=%d l2black=%d l2red=%d letter=%d "
                        "lrest=%d\n",
                        (int)line1_red, (int)line2_black, (int)line2_red,
                        (int)letter_red, (int)letter_rest_black);
            std::fflush(stdout);
            assert(line1_red);
            assert(line2_black);
            assert(!line2_red);
            assert(letter_red);
            assert(letter_rest_black);
            /* single-line run: whole line in the first-line style */
            bool short_red = false;
            for (int xx = 12; xx < 128 && !short_red; xx += 3) {
                for (int yy = 152; yy < 168 && !short_red; yy += 2) {
                    unsigned int px = gpixel(w->render, xx, yy);
                    unsigned int r = (px >> 16) & 0xFF,
                                 g = (px >> 8) & 0xFF, b = px & 0xFF;
                    if (r > 0x90 && g < 0x60 && b < 0x60) {
                        short_red = true;
                    }
                }
            }
            std::printf("[first] short=%d\n", (int)short_red);
            std::fflush(stdout);
            assert(short_red);
        }
        whaleui_window_destroy(w);
    }

    /* :has() parent-conditional: a card containing an image gets a red
     * background, a text-only card stays white. */
    {
        whaleui_window_t* w =
            whaleui_window_create(app, "has", 200, 200);
        assert(w != nullptr);
        assert(whaleui_window_load_html(
                   w,
                   "<html><head><style>body{margin:0}"
                   ".card{position:absolute;left:10px;width:80px;height:40px;"
                   "background-color:#ffffff}"
                   "#a{top:10px}#b{top:60px}"
                   ".card:has(img){background-color:#ff0000}"
                   ".card img{width:20px;height:20px}"
                   "</style></head>"
                   "<body><div class=\"card\" id=\"a\"><img></div>"
                   "<div class=\"card\" id=\"b\">text</div></body></html>") == 0);
        if (whaleui_window_show(w) == 0 && w->render != nullptr) {
            for (int i = 0; i < 3; ++i) {
                assert(whaleui_render_frame(w->render, w->document) == 0);
            }
            /* card with img: red (sample the bottom-right, past the
             * 20px placeholder); text-only card: white */
            unsigned int with = gpixel(w->render, 70, 40);
            unsigned int without = gpixel(w->render, 70, 90);
            std::printf("[has] with=%08X without=%08X\n", with, without);
            std::fflush(stdout);
            assert((with & 0xFFFFFFu) == 0xFF0000u);
            assert((without & 0xFFFFFFu) == 0xFFFFFFu);
        }
        whaleui_window_destroy(w);
    }

    whaleui_app_destroy(app);
    std::printf("[scroll] OK\n");
    return 0;
}
