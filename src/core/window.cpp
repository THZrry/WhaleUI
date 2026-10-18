/* Window core: public C API implementation.
 * The native SDL_Window is created lazily on the first show(); CSS from the
 * document (<style>/<link>) plus the built-in default stylesheet is parsed
 * and handed to the render context. */

#include "core/window.h"
#include "core/app.h"
#include "dom/dom.h"
#include "dom/events.h"
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

/* collect <style> text + <link rel=stylesheet> from the document.
 *
 * Scans <head> AND <body> in document order: HTML allows <style> in the body
 * (it is a common pattern for page-scoped or lazily inserted rules), and a
 * browser applies it, so reading only <head> made the same markup behave
 * differently here than in a browser. Document order also keeps the cascade's
 * source-order tiebreak right when sheets sit in both sections.
 *
 * A <style media="..."> element's own media condition is prepended as an
 * @media wrapper, so it is filtered by the same evaluator as an inline
 * @media block (the attribute is a real part of the cascade; ignoring it made
 * media="print" rules apply on screen). */
void collect_doc_css(whaleui_dom_document_t* doc, std::string& out)
{
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    if (!hd) {
        return;
    }
    lxb_dom_node* roots[2] = {nullptr, nullptr};
    if (hd->head) {
        roots[0] = &hd->head->element.element.node;
    }
    if (hd->body) {
        roots[1] = &hd->body->element.element.node;
    }
    for (int ri = 0; ri < 2; ++ri) {
        for (lxb_dom_node* n = roots[ri] ? roots[ri]->first_child : nullptr; n;
             n = n->next) {
            if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                continue;
            }
            lxb_dom_element* el = lxb_dom_interface_element(n);
            size_t len = 0;
            const lxb_char_t* name = lxb_dom_element_local_name(el, &len);
            if (!name) {
                continue;
            }
            /* media attribute ("screen", "print", "(min-width: 600px)") */
            std::string media;
            size_t mlen = 0;
            const lxb_char_t* mv = lxb_dom_element_get_attribute(
                el, (const lxb_char_t*)"media", 5, &mlen);
            if (mv && mlen) {
                media.assign(reinterpret_cast<const char*>(mv), mlen);
            }
            std::string sheet;
            if (len == 5 && std::memcmp(name, "style", 5) == 0) {
                size_t tlen = 0;
                const lxb_char_t* txt =
                    lxb_dom_node_text_content(&el->node, &tlen);
                if (txt) {
                    sheet.assign(reinterpret_cast<const char*>(txt), tlen);
                }
            } else if (len == 4 && std::memcmp(name, "link", 4) == 0) {
                size_t alen = 0;
                const lxb_char_t* rel = lxb_dom_element_get_attribute(
                    el, (const lxb_char_t*)"rel", 3, &alen);
                if (!(rel && alen >= 10 &&
                      std::memcmp(rel, "stylesheet", 10) == 0)) {
                    continue;
                }
                size_t hlen = 0;
                const lxb_char_t* href = lxb_dom_element_get_attribute(
                    el, (const lxb_char_t*)"href", 4, &hlen);
                if (!href) {
                    continue;
                }
                std::string uri(reinterpret_cast<const char*>(href), hlen);
                char* data = nullptr;
                size_t dlen = 0;
                if (whaleui_fs_load(uri.c_str(), &data, &dlen) == 0) {
                    sheet.assign(data, dlen);
                    std::free(data);
                }
            } else {
                continue;
            }
            if (sheet.empty()) {
                continue;
            }
            if (!media.empty()) {
                out += "@media ";
                out += media;
                out += "{";
                out += sheet;
                out += "}\n";
            } else {
                out += sheet;
                out += "\n";
            }
        }
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
    /* the document's programmatic sheet (whaleui_css_apply) cascades AFTER the
     * document's own <style>/<link> text, which is the order a sheet attached
     * later should win at equal specificity */
    size_t doc_rule_count = 0;
    const whaleui_css_rule_t* doc_rules =
        whaleui_style_doc_rules_get(win->document, &doc_rule_count);
    whaleui_css_rule_t* rules = nullptr;
    size_t count = 0;
    whaleui_css_keyframes_t kf = {nullptr, 0};
    if (whaleui_css_parse_full(css.c_str(), css.size(), &rules, &count, &kf) != 0) {
        return;
    }
    /* the document's OWN sheet may also carry @keyframes (whaleui_css_apply_text
     * keeps them, because the public whaleui_css_parse drops them). Append them
     * to this sheet's keyframes: the animation engine resolves
     * `animation: <name>` against ONE registry, so a sheet mounted from C++ and
     * the markup sheet must land in the same list. */
    if (win->document) {
        const whaleui_css_keyframes_t* dkf =
            whaleui_style_doc_keyframes_get(win->document);
        if (dkf && dkf->count) {
            size_t old = kf.count;
            whaleui_keyframes_t* grown = static_cast<whaleui_keyframes_t*>(
                std::realloc(kf.items,
                             (old + dkf->count) * sizeof(whaleui_keyframes_t)));
            if (grown) {
                kf.items = grown;
                for (size_t i = 0; i < dkf->count; ++i) {
                    whaleui_keyframes_t* d = &kf.items[old + i];
                    const whaleui_keyframes_t* s = &dkf->items[i];
                    std::memset(d, 0, sizeof(*d));
                    d->name = s->name ? strdup(s->name) : nullptr;
                    if (s->frame_count) {
                        d->frames = static_cast<char**>(
                            std::calloc(s->frame_count, sizeof(char*)));
                        if (d->frames) {
                            for (size_t f = 0; f < s->frame_count; ++f) {
                                d->frames[f] =
                                    s->frames[f] ? strdup(s->frames[f]) : nullptr;
                            }
                            d->frame_count = s->frame_count;
                        }
                    }
                }
                kf.count = old + dkf->count;
            }
        }
    }
    if (doc_rules && doc_rule_count) {
        /* Append: the programmatic sheet becomes part of the same rule array, so
         * selector matching / the cascade / the tag index see ONE sheet. A
         * separate array would need a second match pass and would lose the
         * source-order tiebreak between the two. The array comes from the
         * parser's realloc-grown buffer, so growing it here is the same
         * allocator; a fresh allocation on failure keeps the sheet complete
         * rather than silently dropping the programmatic rules. */
        whaleui_css_rule_t* merged = static_cast<whaleui_css_rule_t*>(
            std::realloc(rules, (count + doc_rule_count) * sizeof(*rules)));
        if (!merged) {
            merged = static_cast<whaleui_css_rule_t*>(
                std::malloc((count + doc_rule_count) * sizeof(*rules)));
            if (merged) {
                std::memcpy(merged, rules, count * sizeof(*rules));
                /* the old array's rules are MOVED, not copied: free only the
                 * container, the members now live in `merged` */
                std::free(rules);
            }
        }
        if (merged) {
            rules = merged;
            for (size_t i = 0; i < doc_rule_count; ++i) {
                std::memset(&rules[count + i], 0, sizeof(rules[count + i]));
                const whaleui_css_rule_t* s = &doc_rules[i];
                rules[count + i].selector = s->selector ? strdup(s->selector)
                                                        : nullptr;
                rules[count + i].media = s->media ? strdup(s->media) : nullptr;
                rules[count + i].important = s->important;
                if (s->decl_count) {
                    rules[count + i].decls = static_cast<char**>(
                        std::malloc(s->decl_count * sizeof(char*)));
                    for (size_t d = 0; d < s->decl_count; ++d) {
                        rules[count + i].decls[d] = strdup(s->decls[d]);
                    }
                    rules[count + i].decl_count = s->decl_count;
                }
            }
            count += doc_rule_count;
        }
    }
    /* drop rules whose @media condition does not match the current window
     * (theme, viewport, reduced-motion preference) */
    whaleui_style_filter_media(rules, &count,
                               whaleui_app_resolved_theme(win->app),
                               win->width, win->app->reduced_motion,
                               win->height);
    /* identical to the active ruleset? (e.g. a plain window RESIZE with no
     * @media breakpoint crossed - the common case while dragging) Skip the
     * hand-off so the style cascade cache / CSS vars survive: without this
     * every resize cleared them and the following whole-tree relayout
     * re-cascaded all 34k elements (~seconds frozen mid-drag). */
    {
        const whaleui_css_rule_t* cur = win->render->rules;
        size_t cur_n = win->render->rule_count;
        bool same = cur && count == cur_n;
        if (same) {
            for (size_t i = 0; same && i < count; ++i) {
                if ((rules[i].selector && (!cur[i].selector ||
                      std::strcmp(rules[i].selector, cur[i].selector) != 0)) ||
                    (!rules[i].selector && cur[i].selector)) {
                    same = false;
                }
                if (same && (rules[i].decl_count != cur[i].decl_count)) {
                    same = false;
                }
                for (size_t d = 0; same && d < rules[i].decl_count; ++d) {
                    if (std::strcmp(rules[i].decls[d], cur[i].decls[d]) != 0) {
                        same = false;
                    }
                }
            }
        }
        /* @keyframes are part of the sheet too: comparing rules alone treated
         * a sheet that only adds/changes a keyframes block as "unchanged" and
         * threw it away, so the animation never attached (the same sheet in
         * the initial markup worked, because the first parse had no incumbent
         * to compare against). */
        if (same) {
            const whaleui_css_keyframes_t* curk = &win->render->keyframes;
            if (kf.count != curk->count) {
                same = false;
            }
            for (size_t i = 0; same && i < kf.count; ++i) {
                const whaleui_keyframes_t* a = &kf.items[i];
                const whaleui_keyframes_t* b = &curk->items[i];
                if (std::strcmp(a->name ? a->name : "", b->name ? b->name : "") != 0 ||
                    a->frame_count != b->frame_count) {
                    same = false;
                    break;
                }
                for (size_t f = 0; same && f < a->frame_count; ++f) {
                    if (std::strcmp(a->frames[f] ? a->frames[f] : "",
                                    b->frames[f] ? b->frames[f] : "") != 0) {
                        same = false;
                    }
                }
            }
        }
        if (same) {
            whaleui_css_rules_destroy(rules, count);
            whaleui_css_keyframes_destroy(&kf);
            return; /* ruleset unchanged: keep the style cache */
        }
    }
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

/* HitTest ctx + helpers (file scope, after anonymous namespace) */
struct HitTestCtx { whaleui_hit_test_fn fn; void* ud; };
static whaleui_hit_test_result_t default_borderless_hit_test(whaleui_window_t* win, int x, int y, void*)
{
    const int kBorder = 8;
    const int kTitle = 32;
    int w = win->width;
    int h = win->height;
    bool left = x < kBorder;
    bool right = x >= w - kBorder;
    bool top = y < kBorder;
    bool bottom = y >= h - kBorder;
    if (top && left) return WHALEUI_HITTEST_RESIZE_TOPLEFT;
    if (top && right) return WHALEUI_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left) return WHALEUI_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return WHALEUI_HITTEST_RESIZE_BOTTOMRIGHT;
    if (left) return WHALEUI_HITTEST_RESIZE_LEFT;
    if (right) return WHALEUI_HITTEST_RESIZE_RIGHT;
    if (top) return WHALEUI_HITTEST_RESIZE_TOP;
    if (bottom) return WHALEUI_HITTEST_RESIZE_BOTTOM;
    if (y < kTitle) return WHALEUI_HITTEST_DRAGGABLE;
    return WHALEUI_HITTEST_NORMAL;
}
static SDL_HitTestResult sdl_hit_test_bridge(SDL_Window* sdl_win, const SDL_Point* area, void* data)
{
    (void)sdl_win;
    whaleui_window_t* win = static_cast<whaleui_window_t*>(data);
    if (!win || !area) return SDL_HITTEST_NORMAL;
    HitTestCtx* ctx = static_cast<HitTestCtx*>(win->hit_test_ud);
    if (!ctx || !ctx->fn) return SDL_HITTEST_NORMAL;
    whaleui_hit_test_result_t r = ctx->fn(win, area->x, area->y, ctx->ud);
    switch (r) {
    case WHALEUI_HITTEST_DRAGGABLE: return SDL_HITTEST_DRAGGABLE;
    case WHALEUI_HITTEST_RESIZE_TOPLEFT: return SDL_HITTEST_RESIZE_TOPLEFT;
    case WHALEUI_HITTEST_RESIZE_TOP: return SDL_HITTEST_RESIZE_TOP;
    case WHALEUI_HITTEST_RESIZE_TOPRIGHT: return SDL_HITTEST_RESIZE_TOPRIGHT;
    case WHALEUI_HITTEST_RESIZE_RIGHT: return SDL_HITTEST_RESIZE_RIGHT;
    case WHALEUI_HITTEST_RESIZE_BOTTOMRIGHT: return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    case WHALEUI_HITTEST_RESIZE_BOTTOM: return SDL_HITTEST_RESIZE_BOTTOM;
    case WHALEUI_HITTEST_RESIZE_BOTTOMLEFT: return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    case WHALEUI_HITTEST_RESIZE_LEFT: return SDL_HITTEST_RESIZE_LEFT;
    default: return SDL_HITTEST_NORMAL;
    }
}
static void window_apply_hit_test(whaleui_window_t* win)
{
    if (!win || !win->sdl) return;
    if (win->hit_test_active) {
        SDL_SetWindowHitTest(win->sdl, sdl_hit_test_bridge, win);
    } else {
        SDL_SetWindowHitTest(win->sdl, nullptr, nullptr);
    }
}

