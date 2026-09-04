#ifndef WHALEUI_CORE_APP_H
#define WHALEUI_CORE_APP_H

/* Application core - internal interface. */

#include "whaleui.h"

#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <SDL3/SDL.h>

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
    std::atomic<int> running; /* event loop flag; QUIT/close clears it */
    /* Single-threaded event + render model (since 0.97): whaleui_app_run
     * handles SDL events and renders frames on the SAME thread, in one
     * loop, so input handling and rendering are serialized by construction
     * (no queue, no render lock, no cross-thread GPU swapchain handoff -
     * the old two-thread worker made every frame ~5x slower on the D3D12
     * backend once the swapchain had been presented from another thread).
     * The atomics/queue/cv fields below are retained (zero-cost, keep the
     * destroy paths and tests compiling) but no longer drive the loop:
     * `running` is the only live flag, `frames_alive` is set per frame so
     * the loop keeps animating windows alive and parks static ones. */
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

    /* --- retained two-thread fields (inert since 0.97, see above) --- */
    std::thread render_thread;
    std::atomic<int> frame_request{0};
    std::atomic<int> frame_done{0};
    std::atomic<int> frames_alive{0};
    int display_refresh;
    unsigned long long last_frame_tick; /* worker frame pacing */
    std::mutex render_lock;
    std::condition_variable frame_cv;
    std::deque<SDL_Event> input_queue;
};

/* resolved theme (SYSTEM -> platform detection); internal, used by window. */
whaleui_theme_t whaleui_app_resolved_theme(const whaleui_app_t* app);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_APP_H */
