#ifndef WHALEUI_CORE_WINDOW_H
#define WHALEUI_CORE_WINDOW_H

/* Window core - internal interface.
 *
 * The window owns the native SDL_Window* (created in step 3 via the platform
 * backend). All queries/mutations go through the window_* functions, which
 * wrap the SDL APIs; the struct only keeps pointers + cached state. */

#include "whaleui.h"

#include <string>

/* SDL3 opaque types; never dereferenced in this header. */
typedef struct SDL_Window SDL_Window;

/* render context for this window (src/render/render.h) */
struct whaleui_render;
typedef struct whaleui_render whaleui_render_t;

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_window
{
    whaleui_app_t* app;
    SDL_Window* sdl;       /* native window (NULL until step 3) */

    whaleui_render_t* render; /* per-window render context */

    /* cached state used before the native window exists; replaced by SDL
     * queries once sdl is set */
    char* title;
    int width;
    int height;
    int visible;

    whaleui_dom_document_t* document;

    /* URI the current document was loaded from ("" for load_html); the
     * base for resolving relative <a href> navigation (the demo's
     * test_html pages link to each other by bare relative names, which
     * must resolve against the page's own directory, not the cwd). */
    std::string base_uri;

    /* coalesced resize: a window drag floods SDL_EVENT_WINDOW_RESIZED (one
     * per frame of the drag); rebuilding GPU targets + relayout per event
     * is what makes the window feel frozen while resizing. The event only
     * records the target size here; the render worker applies it at most
     * once per kResizeCoalesceMs (app.cpp), so the window keeps responding
     * during the drag and settles ~80ms after it stops. */
    int resize_pending;
    int resize_w;
    int resize_h;
    unsigned long long resize_last; /* last time resize was actually applied */
};

/* Re-parse the stylesheet + rebuild theme variables (used after theme or
 * accent changes). Internal; declared here for core/app.cpp. */
void whaleui_window_refresh_css(whaleui_window_t* win);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_WINDOW_H */
