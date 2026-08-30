#ifndef WHALEUI_CORE_APP_H
#define WHALEUI_CORE_APP_H

/* Application core - internal interface. */

#include "whaleui.h"

#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

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
    whaleui_theme_t system_theme; /* OS scheme captured at app_create */
    char theme_style[24];
    char accent[16];
    int max_fps;
    int battery_saver;
    int vsync;
    int running;
    /* async first layout (WHALEUI_RENDER_ASYNC_LAYOUT): the initial full
     * layout of each window runs on a worker thread so a large page does
     * not freeze the window while it lays out. Off by default - the frame
     * contract is synchronous. */
    int async_layout;
    int reduced_motion; /* prefers-reduced-motion: reduce */
    /* system power state (refreshed from SDL_GetPowerInfo in the event
     * loop): drives the battery-saver default - 60fps + FSR on battery,
     * uncapped on AC power */
    int on_battery;
    unsigned long long power_check_ticks; /* last power-state poll */

    /* SDL3 GPU device shared by all windows (created lazily on the first
     * window show, owned by the app, destroyed in whaleui_app_destroy). */
    SDL_GPUDevice* gpu;

    /* <select> change callback */
    whaleui_select_cb select_cb;
    void* select_ud;

    /* key events (library dispatches; the app decides what to do) */
    whaleui_key_cb key_cb;
    void* key_ud;

    std::vector<whaleui_window_t*> windows;

    /* render worker thread: whaleui_render_frame runs here so a slow frame
     * never blocks the event loop (input stays responsive). The main thread
     * polls events, mutates render state, and kicks the worker; the worker
     * renders and presents. state is guarded by render_lock (short locks on
     * the main thread, the worker takes it for the whole frame). */
    std::thread render_thread;
    std::atomic<int> frame_request{0}; /* main -> worker: render now */
    std::atomic<int> frame_done{0};    /* worker -> main: frame finished */
    std::mutex render_lock;
};

/* resolved theme (SYSTEM -> platform detection); internal, used by window. */
whaleui_theme_t whaleui_app_resolved_theme(const whaleui_app_t* app);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_APP_H */
