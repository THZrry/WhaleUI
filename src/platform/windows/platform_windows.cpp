/* Windows platform backend.
 * Step 2: contract stub. Real window/event/font-dir implementation lands
 * with the render pipeline in step 3. */

#include "platform/platform.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

extern "C" int whaleui_platform_system_font_dirs(const char** out, int n)
{
    if (!out || n <= 0) {
        return 0;
    }
    /* Windows system fonts live in %WINDIR%\Fonts */
    const char* windir = std::getenv("WINDIR");
    static char path[512];
    if (!windir) {
        return 0;
    }
    std::snprintf(path, sizeof(path), "%s\\Fonts", windir);
    out[0] = path;
    return 1;
}

extern "C" whaleui_theme_t whaleui_platform_system_theme(void)
{
    /* Step 2: light default; registry query (AppsUseLightTheme) in step 3. */
    return WHALEUI_THEME_LIGHT;
}

extern "C" int whaleui_platform_init(void)
{
    /* Step 2: no-op; SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS) in step 3. */
    return 0;
}

extern "C" void whaleui_platform_shutdown(void)
{
    /* Step 2: no-op; SDL_Quit() in step 3. */
}
