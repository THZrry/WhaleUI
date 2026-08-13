/* Application core: public C API implementation.
 * Step 2: contract implementation (state kept, event loop stub). */

#include "core/app.h"

#include <cstring>

extern "C" whaleui_app_t* whaleui_app_create(void)
{
    whaleui_app_t* app = new whaleui_app_t;
    app->theme = WHALEUI_THEME_SYSTEM;
    std::strcpy(app->accent, "#0078D4"); /* default accent */
    app->max_fps = 0;
    app->battery_saver = 1; /* default 60fps in battery saver */
    app->vsync = 1;
    app->running = 0;
    return app;
}

extern "C" void whaleui_app_destroy(whaleui_app_t* app)
{
    delete app;
}

extern "C" int whaleui_app_run(whaleui_app_t* app)
{
    if (!app) {
        return -1;
    }
    app->running = 1;
    /* Step 2: no event loop yet. Quit immediately. */
    app->running = 0;
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
    return 0;
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
