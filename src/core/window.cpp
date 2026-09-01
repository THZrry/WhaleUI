/* Window core: public C API implementation.
 * The native SDL_Window is created lazily on the first show(); CSS from the
 * document (<style>/<link>) plus the built-in default stylesheet is parsed
 * and handed to the render context. */

#include "core/window.h"
#include "core/app.h"
#include "dom/dom.h"
#include "fs/fs.h"
#include "render/render.h"
#include "style/style.h"
#include "style/theme.h"

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace {
char* dup_str(const char* s)
{
    size_t n = std::strlen(s) + 1;
    char* d = static_cast<char*>(std::malloc(n));
    if (d) {
        std::memcpy(d, s, n);
    }
    return d;
}

/* collect <style> text + <link rel=stylesheet> from the document head */
void collect_doc_css(whaleui_dom_document_t* doc, std::string& out)
{
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    if (!hd || !hd->head) {
        return;
    }
    lxb_dom_node* n = hd->head->element.element.node.first_child;
    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element* el = lxb_dom_interface_element(n);
            size_t len = 0;
            const lxb_char_t* name = lxb_dom_element_local_name(el, &len);
            if (name && len == 5 && std::memcmp(name, "style", 5) == 0) {
                size_t tlen = 0;
                const lxb_char_t* txt = lxb_dom_node_text_content(&el->node, &tlen);
                if (txt) {
                    out.append(reinterpret_cast<const char*>(txt), tlen);
                    out += "\n";
                }
            } else if (name && len == 4 && std::memcmp(name, "link", 4) == 0) {
                size_t alen = 0;
                const lxb_char_t* rel = lxb_dom_element_get_attribute(el, (const lxb_char_t*)"rel", 3, &alen);
                if (rel && alen >= 10 && std::memcmp(rel, "stylesheet", 10) == 0) {
                    size_t hlen = 0;
                    const lxb_char_t* href = lxb_dom_element_get_attribute(el, (const lxb_char_t*)"href", 4, &hlen);
                    if (href) {
                        std::string uri(reinterpret_cast<const char*>(href), hlen);
                        char* data = nullptr;
                        size_t dlen = 0;
                        if (whaleui_fs_load(uri.c_str(), &data, &dlen) == 0) {
                            out.append(data, dlen);
                            out += "\n";
                            std::free(data);
                        }
                    }
                }
            }
        }
        n = n->next;
    }
}

void window_reload_css(whaleui_window_t* win)
{
    if (!win || !win->render) {
        return;
    }
    std::string css = whaleui_theme_default_css(win->app->theme_style);
    if (win->document) {
        collect_doc_css(win->document, css);
    }
    whaleui_css_rule_t* rules = nullptr;
    size_t count = 0;
    whaleui_css_keyframes_t kf = {nullptr, 0};
    if (whaleui_css_parse_full(css.c_str(), css.size(), &rules, &count, &kf) != 0) {
        return;
    }
    /* drop rules whose @media condition does not match the current window
     * (theme, viewport width, reduced-motion preference) */
    whaleui_style_filter_media(rules, &count,
                               whaleui_app_resolved_theme(win->app),
                               win->width, win->app->reduced_motion);
    std::map<std::string, std::string> theme_vars;
    whaleui_theme_vars(win->app->theme_style,
                       whaleui_app_resolved_theme(win->app),
                       win->app->accent, theme_vars);
    whaleui_render_set_css(win->render, rules, count, &kf, &theme_vars);
    whaleui_css_rules_destroy(rules, count);
    whaleui_css_keyframes_destroy(&kf);
}

/* no JS engine: a reveal-on-scroll page keeps its content hidden (opacity:0
 * until JS adds .in). Simulate the JS by adding .in to every element whose
 * class list contains "reveal", so content shows while decorative
 * @keyframes animations still play. (app_create defaults to FULL motion, so
 * prefers-reduced-motion doesn't stop those animations; reduced_motion=1
 * would surface a reveal page but stop the animations.) */
