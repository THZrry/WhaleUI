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
    /* Render worker thread model: whaleui_render_frame runs on a worker
     * that ALSO handles input (process_event) - input and rendering are
     * serialized on that thread, the main thread only polls SDL events
     * and pushes them under render_lock (a short queue critical section).
     * Rendering must live off the main thread so an animated page and a
     * live window drag keep updating while SDL_PollEvent is parked inside
     * the OS modal drag loop. The lock guards ONLY input_queue; the frame
     * pacing sleep happens outside it (holding it across the sleep made
     * the old worker block every event push for a frame - 25-45ms/frame).
     * `running` is the shutdown flag, `frames_alive` tells the main loop
     * an animation is running (keep polling), `frame_request` wakes the
     * worker (animation self-drive + the modal-drag event watch). */
    std::atomic<int> running; /* shutdown flag: QUIT/close/app_quit clear */
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

    /* render worker thread (see model comment above) */
    std::thread render_thread;
    std::atomic<int> frame_request{0}; /* wake worker: render now */
    std::atomic<int> frame_done{0};    /* worker -> main: frame finished */
    std::atomic<int> frames_alive{0};  /* an animation needs frames */
    int display_refresh;               /* Hz of the first shown window */
    unsigned long long last_frame_tick;
    std::mutex render_lock;            /* guards input_queue ONLY */
    std::condition_variable frame_cv;  /* worker waits on queue/request */
    std::deque<SDL_Event> input_queue; /* main posts, worker consumes */
};

/* resolved theme (SYSTEM -> platform detection); internal, used by window. */
whaleui_theme_t whaleui_app_resolved_theme(const whaleui_app_t* app);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_APP_H */
