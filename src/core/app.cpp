/* Application core: public C API implementation.
 * Event loop polls SDL events, repaints visible windows, honors the
 * max-fps / battery-saver frame cap. */

#include "core/app.h"
#include "core/window.h"
#include "dom/events.h"
#include "platform/platform.h"
#include "render/render.h"
#include "style/theme.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace {

/* dispatch a DOM event on the given element (no-op when NULL) */
void dom_dispatch(whaleui_dom_element_t* el, const char* type, int key,
                  int mx, int my, int button, float wx, float wy)
{
    if (el) {
        whaleui_dom_dispatch_event_full(
            reinterpret_cast<lxb_dom_element*>(el), type, key, mx, my, button,
            wx, wy);
    }
}
/* theme resolution: SYSTEM -> the OS scheme captured at app_create */
whaleui_theme_t resolved_theme(const whaleui_app_t* app)
{
    if (!app) {
        return WHALEUI_THEME_LIGHT;
    }
    return app->theme == WHALEUI_THEME_SYSTEM ? app->system_theme : app->theme;
}

/* window owning an SDL window id (used by key/text/wheel dispatch) */
whaleui_window_t* window_for(whaleui_app_t* app, SDL_WindowID id)
{
    for (whaleui_window_t* win : app->windows) {
        if (win->sdl && SDL_GetWindowID(win->sdl) == id) {
            return win;
        }
    }
    return nullptr;
}

/* render worker thread: renders+present all visible windows on a separate
 * thread so a slow frame (27ms) never blocks the event loop - the main
 * thread keeps polling input, so hover/typing stay responsive even while a
 * frame renders. The main thread sets frame_request; the worker takes
 * render_lock for the whole frame (main-thread input mostly mutates state
 * without locking - the worker reads it one frame behind at worst). */
void render_worker_fn(whaleui_app_t* app)
{
    while (app->running) {
        if (app->frame_request.load()) {
            std::lock_guard<std::mutex> lk(app->render_lock);
            for (whaleui_window_t* win : app->windows) {
                if (win->visible && win->sdl && win->render && win->document) {
                    if (SDL_GetWindowFlags(win->sdl) & SDL_WINDOW_MINIMIZED) {
                        continue;
                    }
                    whaleui_render_frame(win->render, win->document);
                }
            }
            app->frame_done.store(1);
            app->frame_request.store(0);
        } else {
            std::this_thread::yield();
        }
    }
}
} // namespace

whaleui_theme_t whaleui_app_resolved_theme(const whaleui_app_t* app)
{
    return resolved_theme(app);
}

extern "C" whaleui_app_t* whaleui_app_create(void)
{
    if (whaleui_platform_init() != 0) {
        return nullptr;
    }
    whaleui_app_t* app = new whaleui_app_t;
    app->theme = WHALEUI_THEME_SYSTEM;
    /* capture the OS scheme once at init (defaults to light when the
     * platform cannot report one, e.g. older systems) */
    app->system_theme = whaleui_platform_system_theme();
    std::strcpy(app->theme_style, "fluent");
    std::strcpy(app->accent, "#0067c0"); /* default accent (Win11 Fluent blue) */
    app->max_fps = 0;
    app->async_layout = 0; /* async first layout: opt-in only */
    /* battery saver is a *default*, gated on the actual system power state:
     * 60fps (and FSR, see fsr_want_active) while on battery, uncapped while
     * plugged in. The state is re-polled in the event loop, so unplugging
     * throttles the app without user intervention. */
    app->battery_saver = 1;
    app->on_battery =
        (SDL_GetPowerInfo(nullptr, nullptr) == SDL_POWERSTATE_ON_BATTERY) ? 1 : 0;
    app->power_check_ticks = 0;
    app->vsync = 1;
    app->running = 0;
    /* default is FULL motion (animated pages play their CSS animations).
     * The no-JS tradeoff is documented: a reveal-on-scroll page (opacity:0
     * until JS adds .in) would keep its content hidden without a JS engine,
     * so apps can set reduced motion (whaleui_app_set_reduced_motion(app,1))
     * to surface that content - but that also stops the page's decorative
     * @keyframes animations. Animated content wins by default. */
    app->reduced_motion = 0;
    app->gpu = nullptr;
    app->select_cb = nullptr;
    app->select_ud = nullptr;
    return app;
}