void window_reveal_apply(whaleui_dom_document_t* doc)
{
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    if (!hd) {
        return;
    }
    lxb_dom_element* root = lxb_dom_document_element(&hd->dom_document);
    if (!root) {
        return;
    }
    const lxb_char_t* cls = (const lxb_char_t*)"class";
    const lxb_char_t* in_attr = (const lxb_char_t*)"in";
    std::function<void(lxb_dom_node*)> walk = [&](lxb_dom_node* n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element* el = lxb_dom_interface_element(n);
            size_t clen = 0;
            const lxb_char_t* c =
                lxb_dom_element_get_attribute(el, cls, 5, &clen);
            if (c && clen) {
                std::string cv(reinterpret_cast<const char*>(c), clen);
                /* simulate the page JS: the IntersectionObserver adds
                 * .in to .reveal AND .lines elements (a .lines mask
                 * whose i never leaves translateY(112%) slides the h1
                 * out of its overflow:hidden span - "the title text is
                 * clipped"). */
                if ((cv.find("reveal") != std::string::npos ||
                     cv.find("lines") != std::string::npos) &&
                    cv.find(" in") == std::string::npos) {
                    std::string nc = cv + " in";
                    lxb_dom_element_set_attribute(
                        el, cls, 5,
                        reinterpret_cast<const lxb_char_t*>(nc.c_str()),
                        nc.size());
                }
            }
        }
        for (lxb_dom_node* ch = n->first_child; ch; ch = ch->next) {
            walk(ch);
        }
    };
    walk(&root->node);
}

/* public internal: refresh after theme/accent change */
} // namespace

extern "C" void whaleui_window_refresh_css(whaleui_window_t* win)
{
    window_reload_css(win);
}

extern "C" whaleui_window_t* whaleui_window_create(whaleui_app_t* app,
                                                   const char* title, int width, int height)
{
    if (!app || !title || width <= 0 || height <= 0) {
        return nullptr;
    }
    whaleui_window_t* win = new whaleui_window_t;
    win->app = app;
    win->sdl = nullptr;
    win->render = nullptr;
    win->title = dup_str(title);
    win->width = width;
    win->height = height;
    win->visible = 0;
    win->document = nullptr;
    win->resize_pending = 0;
    win->resize_w = width;
    win->resize_h = height;
    win->resize_last = 0;
    app->windows.push_back(win);
    return win;
}

extern "C" void whaleui_window_destroy(whaleui_window_t* win)
{
    if (!win) {
        return;
    }
    /* detach from the app's window list */
    for (size_t i = 0; i < win->app->windows.size(); ++i) {
        if (win->app->windows[i] == win) {
            win->app->windows.erase(win->app->windows.begin() + static_cast<long>(i));
            break;
        }
    }
    whaleui_window_close(win);
    if (win->document) {
        whaleui_dom_document_destroy(win->document);
    }
    std::free(win->title);
    delete win;
}

