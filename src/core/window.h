#ifndef WHALEUI_CORE_WINDOW_H
#define WHALEUI_CORE_WINDOW_H

/* Window core - internal interface.
 *
 * The window owns the native SDL_Window* (created in step 3 via the platform
 * backend). All queries/mutations go through the window_* functions, which
 * wrap the SDL APIs; the struct only keeps pointers + cached state. */

#include "whaleui.h"

#include <atomic>
#include <string>

/* HitTest types are declared in whaleui.h (single source of truth). */
typedef whaleui_hit_test_result_t (*whaleui_hit_test_fn)(whaleui_window_t* win, int x, int y, void* userdata);

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
    /* byte size of the loaded document: the async-first-layout policy (see
         * app->async_layout -1) needs it, but the render context does not exist
         * yet at load time (it is created by window_show) */
    size_t doc_bytes;
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
    /* event-watch slots (written by the SDL event watch with NO lock -
     * the watch runs inside SDL's own event lock, taking render_lock there
     * could deadlock against the worker holding it while calling SDL).
     * The worker folds watch_seq changes into resize_pending. */
    std::atomic<int> watch_seq{0};
    std::atomic<int> watch_w{0};
    std::atomic<int> watch_h{0};
    /* last watch_seq the worker folded (worker-thread only) */
    int watch_seq_last = 0;
    int resize_h;
    unsigned long long resize_last; /* last time resize was actually applied */

    /* coalesced mouse motion: a fast mouse produces dozens of MOTION events
     * per frame, and each one used to run whaleui_render_set_hover - a
     * whole-tree hit_test plus an SDL_SetCursor system call. On a large page
     * the queue drained slower than the mouse moved, so :hover only caught
     * up ~1s later (and often looked like it reacted when the pointer had
     * already left). The event now only records the newest position; the
     * worker runs the hit-test ONCE per frame with it. */
    int mouse_pending;
    float mouse_x;
    float mouse_y;
    /* occlusion state (SDL_EVENT_WINDOW_OCCLUDED / EXPOSED): a fully covered
     * window is fully parked (no tick/paint/present) until the next exposure.
     * Set/cleared by the event path in app.cpp; read by the worker's
     * per-window render gate. */
    int occluded;
    /* last background-throttle paint timestamp, ms (worker-thread only):
     * unfocused multi-window siblings paint at most every ~33ms (30fps) so
     * a background demo stays smooth while costing ~half the paint rate. */
    unsigned long long bg_last;
    /* coalesced wheel: same pattern as motion (a flick delivers several
     * WHEEL events per frame; each one scrolled + repainted separately, so
     * frames queued behind the live offset and the content trailed). The
     * worker applies the accumulated ticks once per wake-up. */
    int wheel_pending;
    float wheel_acc;
    float wheel_acc_x;
    float wheel_acc_y;

    /* --- appearance / constraints cache (SDL is source of truth when sdl != NULL) --- */
    int min_w;
    int min_h;
    int max_w;
    int max_h;
    int bordered;       /* 1 = bordered (default) */
    int resizable;      /* 1 = resizable (default) */
    int always_on_top;
    float opacity;      /* 1.0 default */
    int mouse_grab;
    int keyboard_grab;
    int focusable;      /* 1 = focusable (default) */
    whaleui_fullscreen_mode_t fullscreen;

    /* event callback (per-window, not app-global) */
    whaleui_window_event_cb win_event_cb;
    void* win_event_ud;

    /* per-window input callbacks (override app-global when set) */
    whaleui_key_cb key_cb;
    void* key_ud;
    whaleui_select_cb select_cb;
    void* select_ud;

    /* HitTest (borderless drag/resize) */
    void* hit_test_ud;
    int hit_test_active;
    /* per-window display refresh (Hz), refined at show() */
    int display_refresh;
};

/* Re-parse the stylesheet + rebuild theme variables (used after theme or
 * accent changes). Internal; declared here for core/app.cpp. */
void whaleui_window_refresh_css(whaleui_window_t* win);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_WINDOW_H */
