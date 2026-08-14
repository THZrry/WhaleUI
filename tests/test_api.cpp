// test_api: public C API contract tests (app/theme/render options/variant).
#include "whaleui.h"

#include <cassert>
#include <cstring>

int main(void)
{
    /* build info */
    assert(std::strcmp(whaleui_version(), WHALEUI_VERSION) == 0);
    const char* v = whaleui_variant();
    assert(std::strcmp(v, "full") == 0 || std::strcmp(v, "lite") == 0 ||
           std::strcmp(v, "minimal") == 0);

    /* lifecycle */
    {
        whaleui_app_t* app = whaleui_app_create();
        assert(app != nullptr);

        assert(whaleui_app_get_theme(app) == WHALEUI_THEME_SYSTEM);

        assert(whaleui_app_set_theme(app, WHALEUI_THEME_DARK) == 0);
        assert(whaleui_app_get_theme(app) == WHALEUI_THEME_DARK);

        assert(whaleui_app_set_theme(app, WHALEUI_THEME_LIGHT) == 0);
        assert(whaleui_app_get_theme(app) == WHALEUI_THEME_LIGHT);

        /* invalid theme rejected */
        assert(whaleui_app_set_theme(app, (whaleui_theme_t)99) != 0);

        /* accent color */
        assert(whaleui_app_set_accent_color(app, "#FF8800") == 0);
        assert(whaleui_app_set_accent_color(nullptr, "#FF8800") != 0);

        /* render options */
        assert(whaleui_app_set_render_option(app, WHALEUI_RENDER_MAX_FPS, 120) == 0);
        assert(whaleui_app_set_render_option(app, WHALEUI_RENDER_BATTERY_SAVER, 0) == 0);
        assert(whaleui_app_set_render_option(app, WHALEUI_RENDER_VSYNC, 0) == 0);
        assert(whaleui_app_set_render_option(app, (whaleui_render_option_t)99, 1) != 0);

        /* key / select callbacks */
        assert(whaleui_app_set_key_callback(app, nullptr, nullptr) == 0);
        assert(whaleui_app_set_key_callback(nullptr, nullptr, nullptr) != 0);
        assert(whaleui_app_set_select_callback(app, nullptr, nullptr) == 0);
        assert(whaleui_app_set_select_callback(nullptr, nullptr, nullptr) != 0);

        /* run/quit (stub loop returns immediately) */
        assert(whaleui_app_run(app) == 0);

        whaleui_app_destroy(app);
    }

    /* null-safety */
    {
        whaleui_app_destroy(nullptr); /* no-op */
        assert(whaleui_app_get_theme(nullptr) == WHALEUI_THEME_SYSTEM);
        assert(whaleui_app_set_theme(nullptr, WHALEUI_THEME_DARK) != 0);
        assert(whaleui_app_run(nullptr) != 0);
    }

    return 0;
}