extern "C" int whaleui_window_show(whaleui_window_t* win)
{
    if (!win) {
        return -1;
    }
    if (!win->sdl) {
        win->sdl = SDL_CreateWindow(win->title, win->width, win->height,
                                    SDL_WINDOW_RESIZABLE);
        if (!win->sdl) {
            return -2;
        }
        if (!win->app->gpu) {
            /* Backend choice: WHALEUI_GPU=d3d12|vulkan selects explicitly
             * (Vulkan preferred on machines with a driver; D3D12 is the
             * default - it works everywhere, incl. Basic Display Adapter
             * via WARP). The shader formats declared cover both. */
            const char* want = SDL_getenv("WHALEUI_GPU");
            if (want && *want) {
                SDL_SetHint(SDL_HINT_GPU_DRIVER, want);
            }
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetBooleanProperty(props,
                SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXBC_BOOLEAN, true);
            SDL_SetBooleanProperty(props,
                SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
            SDL_SetBooleanProperty(props,
                SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
            /* prefer the integrated (low-power) adapter: on laptops that is
             * the iGPU, saving battery. Default ON - desktop users with a
             * discrete card can disable with WHALEUI_GPU_LOWPOWER=0 (or
             * override per-app in Windows Settings > Display > Graphics). */
            const char* lp = SDL_getenv("WHALEUI_GPU_LOWPOWER");
            if (!lp || (lp[0] != '0')) {
                SDL_SetBooleanProperty(props,
                    SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, true);
            }
            win->app->gpu = SDL_CreateGPUDeviceWithProperties(props);
            SDL_DestroyProperties(props);
            if (!win->app->gpu) {
                /* fallback: let SDL pick any backend */
                win->app->gpu = SDL_CreateGPUDevice(
                    SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_DXIL |
                        SDL_GPU_SHADERFORMAT_SPIRV,
                    false, nullptr);
            }
            if (!win->app->gpu) {
                SDL_DestroyWindow(win->sdl);
                win->sdl = nullptr;
                return -3;
            }
        }
        if (!SDL_ClaimWindowForGPUDevice(win->app->gpu, win->sdl)) {
            SDL_DestroyWindow(win->sdl);
            win->sdl = nullptr;
            return -4;
        }
        /* honor the app's vsync preference: SDL3's default is VSYNC, but
         * setting it explicitly (and re-applying on change) keeps the
         * worker from rendering faster than the display can present (an
         * uncapped animation ran at 147fps, ~90% CPU on one core). */
        if (win->app->vsync) {
            SDL_SetGPUSwapchainParameters(
                win->app->gpu, win->sdl, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                SDL_GPU_PRESENTMODE_VSYNC);
        }
        /* display refresh rate: the worker paces frames to it (the D3D12
         * backend's VSYNC present mode does not actually wait for vblank) */
        {
            SDL_DisplayID did = SDL_GetDisplayForWindow(win->sdl);
            const SDL_DisplayMode* dm =
                did ? SDL_GetCurrentDisplayMode(did) : nullptr;
            if (dm && dm->refresh_rate > 0) {
                win->app->display_refresh =
                    static_cast<int>(dm->refresh_rate);
            }
        }
        win->render = whaleui_render_create(win->app->gpu, win->sdl,
                                            win->width, win->height);
        if (!win->render) {
            return -5;
        }
        win->render->async_layout = win->app->async_layout;
        window_reload_css(win);
    }
    /* pre-render the first frame before the window becomes visible, so it
     * appears with content already painted instead of a blank flash while
     * the first layout + text rasterization run */
    if (win->render && win->document) {
        whaleui_render_frame(win->render, win->document);
    }
    SDL_ShowWindow(win->sdl);
    win->visible = 1;
    return 0;
}

extern "C" int whaleui_window_hide(whaleui_window_t* win)
{
    if (!win) {
        return -1;
    }
    if (win->sdl) {
        SDL_HideWindow(win->sdl);
    }
    win->visible = 0;
    return 0;
}

extern "C" int whaleui_window_close(whaleui_window_t* win)
{
    if (!win) {
        return -1;
    }
    if (win->render) {
        whaleui_render_destroy(win->render);
        win->render = nullptr;
    }
    if (win->sdl) {
        if (win->app->gpu) {
            SDL_ReleaseWindowFromGPUDevice(win->app->gpu, win->sdl);
        }
        SDL_DestroyWindow(win->sdl);
        win->sdl = nullptr;
    }
    win->visible = 0;
    return 0;
}

extern "C" int whaleui_window_set_title(whaleui_window_t* win, const char* title)
{
    if (!win || !title) {
        return -1;
    }
    char* t = dup_str(title);
    if (!t) {
        return -2;
    }
    std::free(win->title);
    win->title = t;
    if (win->sdl) {
        SDL_SetWindowTitle(win->sdl, title);
    }
    return 0;
}

extern "C" const char* whaleui_window_get_title(const whaleui_window_t* win)
{
    return win ? win->title : nullptr;
}

extern "C" int whaleui_window_set_size(whaleui_window_t* win, int width, int height)
{
    if (!win || width <= 0 || height <= 0) {
        return -1;
    }
    win->width = width;
    win->height = height;
    if (win->sdl) {
        SDL_SetWindowSize(win->sdl, width, height);
    }
    if (win->render) {
        whaleui_render_resize(win->render, width, height);
        /* viewport changed: re-filter @media rules against the new width
         * (the worker's coalesced resize path does the same) */
        whaleui_window_refresh_css(win);
    }
    return 0;
}

extern "C" int whaleui_window_get_size(const whaleui_window_t* win, int* w, int* h)
{
    if (!win || !w || !h) {
        return -1;
    }
    *w = win->width;
    *h = win->height;
    return 0;
}

extern "C" int whaleui_window_load_html(whaleui_window_t* win, const char* html)
{
    if (!win || !html) {
        return -1;
    }
    win->base_uri.clear(); /* in-memory html has no base for relative links */
    if (win->document) {
        whaleui_dom_document_destroy(win->document);
    }
    /* drop caches keyed on the old document's elements (text rasters,
     * images, select/scroll/edit state) before the new DOM takes over */
    if (win->render) {
        whaleui_render_reset_dom(win->render);
    }
    win->document = whaleui_dom_parse_html(html, std::strlen(html));
    if (!win->document) {
        return -2;
    }
    /* no JS engine: add .in to reveal elements so their content shows */
    window_reveal_apply(win->document);
    window_reload_css(win);
    return 0;
}

extern "C" int whaleui_window_load_uri(whaleui_window_t* win, const char* uri)
{
    if (!win || !uri) {
        return -1;
    }
    char* buf = nullptr;
    size_t len = 0;
    if (whaleui_fs_load(uri, &buf, &len) != 0) {
        return -2;
    }
    int rc = whaleui_window_load_html(win, buf);
    std::free(buf);
    /* load_html clears the base (in-memory html); set it AFTER so the
     * loaded document keeps its URI for resolving relative <a href> */
    win->base_uri = uri;
    return rc;
}

extern "C" whaleui_dom_document_t* whaleui_window_get_document(whaleui_window_t* win)
{
    return win ? win->document : nullptr;
}