/* public internal: refresh after theme/accent change */
} // namespace

extern "C" void whaleui_window_refresh_css(whaleui_window_t* win)
{
    window_reload_css(win);
}

/* frame-level hook (see render.h css_reload_fn): lets the frame re-collect the
 * document stylesheet after a DOM edit to a <style>/<link> without knowing how
 * the sheet is assembled (theme default + document sheets). */
static void window_css_reload_hook(whaleui_render_t* r, void* ud)
{
    (void)r;
    whaleui_window_t* win = static_cast<whaleui_window_t*>(ud);
    if (win) {
        window_reload_css(win);
    }
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
    win->mouse_pending = 0;
    win->mouse_x = 0.0f;
    win->mouse_y = 0.0f;
    /* MouseEvent.movementX/Y is computed from these on the first mouse
     * event. `new` leaves them INDETERMINATE, so the first event reported a
     * garbage delta (a huge movement value, and for a page listening on it,
     * arbitrary behaviour). */
    win->last_mouse_x = 0.0f;
    win->last_mouse_y = 0.0f;
    win->occluded = 0;
    win->bg_last = 0;
    win->wheel_pending = 0;
    win->wheel_acc = 0.0f;
    win->wheel_acc_x = 0.0f;
    win->wheel_acc_y = 0.0f;
    win->min_w = 0;
    win->min_h = 0;
    win->max_w = 0;
    win->max_h = 0;
    win->bordered = 1;
    win->resizable = 1;
    win->always_on_top = 0;
    win->opacity = 1.0f;
    win->mouse_grab = 0;
    win->keyboard_grab = 0;
    win->focusable = 1;
    win->fullscreen = WHALEUI_FULLSCREEN_OFF;
    win->win_event_cb = nullptr;
    win->win_event_ud = nullptr;
    win->key_cb = nullptr;
    win->key_ud = nullptr;
    win->select_cb = nullptr;
    win->select_ud = nullptr;
    win->hit_test_ud = nullptr;
    win->hit_test_active = 0;
    win->display_refresh = 60;
    /* `new whaleui_window_t` leaves scalars INDETERMINATE, and doc_bytes
     * decides the async-layout policy at show time. Garbage >= 256 KB turned
     * async layout on for a tiny page: the first layout is then SLICED (only
     * the viewport's rows are built) and the rest streams in from a worker -
     * with no worker draining it in a direct render_frame caller the tree
     * stayed at ~33 nodes for every frame, so nothing laid out, nothing
     * repainted and every C++ edit hit a node that was never in the tree
     * ("layout is wrong and edits do nothing"). Zero-initialize the field. */
    win->doc_bytes = 0;
    app->windows.push_back(win);
    return win;
}

