/* Windows platform backend.
 * SDL creates windows/events cross-platform; this backend only fills what
 * SDL does not cover: system font dirs, OS color scheme, and SDL/TTF init. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "platform/platform.h"

#include <SDL3/SDL.h>
#ifdef WHALEUI_BUILD_FULL
#include <SDL3_ttf/SDL_ttf.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int whaleui_platform_system_font_dirs(const char** out, int n)
{
    if (!out || n <= 0) {
        return 0;
    }
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
    HKEY key = nullptr;
    DWORD value = 1, size = sizeof(value);
    LONG rc = RegGetValueW(HKEY_CURRENT_USER,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                           L"AppsUseLightTheme", RRF_RT_REG_DWORD,
                           nullptr, &value, &size);
    RegCloseKey(key);
    if (rc == ERROR_SUCCESS) {
        return value ? WHALEUI_THEME_LIGHT : WHALEUI_THEME_DARK;
    }
    return WHALEUI_THEME_LIGHT;
}

extern "C" int whaleui_platform_init(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        return -1;
    }
#ifdef WHALEUI_BUILD_FULL
    if (!TTF_Init()) {
        return -1;
    }
#endif
    return 0;
}

extern "C" void whaleui_platform_shutdown(void)
{
#ifdef WHALEUI_BUILD_FULL
    TTF_Quit();
#endif
    SDL_Quit();
}