extern "C" void whaleui_app_destroy(whaleui_app_t* app)
{
    if (!app) {
        return;
    }
    app->running = 0;
    app->frame_request.store(1);
    if (app->render_thread.joinable()) {
        app->render_thread.join();
    }
    for (whaleui_window_t* win : app->windows) {
        whaleui_window_destroy(win);
    }
    app->windows.clear();
    if (app->gpu) {
        SDL_DestroyGPUDevice(app->gpu);
    }
    whaleui_platform_shutdown();
    delete app;
}

extern "C" int whaleui_app_run(whaleui_app_t* app)
{
    if (!app) {
        return -1;
    }
    app->running = 1;

    /* nothing visible -> nothing to run (keeps headless tests fast) */
    bool any = false;
    for (whaleui_window_t* win : app->windows) {
        if (win->visible && win->sdl) {
            any = true;
            break;
        }
    }
    if (!any) {
        app->running = 0;
        return 0;
    }

    Uint64 last = SDL_GetTicks();
    app->render_thread = std::thread(render_worker_fn, app);
    while (app->running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                app->running = 0;
                break;
            case SDL_EVENT_KEY_DOWN: {
                whaleui_window_t* w = window_for(app, e.key.windowID);
                if (w && w->render) {
                    whaleui_render_handle_key(w->render, static_cast<int>(e.key.key),
                                              1, static_cast<int>(e.key.mod));
                    dom_dispatch(whaleui_render_focus_element(w->render),
                                 "keydown", static_cast<int>(e.key.key),
                                 0, 0, 0, 0, 0);
                }
                if (app->key_cb) {
                    app->key_cb(app, static_cast<int>(e.key.key), 1, app->key_ud);
                }
                break;
            }
            case SDL_EVENT_KEY_UP: {
                whaleui_window_t* w = window_for(app, e.key.windowID);
                if (w && w->render) {
                    whaleui_render_handle_key(w->render, static_cast<int>(e.key.key),
                                              0, static_cast<int>(e.key.mod));
                    dom_dispatch(whaleui_render_focus_element(w->render),
                                 "keyup", static_cast<int>(e.key.key),
                                 0, 0, 0, 0, 0);
                }
                if (app->key_cb) {
                    app->key_cb(app, static_cast<int>(e.key.key), 0, app->key_ud);
                }
                break;
            }
            case SDL_EVENT_TEXT_INPUT: {
                whaleui_window_t* w = window_for(app, e.text.windowID);
                if (w && w->render) {
                    whaleui_render_handle_text(w->render, e.text.text);
                }
                break;
            }
            case SDL_EVENT_TEXT_EDITING: {
                whaleui_window_t* w = window_for(app, e.edit.windowID);
                if (w && w->render) {
                    whaleui_render_handle_editing(w->render, e.edit.text);
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                for (whaleui_window_t* win : app->windows) {
                    if (win->render && SDL_GetWindowID(win->sdl) == e.wheel.windowID) {
                        whaleui_render_handle_wheel(win->render,
                                                    static_cast<int>(e.wheel.mouse_x),
                                                    static_cast<int>(e.wheel.mouse_y),
                                                    e.wheel.y);
                        break;
                    }
                }
                break;
            }
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                for (whaleui_window_t* win : app->windows) {
                    if (win->sdl && SDL_GetWindowID(win->sdl) == e.window.windowID) {
                        win->visible = 0;
                        app->running = 0;
                        break;
                    }
                }
                break;
            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_RESTORED:
                /* the window just became visible again (e.g. unminimized):
                 * force a repaint so it isn't left stale */
                for (whaleui_window_t* win : app->windows) {
                    if (win->render &&
                        SDL_GetWindowID(win->sdl) == e.window.windowID) {
                        whaleui_render_invalidate(win->render);
                        break;
                    }
                }
                break;
            case SDL_EVENT_WINDOW_RESIZED: {
                for (whaleui_window_t* win : app->windows) {
                    if (win->sdl && SDL_GetWindowID(win->sdl) == e.window.windowID) {
                        win->width = e.window.data1;
                        win->height = e.window.data2;
                        if (win->render) {
                            whaleui_render_resize(win->render, win->width, win->height);
                        }
                        break;
                    }
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
                for (whaleui_window_t* win : app->windows) {
                    if (win->render && SDL_GetWindowID(win->sdl) == e.motion.windowID) {
                        whaleui_render_set_hover(win->render,
                                                 static_cast<int>(e.motion.x),
                                                 static_cast<int>(e.motion.y));
                        break;
                    }
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    for (whaleui_window_t* win : app->windows) {
                        if (win->render && win->document &&
                            SDL_GetWindowID(win->sdl) == e.button.windowID) {
                            whaleui_render_set_pressed_ex(win->render,
                                                          static_cast<int>(e.button.x),
                                                          static_cast<int>(e.button.y),
                                                          1, static_cast<int>(e.button.clicks),
                                                          static_cast<int>(SDL_GetModState()));
                            const char* val = nullptr;
                            if (whaleui_render_handle_click(win->render,
                                                            e.button.x, e.button.y,
                                                            &val) &&
                                val && app->select_cb) {
                                app->select_cb(app, val, app->select_ud);
                            }
                            whaleui_dom_element_t* hit = whaleui_render_hit_element(
                                win->render, static_cast<int>(e.button.x),
                                static_cast<int>(e.button.y));
                            dom_dispatch(hit, "mousedown", 0,
                                         static_cast<int>(e.button.x),
                                         static_cast<int>(e.button.y),
                                         e.button.button, 0, 0);
                            dom_dispatch(hit, "click", 0,
                                         static_cast<int>(e.button.x),
                                         static_cast<int>(e.button.y),
                                         e.button.button, 0, 0);
                            break;
                        }
                    }
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    for (whaleui_window_t* win : app->windows) {
                        if (win->render &&
                            SDL_GetWindowID(win->sdl) == e.button.windowID) {
                            whaleui_render_set_pressed_ex(win->render,
                                                          static_cast<int>(e.button.x),
                                                          static_cast<int>(e.button.y),
                                                          0, 1,
                                                          static_cast<int>(SDL_GetModState()));
                            dom_dispatch(
                                whaleui_render_hit_element(
                                    win->render, static_cast<int>(e.button.x),
                                    static_cast<int>(e.button.y)),
                                "mouseup", 0, static_cast<int>(e.button.x),
                                static_cast<int>(e.button.y),
                                e.button.button, 0, 0);
                            break;
                        }
                    }
                }
                break;
            default:
                break;
            }
        }

        /* hand rendering to the worker thread: the event loop keeps polling
         * input while a frame renders (a slow ~27ms frame no longer blocks
         * the UI). The worker takes render_lock for the whole frame. */
        app->frame_request.store(1);

        /* power-state poll (~2s): unplugging throttles the loop to the
         * battery-saver cap, plugging back in uncaps it. SDL3 3.4 has no
         * power-change event to listen for, so poll cheaply (one system
         * call; 2s is plenty - battery transitions are not time-critical). */
        Uint64 now = SDL_GetTicks();
        if (now - app->power_check_ticks > 2000) {
            app->power_check_ticks = now;
            app->on_battery =
                (SDL_GetPowerInfo(nullptr, nullptr) == SDL_POWERSTATE_ON_BATTERY) ? 1 : 0;
        }
        /* frame cap: max_fps wins; otherwise battery saver caps at 60 while
         * on battery, AC power stays uncapped */
        int fps = app->max_fps > 0
                      ? app->max_fps
                      : (app->battery_saver && app->on_battery ? 60 : 0);
        if (fps > 0) {
            Uint64 target = last + 1000 / fps;
            if (now < target) {
                SDL_Delay(static_cast<Uint32>(target - now));
                now = target;
            }
        }
        last = now;
    }
    app->running = 0;
    app->frame_request.store(1);
    app->render_thread.join();
    return 0;
}

extern "C" void whaleui_app_quit(whaleui_app_t* app)
{
    if (app) {
        app->running = 0;
    }
}

extern "C" int whaleui_app_set_theme(whaleui_app_t* app, whaleui_theme_t theme)
{
    if (!app || theme < WHALEUI_THEME_SYSTEM || theme > WHALEUI_THEME_DARK) {
        return -1;
    }
    app->theme = theme;
    /* re-parse the stylesheet with the new theme variables, then repaint */
    for (whaleui_window_t* win : app->windows) {
        if (win->render) {
            whaleui_window_refresh_css(win);
        }
    }
    return 0;
}

extern "C" int whaleui_app_set_theme_style(whaleui_app_t* app, const char* style)
{
    if (!app || !style) {
        return -1;
    }
    const char* resolved = whaleui_theme_resolve(style);
    std::strncpy(app->theme_style, resolved, sizeof(app->theme_style) - 1);
    app->theme_style[sizeof(app->theme_style) - 1] = '\0';
    for (whaleui_window_t* win : app->windows) {
        if (win->render) {
            whaleui_window_refresh_css(win);
        }
    }
    return 0;
}

extern "C" const char* whaleui_app_get_theme_style(const whaleui_app_t* app)
{
    return app ? app->theme_style : "fluent";
}

extern "C" whaleui_theme_t whaleui_app_get_theme(const whaleui_app_t* app)
{
    return app ? app->theme : WHALEUI_THEME_SYSTEM;
}

extern "C" int whaleui_app_set_accent_color(whaleui_app_t* app, const char* hex)
{
    if (!app || !hex) {
        return -1;
    }
    std::strncpy(app->accent, hex, sizeof(app->accent) - 1);
    app->accent[sizeof(app->accent) - 1] = '\0';
    /* accent feeds --accent; refresh windows to pick it up */
    for (whaleui_window_t* win : app->windows) {
        if (win->render) {
            whaleui_window_refresh_css(win);
        }
    }
    return 0;
}

extern "C" int whaleui_app_set_key_callback(whaleui_app_t* app,
                                            whaleui_key_cb cb, void* userdata)
{
    if (!app) {
        return -1;
    }
    app->key_cb = cb;
    app->key_ud = userdata;
    return 0;
}

extern "C" int whaleui_app_set_select_callback(whaleui_app_t* app,
                                               whaleui_select_cb cb, void* userdata)
{
    if (!app) {
        return -1;
    }
    app->select_cb = cb;
    app->select_ud = userdata;
    return 0;
}

extern "C" int whaleui_app_set_render_option(whaleui_app_t* app,
                                             whaleui_render_option_t opt, int value)
{
    if (!app) {
        return -1;
    }
    switch (opt) {
    case WHALEUI_RENDER_MAX_FPS:       app->max_fps = value; break;
    case WHALEUI_RENDER_BATTERY_SAVER: app->battery_saver = value; break;
    case WHALEUI_RENDER_VSYNC:         app->vsync = value; break;
    case WHALEUI_RENDER_ASYNC_LAYOUT:
        app->async_layout = value ? 1 : 0;
        /* nothing to re-apply: each window reads it on its first frame */
        break;
    default: return -1;
    }
    return 0;
}

extern "C" int whaleui_app_set_text_scale(whaleui_app_t* app, float scale)
{
    if (!app || scale <= 0.0f || scale > 4.0f) {
        return -1;
    }
    for (whaleui_window_t* win : app->windows) {
        if (win->render) {
            win->render->text_scale = scale;
            win->render->has_dirty = 1;
        }
    }
    return 0;
}

extern "C" int whaleui_app_set_reduced_motion(whaleui_app_t* app, int reduce)
{
    if (!app) {
        return -1;
    }
    app->reduced_motion = reduce ? 1 : 0;
    for (whaleui_window_t* win : app->windows) {
        whaleui_window_refresh_css(win);
    }
    return 0;
}
