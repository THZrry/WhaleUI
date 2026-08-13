#ifndef WHALEUI_CORE_APP_H
#define WHALEUI_CORE_APP_H

/* Application core - internal interface. */

#include "whaleui.h"

#include <vector>

/* SDL3 opaque types; keep pointers, query/mutate via SDL APIs (wrapped in
 * src/platform/ and src/render/). Never dereference these here. */
typedef struct SDL_GPUDevice SDL_GPUDevice;

struct whaleui_window;

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_app
{
    whaleui_theme_t theme;
    char accent[16];
    int max_fps;
    int battery_saver;
    int vsync;
    int running;

    /* SDL3 GPU device shared by all windows (created lazily on the first
     * window show, owned by the app, destroyed in whaleui_app_destroy). */
    SDL_GPUDevice* gpu;

    std::vector<whaleui_window_t*> windows;
};

/* resolved theme (SYSTEM -> platform detection); internal, used by window. */
whaleui_theme_t whaleui_app_resolved_theme(const whaleui_app_t* app);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_APP_H */
