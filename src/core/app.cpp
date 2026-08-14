/* Application core: public C API implementation.
 * Event loop polls SDL events, repaints visible windows, honors the
 * max-fps / battery-saver frame cap. */

#include "core/app.h"
#include "core/window.h"
#include "platform/platform.h"
#include "render/render.h"
#include "style/theme.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace {
/* theme resolution: SYSTEM -> platform detection */
whaleui_theme_t resolved_theme(const whaleui_app_t* app)
{
    if (!app) {
        return WHALEUI_THEME_LIGHT;
    }
    return app->theme == WHALEUI_THEME_SYSTEM ? whaleui_platform_system_theme() : app->theme;
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
    std::strcpy(app->theme_style, "fluent");
    std::strcpy(app->accent, "#0067c0"); /* default accent (Win11 Fluent blue) */
    app->max_fps = 0;
    app->battery_saver = 1; /* default 60fps in battery saver */
    app->vsync = 1;
    app->running = 0;
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
    while (app->running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                app->running = 0;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) {
                    app->running = 0;
                } else if (e.key.key == SDLK_T) {
                    /* toggle theme for the demo */
                    whaleui_app_set_theme(app, app->theme == WHALEUI_THEME_DARK
                                                 ? WHALEUI_THEME_LIGHT
                                                 : WHALEUI_THEME_DARK);
                }
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                for (whaleui_window_t* win : app->windows) {
                    if (win->sdl && SDL_GetWindowID(win->sdl) == e.window.windowID) {
                        win->visible = 0;
                        app->running = 0;
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
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    for (whaleui_window_t* win : app->windows) {
                        if (win->render && win->document &&
                            SDL_GetWindowID(win->sdl) == e.button.windowID) {
                            const char* val = nullptr;
                            if (whaleui_render_handle_click(win->render,
                                                            e.button.x, e.button.y,
                                                            &val) &&
                                val && app->select_cb) {
                                app->select_cb(app, val, app->select_ud);
                            }
                            break;
                        }
                    }
                }
                break;
            default:
                break;
            }
        }

        /* paint all visible windows */
        for (whaleui_window_t* win : app->windows) {
            if (win->visible && win->sdl && win->render && win->document) {
                whaleui_render_frame(win->render, win->document);
            }
        }

        /* frame cap: max_fps, else battery saver default 60 */
        int fps = app->max_fps > 0 ? app->max_fps : (app->battery_saver ? 60 : 0);
        Uint64 now = SDL_GetTicks();
        if (fps > 0) {
            Uint64 target = last + 1000 / fps;
            if (now < target) {
                SDL_Delay(static_cast<Uint32>(target - now));
                now = target;
            }
        }
        last = now;
    }
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
    default: return -1;
    }
    return 0;
}