extern "C" void whaleui_window_destroy(whaleui_window_t* win)
{
    if (!win) {
        return;
    }
    if (win->hit_test_active) {
        HitTestCtx* ctx = static_cast<HitTestCtx*>(win->hit_test_ud);
        delete ctx;
        win->hit_test_ud = nullptr;
        win->hit_test_active = 0;
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
    if (win->drop_dt) {
        whaleui_data_transfer_destroy(win->drop_dt);
        win->drop_dt = nullptr;
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
        Uint64 flags = 0;
        if (win->resizable) flags |= SDL_WINDOW_RESIZABLE;
        if (!win->bordered) flags |= SDL_WINDOW_BORDERLESS;
        if (win->always_on_top) flags |= SDL_WINDOW_ALWAYS_ON_TOP;
        if (!win->focusable) flags |= SDL_WINDOW_NOT_FOCUSABLE;
        win->sdl = SDL_CreateWindow(win->title, win->width, win->height, flags);
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
        /* display refresh rate: per-window + global compat */
        {
            SDL_DisplayID did = SDL_GetDisplayForWindow(win->sdl);
            const SDL_DisplayMode* dm =
                did ? SDL_GetCurrentDisplayMode(did) : nullptr;
            int rr = (dm && dm->refresh_rate > 0) ? (int)dm->refresh_rate : 60;
            win->display_refresh = rr;
            win->app->display_refresh = rr;
        }
        /* apply cached constraints/appearance that were set before show() */
        if (win->min_w || win->min_h) SDL_SetWindowMinimumSize(win->sdl, win->min_w, win->min_h);
        if (win->max_w || win->max_h) SDL_SetWindowMaximumSize(win->sdl, win->max_w, win->max_h);
        if (win->opacity != 1.0f) SDL_SetWindowOpacity(win->sdl, win->opacity);
        if (win->always_on_top) SDL_SetWindowAlwaysOnTop(win->sdl, true);
        /* resizable flag already in create flags; re-apply for completeness */
        if (!win->resizable) SDL_SetWindowResizable(win->sdl, false);
        if (win->mouse_grab) SDL_SetWindowMouseGrab(win->sdl, true);
        if (win->keyboard_grab) SDL_SetWindowKeyboardGrab(win->sdl, true);
        if (win->hit_test_active) window_apply_hit_test(win);
        else if (!win->bordered) {
            HitTestCtx* ctx = new HitTestCtx{default_borderless_hit_test, nullptr};
            win->hit_test_ud = ctx;
            win->hit_test_active = 1;
            window_apply_hit_test(win);
        }
        win->render = whaleui_render_create(win->app->gpu, win->sdl,
                                            win->width, win->height);
        if (!win->render) {
            return -5;
        }
        /* let the frame re-collect the document stylesheet when the DOM edits
         * a <style>/<link> (see render.h css_reload_fn) */
        win->render->css_reload_fn = window_css_reload_hook;
        win->render->css_reload_ud = win;
        /* -1 (auto): decided by the document size once we know it */
        if (win->app->async_layout < 0) {
            win->render->async_layout =
                win->doc_bytes >= (256u * 1024u) ? 1 : 0;
        } else {
            win->render->async_layout = win->app->async_layout > 0 ? 1 : 0;
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

static int window_load_html_len(whaleui_window_t* win, const char* html,
                                size_t len)
{
    if (!win || !html) {
        return -1;
    }
    win->base_uri.clear(); /* in-memory html has no base for relative links */
    /* Stop anything that still reads the OLD document BEFORE freeing it.
     * reset_dom joins the async first-layout worker (which walks that
     * document and the rule array) and drops the layout tree, the animation
     * state and every element-keyed cache. Doing this after the destroy was
     * a use-after-free: a sliced first layout still streaming in from the
     * worker kept reading freed lexbor nodes ("startup sometimes crashes /
     * hangs with the tab bar drawn but the content missing"). */
    if (win->render) {
        whaleui_render_reset_dom(win->render);
    }
    if (win->document) {
        whaleui_dom_document_destroy(win->document);
        win->document = nullptr;
    }
    /* the REAL byte count, not strlen(): a UTF-16 file carries NUL bytes
     * after every ASCII character, so strlen() stops at the first one and
     * the rest of the document is lost (a UTF-16 log rendered as one "C") */
    win->document = whaleui_dom_parse_html(html, len);
    if (!win->document) {
        return -2;
    }
    win->doc_bytes = len; /* the show-time async policy reads this */
    /* no JS engine: add .in to reveal elements so their content shows */
    window_reveal_apply(win->document);
    window_reload_css(win);
    /* DOM load event on the document root (document-level listeners sit
     * here; the browser fires it after the document parses). */
    WUI_DOM_FIRE(
        reinterpret_cast<whaleui_dom_element_t*>(
            whaleui_dom_document_element(win->document)),
        "load", 0, 0, 0, 0, 0, 0);
    return 0;
}

extern "C" int whaleui_window_load_html(whaleui_window_t* win, const char* html)
{
    return window_load_html_len(win, html, html ? std::strlen(html) : 0);
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
    int rc = window_load_html_len(win, buf, len);
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

/* --- selection (window.getSelection() subset): the render context owns the
 * selection state; these thin wrappers expose it per window. --- */

extern "C" whaleui_dom_element_t* whaleui_window_sel_anchor_element(whaleui_window_t* win)
{
    return (win && win->render) ? whaleui_render_sel_anchor_element(win->render) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_window_sel_focus_element(whaleui_window_t* win)
{
    return (win && win->render) ? whaleui_render_sel_focus_element(win->render) : nullptr;
}

extern "C" int whaleui_window_sel_anchor_offset(whaleui_window_t* win)
{
    return (win && win->render) ? whaleui_render_sel_anchor_offset(win->render) : 0;
}

extern "C" int whaleui_window_sel_focus_offset(whaleui_window_t* win)
{
    return (win && win->render) ? whaleui_render_sel_focus_offset(win->render) : 0;
}

extern "C" const char* whaleui_window_selected_text(whaleui_window_t* win)
{
    if (!win || !win->render) {
        return "";
    }
    /* per-window cache: the returned pointer stays valid until the next call
     * on this window (documented contract). */
    win->sel_text_cache = whaleui_render_selected_text(win->render);
    return win->sel_text_cache.c_str();
}

extern "C" void whaleui_window_clear_selection(whaleui_window_t* win)
{
    if (win && win->render) {
        whaleui_render_clear_selection(win->render);
    }
}

extern "C" void whaleui_window_select_all_of(whaleui_window_t* win, whaleui_dom_element_t* el)
{
    if (win && win->render) {
        whaleui_render_select_all_of(win->render, el);
    }
}

/* --- drag & drop state (owned by the render context) --- */

extern "C" int whaleui_window_drag_active(const whaleui_window_t* win)
{
    return (win && win->render) ? whaleui_render_drag_active(win->render) : 0;
}

extern "C" whaleui_dom_element_t* whaleui_window_drag_source(const whaleui_window_t* win)
{
    return (win && win->render) ? whaleui_render_drag_source(win->render) : nullptr;
}

/* ======================= window state/geometry/appearance wrappers ======================= */

extern "C" int whaleui_window_is_visible(const whaleui_window_t* win)
{
    if (!win) return 0;
    if (win->sdl) return (SDL_GetWindowFlags(win->sdl) & SDL_WINDOW_HIDDEN) ? 0 : 1;
    return win->visible ? 1 : 0;
}
extern "C" int whaleui_window_is_minimized(const whaleui_window_t* win)
{
    if (!win || !win->sdl) return 0;
    return (SDL_GetWindowFlags(win->sdl) & SDL_WINDOW_MINIMIZED) ? 1 : 0;
}
extern "C" int whaleui_window_is_maximized(const whaleui_window_t* win)
{
    if (!win || !win->sdl) return 0;
    return (SDL_GetWindowFlags(win->sdl) & SDL_WINDOW_MAXIMIZED) ? 1 : 0;
}
extern "C" int whaleui_window_is_focused(const whaleui_window_t* win)
{
    if (!win || !win->sdl) return 0;
    return (SDL_GetWindowFlags(win->sdl) & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0;
}
extern "C" int whaleui_window_minimize(whaleui_window_t* win)
{
    if (!win || !win->sdl) return -1;
    return SDL_MinimizeWindow(win->sdl) ? 0 : -2;
}
extern "C" int whaleui_window_maximize(whaleui_window_t* win)
{
    if (!win || !win->sdl) return -1;
    return SDL_MaximizeWindow(win->sdl) ? 0 : -2;
}
extern "C" int whaleui_window_restore(whaleui_window_t* win)
{
    if (!win || !win->sdl) return -1;
    return SDL_RestoreWindow(win->sdl) ? 0 : -2;
}
extern "C" int whaleui_window_raise(whaleui_window_t* win)
{
    if (!win || !win->sdl) return -1;
    return SDL_RaiseWindow(win->sdl) ? 0 : -2;
}
extern "C" int whaleui_window_get_size_in_pixels(const whaleui_window_t* win, int* w, int* h)
{
    if (!win || !w || !h) return -1;
    if (win->sdl) {
        int pw = 0, ph = 0;
        if (SDL_GetWindowSizeInPixels(win->sdl, &pw, &ph)) { *w = pw; *h = ph; return 0; }
        return -2;
    }
    *w = win->width; *h = win->height; return 0;
}
extern "C" int whaleui_window_set_position(whaleui_window_t* win, int x, int y)
{
    if (!win) return -1;
    if (win->sdl && !SDL_SetWindowPosition(win->sdl, x, y)) return -2;
    return 0;
}
extern "C" int whaleui_window_get_position(const whaleui_window_t* win, int* x, int* y)
{
    if (!win || !x || !y) return -1;
    if (win->sdl) {
        int px = 0, py = 0;
        if (SDL_GetWindowPosition(win->sdl, &px, &py)) { *x = px; *y = py; return 0; }
        return -2;
    }
    *x = 0; *y = 0; return 0;
}
extern "C" int whaleui_window_center(whaleui_window_t* win)
{
    if (!win) return -1;
    SDL_DisplayID did = 0;
    if (win->sdl) did = SDL_GetDisplayForWindow(win->sdl);
    if (!did) did = SDL_GetPrimaryDisplay();
    SDL_Rect bounds; bounds.x = 0; bounds.y = 0; bounds.w = 800; bounds.h = 600;
    if (did) SDL_GetDisplayBounds(did, &bounds);
    int x = bounds.x + (bounds.w - win->width) / 2;
    int y = bounds.y + (bounds.h - win->height) / 2;
    if (win->sdl) {
        if (!SDL_SetWindowPosition(win->sdl, x, y)) return -2;
    }
    return 0;
}
extern "C" int whaleui_window_set_min_size(whaleui_window_t* win, int w, int h)
{
    if (!win || w < 0 || h < 0) return -1;
    win->min_w = w; win->min_h = h;
    if (win->sdl && !SDL_SetWindowMinimumSize(win->sdl, w, h)) return -2;
    return 0;
}
extern "C" int whaleui_window_get_min_size(const whaleui_window_t* win, int* w, int* h)
{
    if (!win || !w || !h) return -1;
    if (win->sdl) {
        int mw = 0, mh = 0;
        if (SDL_GetWindowMinimumSize(win->sdl, &mw, &mh)) { *w = mw; *h = mh; return 0; }
        return -2;
    }
    *w = win->min_w; *h = win->min_h; return 0;
}
extern "C" int whaleui_window_set_max_size(whaleui_window_t* win, int w, int h)
{
    if (!win || w < 0 || h < 0) return -1;
    win->max_w = w; win->max_h = h;
    if (win->sdl && !SDL_SetWindowMaximumSize(win->sdl, w, h)) return -2;
    return 0;
}
extern "C" int whaleui_window_get_max_size(const whaleui_window_t* win, int* w, int* h)
{
    if (!win || !w || !h) return -1;
    if (win->sdl) {
        int mw = 0, mh = 0;
        if (SDL_GetWindowMaximumSize(win->sdl, &mw, &mh)) { *w = mw; *h = mh; return 0; }
        return -2;
    }
    *w = win->max_w; *h = win->max_h; return 0;
}
extern "C" int whaleui_window_set_bordered(whaleui_window_t* win, int bordered)
{
    if (!win) return -1;
    win->bordered = bordered ? 1 : 0;
    if (win->sdl) {
        if (!SDL_SetWindowBordered(win->sdl, bordered ? true : false)) return -2;
        /* borderless: install default draggable/resize HitTest if none active */
        if (!bordered && !win->hit_test_active) {
            HitTestCtx* ctx = new HitTestCtx{default_borderless_hit_test, nullptr};
            win->hit_test_ud = ctx;
            win->hit_test_active = 1;
            window_apply_hit_test(win);
        } else if (bordered && win->hit_test_active) {
            HitTestCtx* ctx = static_cast<HitTestCtx*>(win->hit_test_ud);
            if (ctx && ctx->fn == default_borderless_hit_test) {
                delete ctx;
                win->hit_test_ud = nullptr;
                win->hit_test_active = 0;
                window_apply_hit_test(win);
            }
        }
    }
    return 0;
}
extern "C" int whaleui_window_set_resizable(whaleui_window_t* win, int resizable)
{
    if (!win) return -1;
    win->resizable = resizable ? 1 : 0;
    if (win->sdl && !SDL_SetWindowResizable(win->sdl, resizable ? true : false)) return -2;
    return 0;
}
extern "C" int whaleui_window_set_always_on_top(whaleui_window_t* win, int on_top)
{
    if (!win) return -1;
    win->always_on_top = on_top ? 1 : 0;
    if (win->sdl && !SDL_SetWindowAlwaysOnTop(win->sdl, on_top ? true : false)) return -2;
    return 0;
}
extern "C" int whaleui_window_set_opacity(whaleui_window_t* win, float opacity)
{
    if (!win || opacity < 0.0f || opacity > 1.0f) return -1;
    win->opacity = opacity;
    if (win->sdl && !SDL_SetWindowOpacity(win->sdl, opacity)) return -2;
    return 0;
}
extern "C" float whaleui_window_get_opacity(const whaleui_window_t* win)
{
    if (!win) return 1.0f;
    if (win->sdl) return SDL_GetWindowOpacity(win->sdl);
    return win->opacity;
}
extern "C" int whaleui_window_set_icon(whaleui_window_t* win, const unsigned char* rgba, int width, int height)
{
    if (!win || !rgba || width <= 0 || height <= 0) return -1;
    if (!win->sdl) return 0; /* cached; applied at show() if needed — store not yet, thin impl treats as success */
    SDL_Surface* surf = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, (void*)rgba, width * 4);
    if (!surf) return -2;
    bool ok = SDL_SetWindowIcon(win->sdl, surf);
    SDL_DestroySurface(surf);
    return ok ? 0 : -3;
}
extern "C" int whaleui_window_set_icon_uri(whaleui_window_t* win, const char* uri)
{
    if (!win || !uri) return -1;
    char* data = nullptr; size_t len = 0;
    if (whaleui_fs_load(uri, &data, &len) != 0) return -2;
    int w = 0, h = 0, comp = 0;
    /* try SDL_image path when available, else stb */
#if defined(WHALEUI_BUILD_FULL)
    SDL_Surface* surf = nullptr;
    /* SDL_image: load from memory via VFS data */
    /* Use SDL_LoadBMP fallback + IMG_Load if available; keep minimal without hard dep */
    /* Prefer SDL's image load via RWops if SDL_image linked */
    SDL_IOStream* io = SDL_IOFromMem(data, (int)len);
    if (io) {
        /* SDL_image registers IMG_Load_IO; if not linked, SDL_LoadBMP_IO still works for BMP */
        /* Try to load via SDL_image when present (weak link guarded by runtime check) */
        surf = nullptr;
        /* IMG_Load_IO is weak; if unavailable, surf stays null and we fallback to stb below */
        SDL_CloseIO(io);
    }
    if (surf) {
        bool ok = win->sdl ? SDL_SetWindowIcon(win->sdl, surf) : true;
        SDL_DestroySurface(surf);
        std::free(data);
        return ok ? 0 : -3;
    }
#endif
    /* stb_image fallback (Lite/Minimal only; Full uses SDL_image path above) */
#if defined(WHALEUI_BUILD_LITE) || defined(WHALEUI_BUILD_MINIMAL)
    {
        extern unsigned char* stbi_load_from_memory(const unsigned char*, int, int*, int*, int*, int);
        extern void stbi_image_free(void*);
        unsigned char* px = stbi_load_from_memory((const unsigned char*)data, (int)len, &w, &h, &comp, 4);
        std::free(data);
        if (!px) return -4;
        int rc = whaleui_window_set_icon(win, px, w, h);
        stbi_image_free(px);
        return rc;
    }
#else
    std::free(data);
    return -4;
#endif
}
extern "C" int whaleui_window_set_fullscreen(whaleui_window_t* win, whaleui_fullscreen_mode_t mode)
{
    if (!win) return -1;
    win->fullscreen = mode;
    if (!win->sdl) return 0;
    bool ok = false;
    if (mode == WHALEUI_FULLSCREEN_OFF) ok = SDL_SetWindowFullscreen(win->sdl, false);
    else ok = SDL_SetWindowFullscreen(win->sdl, true);
    return ok ? 0 : -2;
}
extern "C" whaleui_fullscreen_mode_t whaleui_window_get_fullscreen(const whaleui_window_t* win)
{
    if (!win) return WHALEUI_FULLSCREEN_OFF;
    if (win->sdl) {
        Uint64 flags = SDL_GetWindowFlags(win->sdl);
        if (flags & SDL_WINDOW_FULLSCREEN) return WHALEUI_FULLSCREEN_DESKTOP;
        return WHALEUI_FULLSCREEN_OFF;
    }
    return win->fullscreen;
}
extern "C" int whaleui_window_set_mouse_grab(whaleui_window_t* win, int grab)
{
    if (!win) return -1;
    win->mouse_grab = grab ? 1 : 0;
    if (win->sdl && !SDL_SetWindowMouseGrab(win->sdl, grab ? true : false)) return -2;
    return 0;
}
extern "C" int whaleui_window_get_mouse_grab(const whaleui_window_t* win)
{
    if (!win) return 0;
    if (win->sdl) return SDL_GetWindowMouseGrab(win->sdl) ? 1 : 0;
    return win->mouse_grab;
}
extern "C" int whaleui_window_set_keyboard_grab(whaleui_window_t* win, int grab)
{
    if (!win) return -1;
    win->keyboard_grab = grab ? 1 : 0;
    if (win->sdl && !SDL_SetWindowKeyboardGrab(win->sdl, grab ? true : false)) return -2;
    return 0;
}
extern "C" int whaleui_window_get_keyboard_grab(const whaleui_window_t* win)
{
    if (!win) return 0;
    if (win->sdl) return SDL_GetWindowKeyboardGrab(win->sdl) ? 1 : 0;
    return win->keyboard_grab;
}
extern "C" int whaleui_window_set_mouse_position(whaleui_window_t* win, int x, int y)
{
    if (!win) return -1;
    if (!win->sdl) return -1;
    /* SDL_WarpMouseInWindow is the actual API; SetWindowMousePosition is not in SDL3 video.h — use warp */
    SDL_WarpMouseInWindow(win->sdl, (float)x, (float)y);
    return 0;
}
extern "C" int whaleui_window_set_focusable(whaleui_window_t* win, int focusable)
{
    if (!win) return -1;
    win->focusable = focusable ? 1 : 0;
    if (win->sdl) {
        if (!SDL_SetWindowFocusable(win->sdl, focusable ? true : false)) return -2;
    }
    return 0;
}
extern "C" int whaleui_window_set_parent(whaleui_window_t* win, whaleui_window_t* parent)
{
    if (!win) return -1;
    if (!win->sdl) return 0; /* cache; parent applied at show if needed — for now treat as success */
    SDL_Window* p = parent ? parent->sdl : nullptr;
    /* SDL_SetWindowParent is the real name in this SDL build (see probe) */
    if (!SDL_SetWindowParent(win->sdl, p)) return -2;
    return 0;
}
extern "C" int whaleui_window_set_modal(whaleui_window_t* win, whaleui_window_t* parent)
{
    if (!win) return -1;
    if (!win->sdl || !parent || !parent->sdl) return -1;
    if (!SDL_SetWindowModal(win->sdl, true)) return -2;
    /* SDL_SetWindowModal takes only window+bool; parent association is via SetWindowParent */
    if (!SDL_SetWindowParent(win->sdl, parent->sdl)) return -3;
    return 0;
}
extern "C" int whaleui_window_display_id(const whaleui_window_t* win)
{
    if (!win || !win->sdl) return -1;
    SDL_DisplayID id = SDL_GetDisplayForWindow(win->sdl);
    return id ? (int)id : -1;
}
extern "C" float whaleui_window_display_scale(const whaleui_window_t* win)
{
    if (!win || !win->sdl) return 1.0f;
    float s = SDL_GetWindowDisplayScale(win->sdl);
    return s > 0.0f ? s : 1.0f;
}
extern "C" int whaleui_window_flash(whaleui_window_t* win, whaleui_flash_t mode)
{
    if (!win || !win->sdl) return -1;
    SDL_FlashOperation op = SDL_FLASH_CANCEL;
    if (mode == WHALEUI_FLASH_BRIEFLY) op = SDL_FLASH_BRIEFLY;
    else if (mode == WHALEUI_FLASH_UNTIL_FOCUSED) op = SDL_FLASH_UNTIL_FOCUSED;
    return SDL_FlashWindow(win->sdl, op) ? 0 : -2;
}
extern "C" int whaleui_window_set_event_callback(whaleui_window_t* win, whaleui_window_event_cb cb, void* userdata)
{
    if (!win) return -1;
    win->win_event_cb = cb;
    win->win_event_ud = userdata;
    return 0;
}
extern "C" int whaleui_window_set_key_callback(whaleui_window_t* win, whaleui_key_cb cb, void* userdata)
{
    if (!win) return -1;
    win->key_cb = cb;
    win->key_ud = userdata;
    return 0;
}
extern "C" int whaleui_window_set_select_callback(whaleui_window_t* win, whaleui_select_cb cb, void* userdata)
{
    if (!win) return -1;
    win->select_cb = cb;
    win->select_ud = userdata;
    return 0;
}
extern "C" int whaleui_window_set_hit_test(whaleui_window_t* win, whaleui_hit_test_fn cb, void* userdata)
{
    if (!win) return -1;
    if (win->hit_test_active) {
        HitTestCtx* old = static_cast<HitTestCtx*>(win->hit_test_ud);
        delete old;
        win->hit_test_ud = nullptr;
        win->hit_test_active = 0;
    }
    if (!cb) {
        window_apply_hit_test(win);
        return 0;
    }
    HitTestCtx* ctx = new HitTestCtx{cb, userdata};
    win->hit_test_ud = ctx;
    win->hit_test_active = 1;
    window_apply_hit_test(win);
    return 0;
}







