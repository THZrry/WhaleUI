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
            /* D3D12 first (works everywhere, incl. Basic Display Adapter via
             * WARP); DXBC from D3DCompile needs the device to declare it. */
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetBooleanProperty(props,
                SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXBC_BOOLEAN, true);
            SDL_SetBooleanProperty(props,
                SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
            SDL_SetBooleanProperty(props,
                SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
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
        win->render = whaleui_render_create(win->app->gpu, win->sdl,
                                            win->width, win->height);
        if (!win->render) {
            return -5;
        }
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
    if (win->document) {
        whaleui_dom_document_destroy(win->document);
    }
    win->document = whaleui_dom_parse_html(html, std::strlen(html));
    if (!win->document) {
        return -2;
    }
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
    return rc;
}

extern "C" whaleui_dom_document_t* whaleui_window_get_document(whaleui_window_t* win)
{
    return win ? win->document : nullptr;
}
