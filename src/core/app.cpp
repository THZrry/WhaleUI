/* Application core: public C API implementation.
 * Event loop polls SDL events, repaints visible windows, honors the
 * max-fps / battery-saver frame cap. */

#include "core/app.h"
#include "core/window.h"
#include "dom/events.h"
#include "platform/platform.h"
#include "render/render.h"
#include "style/theme.h"

#include <lexbor/dom/dom.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstring>
#include <string>
#include <functional>
#include <mutex>
#include <vector>

/* SDL window/IME calls must run on the window's message thread (the main
 * thread here). The render worker posts them here; the main loop drains
 * the queue right after SDL_PollEvent. Lives OUTSIDE the anonymous
 * namespace so render.cpp/render_text.cpp can link against it. */
static std::mutex g_main_tasks_mx;
static std::vector<std::function<void()>> g_main_tasks;

extern "C" void whaleui_sdl_on_main(void (*fn)(void*), void* ud)
{
    std::lock_guard<std::mutex> lk(g_main_tasks_mx);
    g_main_tasks.push_back([fn, ud] { fn(ud); });
}

namespace {

/* dispatch a DOM event on the given element (no-op when NULL) */
void dom_dispatch(whaleui_dom_element_t* el, const char* type, int key,
                  int mx, int my, int button, float wx, float wy)
{
    if (el) {
        whaleui_dom_dispatch_event_full(
            reinterpret_cast<lxb_dom_element*>(el), type, key, mx, my, button,
            wx, wy);
    }
}
/* theme resolution: SYSTEM -> the OS scheme captured at app_create */
whaleui_theme_t resolved_theme(const whaleui_app_t* app)
{
    if (!app) {
        return WHALEUI_THEME_LIGHT;
    }
    return app->theme == WHALEUI_THEME_SYSTEM ? app->system_theme : app->theme;
}

/* resolve a possibly-relative <a href> against the window's base URI:
 * absolute URIs/paths pass through, relative names join the base's
 * directory (the page's own folder, not the process cwd - the demo's
 * test_html pages link to each other by bare file names). Empty base
 * falls back to the raw href (cwd-relative, the pre-navigation vfs
 * behavior). */
std::string resolve_href(whaleui_window_t* win, const char* href)
{
    std::string h = href ? href : "";
    if (h.empty() || win->base_uri.empty() ||
        h.rfind("file://", 0) == 0 || h.find(':') != std::string::npos ||
        h[0] == '/' || h[0] == '\\') {
        return h;
    }
    /* base directory: everything up to the last '/' (keep the slash) */
    std::string base = win->base_uri;
    size_t slash = base.find_last_of('/');
    if (slash == std::string::npos) {
        return h;
    }
    /* strip a trailing file name (base ends in "/index.html") */
    base = base.substr(0, slash + 1);
    return base + h;
}

/* window owning an SDL window id (used by key/text/wheel dispatch) */
whaleui_window_t* window_for(whaleui_app_t* app, SDL_WindowID id)
{
    for (whaleui_window_t* win : app->windows) {
        if (win->sdl && SDL_GetWindowID(win->sdl) == id) {
            return win;
        }
    }
    return nullptr;
}

/* process one SDL event on the worker thread. Runs under render_lock
 * (the worker holds it), so it is serialized with rendering - no data
 * race between input handlers and the render pass. */
void process_event(whaleui_app_t* app, const SDL_Event& e)
{
    switch (e.type) {
    case SDL_EVENT_QUIT:
        app->running = 0;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: {
        /* OS 焦点回到窗口时,若控件仍处于编辑态,重新开启文本输入。
         * 必须在主线程执行(IMM 绑定窗口线程),post 到主线程队列。 */
        whaleui_window_t* w = window_for(app, e.window.windowID);
        if (w && w->render && w->render->edit_el && w->sdl) {
            SDL_Window* sdlw = w->sdl;
            whaleui_sdl_on_main(
                [](void* p) { SDL_StartTextInput(static_cast<SDL_Window*>(p)); },
                sdlw);
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST: {
        /* 窗口失去焦点:编辑控件同步失焦 —— 清 edit 态、退出文本输入,
         * caret/选区消失(浏览器切窗即 blur)。SDL 调用回主线程执行。 */
        whaleui_window_t* w = window_for(app, e.window.windowID);
        if (w && w->render && w->render->edit_el) {
            w->render->focus_el = nullptr; /* :focus 样式随失焦移除 */
            w->render->edit_el = nullptr;
            w->render->compose.clear();
            w->render->compose_caret = -1;
            w->render->compose_flow_x = -1;
            w->render->compose_flow_y = -1;
            w->render->sel_anchor_el = w->render->sel_focus_el = nullptr;
            w->render->has_dirty = 1;
            if (w->sdl) {
                SDL_Window* sdlw = w->sdl;
                whaleui_sdl_on_main(
                    [](void* p) { SDL_StopTextInput(static_cast<SDL_Window*>(p)); },
                    sdlw);
            }
        }
        break;
    }
    case SDL_EVENT_KEY_DOWN: {
        whaleui_window_t* w = window_for(app, e.key.windowID);
        if (w && w->render) {
            whaleui_render_handle_key(w->render, static_cast<int>(e.key.key),
                                      1, static_cast<int>(e.key.mod));
            dom_dispatch(whaleui_render_focus_element(w->render),
                         "keydown", static_cast<int>(e.key.key),
                         0, 0, 0, 0, 0);
        }
        if (app->key_cb) {
            app->key_cb(app, static_cast<int>(e.key.key), 1, app->key_ud);
        }
        break;
    }
    case SDL_EVENT_KEY_UP: {
        whaleui_window_t* w = window_for(app, e.key.windowID);
        if (w && w->render) {
            whaleui_render_handle_key(w->render, static_cast<int>(e.key.key),
                                      0, static_cast<int>(e.key.mod));
            dom_dispatch(whaleui_render_focus_element(w->render),
                         "keyup", static_cast<int>(e.key.key),
                         0, 0, 0, 0, 0);
        }
        if (app->key_cb) {
            app->key_cb(app, static_cast<int>(e.key.key), 0, app->key_ud);
        }
        break;
    }
    case SDL_EVENT_TEXT_INPUT: {
        whaleui_window_t* w = window_for(app, e.text.windowID);
        if (w && w->render) {
            whaleui_render_handle_text(w->render, e.text.text);
        }
        break;
    }
    case SDL_EVENT_TEXT_EDITING: {
        whaleui_window_t* w = window_for(app, e.edit.windowID);
        if (w && w->render) {
            whaleui_render_handle_editing(w->render, e.edit.text,
                                          static_cast<int>(e.edit.start));
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        for (whaleui_window_t* win : app->windows) {
            if (win->render && SDL_GetWindowID(win->sdl) == e.wheel.windowID) {
                whaleui_render_handle_wheel(win->render,
                                            static_cast<int>(e.wheel.mouse_x),
                                            static_cast<int>(e.wheel.mouse_y),
                                            e.wheel.y);
                break;
            }
        }
        break;
    }
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        for (whaleui_window_t* win : app->windows) {
            if (win->sdl && SDL_GetWindowID(win->sdl) == e.window.windowID) {
                win->visible = 0;
                app->running = 0;
                break;
            }
        }
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESTORED:
        for (whaleui_window_t* win : app->windows) {
            if (win->render && SDL_GetWindowID(win->sdl) == e.window.windowID) {
                whaleui_render_invalidate(win->render);
                break;
            }
        }
        break;
    case SDL_EVENT_WINDOW_RESIZED: {
        for (whaleui_window_t* win : app->windows) {
            if (win->sdl && SDL_GetWindowID(win->sdl) == e.window.windowID) {
                win->width = e.window.data1;
                win->height = e.window.data2;
                /* coalesced resize: only record the target size here; the
                 * worker applies it at most once per 80ms so a window drag
                 * (one resize event per drag frame) does not rebuild the
                 * GPU targets + relayout on every event - that is what
                 * froze the window mid-resize. */
                win->resize_w = win->width;
                win->resize_h = win->height;
                win->resize_pending = 1;
                break;
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        for (whaleui_window_t* win : app->windows) {
            if (win->render && SDL_GetWindowID(win->sdl) == e.motion.windowID) {
                whaleui_render_set_hover(win->render,
                                         static_cast<int>(e.motion.x),
                                         static_cast<int>(e.motion.y));
                break;
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_LEFT) {
            for (whaleui_window_t* win : app->windows) {
                if (win->render && win->document &&
                    SDL_GetWindowID(win->sdl) == e.button.windowID) {
                    whaleui_render_set_pressed_ex(win->render,
                                                  static_cast<int>(e.button.x),
                                                  static_cast<int>(e.button.y),
                                                  1, static_cast<int>(e.button.clicks),
                                                  static_cast<int>(SDL_GetModState()));
                    const char* val = nullptr;
                    if (whaleui_render_handle_click(win->render,
                                                    e.button.x, e.button.y,
                                                    &val) &&
                        val && app->select_cb) {
                        app->select_cb(app, val, app->select_ud);
                    }
                    whaleui_dom_element_t* hit = whaleui_render_hit_element(
                        win->render, static_cast<int>(e.button.x),
                        static_cast<int>(e.button.y));
                    /* a successful navigation destroys the old document:
                     * `hit` (and the tree it came from) is gone, so the
                     * mousedown/click dispatch below must be skipped or it
                     * dereferences freed lexbor nodes (the click "freezes"
                     * - typically a garbage-pointer walk in the dispatch). */
                    bool navigated = false;
                    /* <a href> navigation: walk up from the hit to the
                     * nearest anchor, resolve the (possibly relative) href
                     * against the document's base URI and load it (the
                     * test_html pages navigate by bare relative names). */
                    {
                        lxb_dom_element* ael = hit ? reinterpret_cast<lxb_dom_element*>(hit) : nullptr;
                        while (ael) {
                            size_t alen = 0;
                            const lxb_char_t* aname =
                                lxb_dom_element_local_name(ael, &alen);
                            if (aname && alen == 1 && aname[0] == 'a') {
                                break;
                            }
                            lxb_dom_node* ap = ael->node.parent;
                            if (!ap || ap->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                                ael = nullptr;
                                break;
                            }
                            ael = lxb_dom_interface_element(ap);
                        }
                        if (ael) {
                            size_t hlen = 0;
                            const lxb_char_t* href = lxb_dom_element_get_attribute(
                                ael, (const lxb_char_t*)"href", 4, &hlen);
                            if (href && hlen > 0) {
                                std::string h(reinterpret_cast<const char*>(href),
                                              hlen);
                                if (h[0] == '#' && h.size() > 1) {
                                    /* page-internal anchor: scroll the
                                     * document to the target element, not a
                                     * navigation */
                                    whaleui_render_scroll_to_id(
                                        win->render, win->document,
                                        h.c_str() + 1);
                                    break;
                                }
                                std::string target = resolve_href(
                                    win, std::string(
                                             reinterpret_cast<const char*>(href),
                                             hlen).c_str());
                                if (!target.empty()) {
                                    if (whaleui_window_load_uri(
                                            win, target.c_str()) == 0) {
                                        navigated = true;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    if (!navigated) {
                        dom_dispatch(hit, "mousedown", 0,
                                     static_cast<int>(e.button.x),
                                     static_cast<int>(e.button.y),
                                     e.button.button, 0, 0);
                        dom_dispatch(hit, "click", 0,
                                     static_cast<int>(e.button.x),
                                     static_cast<int>(e.button.y),
                                     e.button.button, 0, 0);
                    }
                    break;
                }
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT) {
            for (whaleui_window_t* win : app->windows) {
                if (win->render && SDL_GetWindowID(win->sdl) == e.button.windowID) {
                    whaleui_render_set_pressed_ex(win->render,
                                                  static_cast<int>(e.button.x),
                                                  static_cast<int>(e.button.y),
                                                  0, 1,
                                                  static_cast<int>(SDL_GetModState()));
                    dom_dispatch(
                        whaleui_render_hit_element(
                            win->render, static_cast<int>(e.button.x),
                            static_cast<int>(e.button.y)),
                        "mouseup", 0, static_cast<int>(e.button.x),
                        static_cast<int>(e.button.y),
                        e.button.button, 0, 0);
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
}

/* resize coalescing interval (ms): during a window drag the worker applies
 * the latest recorded size at most this often, so the GPU targets are
 * rebuilt a few times per second instead of once per drag frame. */
static const unsigned long long kResizeCoalesceMs = 80;

/* render worker thread: waits on the input queue, then processes all
 * queued events and renders, all under render_lock. Input handling and
 * rendering are serialized on this thread, so they never race - the main
 * thread only posts events (no render-state access) and stays responsive
 * even while a frame renders (a slow ~27ms frame no longer blocks the
 * UI). */
void render_worker_fn(whaleui_app_t* app)
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(app->render_lock);
            app->frame_cv.wait(lk, [&] {
                return !app->input_queue.empty() ||
                       app->frame_request.load() || !app->running.load();
            });
            if (!app->running.load() && app->input_queue.empty()) {
                return;
            }
            while (!app->input_queue.empty()) {
                SDL_Event e = app->input_queue.front();
                app->input_queue.pop_front();
                process_event(app, e);
            }
            /* apply coalesced resizes: at most once per interval. A pending
             * resize keeps the frame alive so the loop below renders at the
             * new size even without further events. */
            const Uint64 now = SDL_GetTicks();
            for (whaleui_window_t* win : app->windows) {
                if (win->resize_pending && win->render &&
                    (now - win->resize_last >= kResizeCoalesceMs)) {
                    whaleui_render_resize(win->render, win->resize_w,
                                          win->resize_h);
                    /* keep the window's own size in sync (the worker path
                     * bypasses whaleui_window_set_size) and re-filter the
                     * @media rules against the NEW viewport width - without
                     * this a resized window kept the rules filtered at load
                     * time and never re-laid out responsively ("改变窗口
                     * 大小不会响应式布局": @media max-width / vw stayed
                     * stuck at the original size). */
                    win->width = win->resize_w;
                    win->height = win->resize_h;
                    whaleui_window_refresh_css(win);
                    win->resize_last = now;
                    win->resize_pending = 0;
                }
            }
            if (app->frame_request.load()) {
                int alive = 0;
                for (whaleui_window_t* win : app->windows) {
                    if (win->visible && win->sdl && win->render && win->document) {
                        if (SDL_GetWindowFlags(win->sdl) & SDL_WINDOW_MINIMIZED) {
                            continue;
                        }
                        whaleui_render_frame(win->render, win->document);
                        alive |= win->render->alive;
                    }
                }
                app->frames_alive.store(alive);
                app->frame_done.store(1);
                app->frame_request.store(0);
                /* pace to the display refresh rate OUTSIDE the lock (the
                 * main thread can still push events): the D3D12 backend's
                 * VSYNC present mode does not wait for vblank, so without
                 * this an uncapped animation ran at ~186fps. 60Hz display
                 * -> 60fps, 144Hz -> 144fps (not a hard 60 cap). */
                Uint64 finterval = app->display_refresh > 0
                                       ? 1000u / static_cast<Uint64>(
                                                     app->display_refresh)
                                       : 16;
                Uint64 fnow = SDL_GetTicks();
                Uint64 fnext = app->last_frame_tick + finterval;
                if (fnow < fnext) {
                    SDL_Delay(static_cast<Uint32>(fnext - fnow));
                    fnow = fnext;
                }
                app->last_frame_tick = fnow;
            }
        }
    }
}
} // namespace

whaleui_theme_t whaleui_app_resolved_theme(const whaleui_app_t* app)
{
    return resolved_theme(app);
}

extern "C" whaleui_app_t* whaleui_app_create(void)
{
    if (whaleui_platform_init() != 0) {
        return nullptr;
    }
    whaleui_app_t* app = new whaleui_app_t;
    app->theme = WHALEUI_THEME_SYSTEM;
    /* capture the OS scheme once at init (defaults to light when the
     * platform cannot report one, e.g. older systems) */
    app->system_theme = whaleui_platform_system_theme();
    std::strcpy(app->theme_style, "fluent");
    /* no default accent: an empty accent means each theme uses its own
     * default (material #6750a4, aero #0078d7, ...). Only an explicit
     * whaleui_app_set_accent_color call feeds --accent. */
    app->accent[0] = '\0';
    app->max_fps = 0;
    app->async_layout = 0; /* async first layout: opt-in only */
    /* battery saver is a *default*, gated on the actual system power state:
     * 60fps (and FSR, see fsr_want_active) while on battery, uncapped while
     * plugged in. The state is re-polled in the event loop, so unplugging
     * throttles the app without user intervention. */
    app->battery_saver = 1;
    app->on_battery =
        (SDL_GetPowerInfo(nullptr, nullptr) == SDL_POWERSTATE_ON_BATTERY) ? 1 : 0;
    app->power_check_ticks = 0;
    app->vsync = 1;
    app->display_refresh = 60; /* refined at window show */
    app->last_frame_tick = 0;
    app->running = 0;
    /* default is FULL motion (animated pages play their CSS animations).
     * The no-JS tradeoff is documented: a reveal-on-scroll page (opacity:0
     * until JS adds .in) would keep its content hidden without a JS engine,
     * so apps can set reduced motion (whaleui_app_set_reduced_motion(app,1))
     * to surface that content - but that also stops the page's decorative
     * @keyframes animations. Animated content wins by default. */
    app->reduced_motion = 0;
    app->gpu = nullptr;
    app->select_cb = nullptr;
    app->select_ud = nullptr;
    return app;
}

extern "C" void whaleui_app_destroy(whaleui_app_t* app)
{
    if (!app) {
        return;
    }
    app->running = 0;
    app->frame_request.store(1);
    app->frame_cv.notify_one();
    if (app->render_thread.joinable()) {
        app->render_thread.join();
    }
    for (whaleui_window_t* win : app->windows) {
        whaleui_window_destroy(win);
    }
    app->windows.clear();
    if (app->gpu) {
        SDL_DestroyGPUDevice(app->gpu);
    }
    whaleui_platform_shutdown();
    delete app;
}

extern "C" int whaleui_app_run(whaleui_app_t* app)
{
    if (!app) {
        return -1;
    }
    app->running = 1;

    /* nothing visible -> nothing to run (keeps headless tests fast) */
    bool any = false;
    for (whaleui_window_t* win : app->windows) {
        if (win->visible && win->sdl) {
            any = true;
            break;
        }
    }
    if (!any) {
        app->running = 0;
        return 0;
    }

    Uint64 last = SDL_GetTicks();
    app->render_thread = std::thread(render_worker_fn, app);
    /* force the first frame: with no events and no animation there is
     * nothing to wake the worker otherwise (the idle loop parks). */
    app->frame_request.store(1);
    app->frame_cv.notify_one();
    while (app->running) {
        /* post events to the worker; it processes input and renders
         * serially under render_lock, so input and rendering never
         * race on the shared render state. */
        bool work = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            {
                std::lock_guard<std::mutex> lk(app->render_lock);
                app->input_queue.push_back(e);
            }
            work = true;
        }

        /* keep continuous frames while the worker says something is still
         * moving (animation, caret); otherwise the loop parks below. */
        if (!work && app->frames_alive.load()) {
            work = true;
        }

        /* hand rendering to the worker thread: the event loop keeps polling
         * input while a frame renders (a slow ~27ms frame no longer blocks
         * the UI). The worker takes render_lock for the whole frame. */
        if (work) {
            app->frame_request.store(1);
            app->frame_cv.notify_one();
        }
        /* run SDL window/IME tasks posted by the worker on the MAIN thread:
         * SDL_StartTextInput/StopTextInput/SetTextInputArea touch the
         * Windows IMM/TSF context, which is bound to the window's message
         * thread. Called from the render worker they leave the IME in a
         * half-activated state (cannot switch/enable input until the window
         * loses+regains focus) and can crash on later input events. */
        {
            std::vector<std::function<void()>> todo;
            {
                std::lock_guard<std::mutex> lk(g_main_tasks_mx);
                todo.swap(g_main_tasks);
            }
            for (auto& f : todo) {
                f();
            }
        }

        /* power-state poll (~2s): unplugging throttles the loop to the
         * battery-saver cap, plugging back in uncaps it. SDL3 3.4 has no
         * power-change event to listen for, so poll cheaply (one system
         * call; 2s is plenty - battery transitions are not time-critical). */
        Uint64 now = SDL_GetTicks();
        if (now - app->power_check_ticks > 2000) {
            app->power_check_ticks = now;
            app->on_battery =
                (SDL_GetPowerInfo(nullptr, nullptr) == SDL_POWERSTATE_ON_BATTERY) ? 1 : 0;
        }

        if (work) {
            /* frame cap: max_fps wins; default 60 on battery and AC alike.
             * The renderer's per-frame cost times the frame rate is the
             * CPU bill, and on a 240Hz display an uncapped animation loop
             * runs at ~130fps burning 65% of a core for little visual
             * gain - power-first: 60fps by default, raise via max_fps
             * (e.g. 144 on a high-refresh display that can afford it). */
            int fps = app->max_fps > 0 ? app->max_fps : 60;
            if (fps > 0) {
                Uint64 target = last + 1000 / fps;
                if (now < target) {
                    SDL_Delay(static_cast<Uint32>(target - now));
                    now = target;
                }
            } else {
                /* uncapped: don't busy-spin while the worker is throttled
                 * to the display refresh - wait up to one frame period for
                 * events (latency <= a frame, negligible) instead of a 1ms
                 * poll. */
                int wait = app->display_refresh > 0
                               ? 1000 / app->display_refresh
                               : 16;
                if (wait < 1) {
                    wait = 1;
                }
                if (SDL_WaitEventTimeout(&e, wait)) {
                    std::lock_guard<std::mutex> lk(app->render_lock);
                    app->input_queue.push_back(e);
                }
            }
            last = now;
        } else {
            /* idle: park until an event arrives. The timeout also wakes us
             * for the 2s power poll and for a pending coalesced resize
             * (which otherwise has no event to trigger its application). */
            Uint32 timeout = 100;
            for (whaleui_window_t* win : app->windows) {
                if (win->resize_pending && win->render) {
                    Uint64 due = win->resize_last + kResizeCoalesceMs;
                    if (now >= due) {
                        timeout = 0;
                        break;
                    }
                    Uint32 rem = static_cast<Uint32>(due - now);
                    if (rem < timeout) {
                        timeout = rem;
                    }
                }
            }
            if (SDL_WaitEventTimeout(&e, static_cast<int>(timeout))) {
                {
                    std::lock_guard<std::mutex> lk(app->render_lock);
                    app->input_queue.push_back(e);
                }
                app->frame_request.store(1);
                app->frame_cv.notify_one();
            } else if (timeout == 0) {
                /* resize due: wake the worker to apply it */
                app->frame_request.store(1);
                app->frame_cv.notify_one();
            }
            last = SDL_GetTicks();
        }
    }
    app->running = 0;
    app->frame_request.store(1);
    app->frame_cv.notify_one();
    app->render_thread.join();
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
    /* re-parse the stylesheet with the new theme variables, then repaint */
    for (whaleui_window_t* win : app->windows) {
        if (win->render) {
            whaleui_window_refresh_css(win);
        }
    }
    return 0;
}

extern "C" int whaleui_app_set_theme_style(whaleui_app_t* app, const char* style)
{
    if (!app || !style) {
        return -1;
    }
    const char* resolved = whaleui_theme_resolve(style);
    std::strncpy(app->theme_style, resolved, sizeof(app->theme_style) - 1);
    app->theme_style[sizeof(app->theme_style) - 1] = '\0';
    for (whaleui_window_t* win : app->windows) {
        if (win->render) {
            whaleui_window_refresh_css(win);
        }
    }
    return 0;
}

extern "C" const char* whaleui_app_get_theme_style(const whaleui_app_t* app)
{
    return app ? app->theme_style : "fluent";
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
    /* accent feeds --accent; refresh windows to pick it up */
    for (whaleui_window_t* win : app->windows) {
        if (win->render) {
            whaleui_window_refresh_css(win);
        }
    }
    return 0;
}

extern "C" int whaleui_app_set_key_callback(whaleui_app_t* app,
                                            whaleui_key_cb cb, void* userdata)
{
    if (!app) {
        return -1;
    }
    app->key_cb = cb;
    app->key_ud = userdata;
    return 0;
}

extern "C" int whaleui_app_set_select_callback(whaleui_app_t* app,
                                               whaleui_select_cb cb, void* userdata)
{
    if (!app) {
        return -1;
    }
    app->select_cb = cb;
    app->select_ud = userdata;
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
    case WHALEUI_RENDER_VSYNC:
        app->vsync = value ? 1 : 0;
        /* re-apply the present mode to every claimed window */
        for (whaleui_window_t* win : app->windows) {
            if (win->sdl && app->gpu) {
                SDL_SetGPUSwapchainParameters(
                    app->gpu, win->sdl, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                    app->vsync ? SDL_GPU_PRESENTMODE_VSYNC
                               : SDL_GPU_PRESENTMODE_IMMEDIATE);
            }
        }
        break;
    case WHALEUI_RENDER_ASYNC_LAYOUT:
        app->async_layout = value ? 1 : 0;
        /* nothing to re-apply: each window reads it on its first frame */
        break;
    default: return -1;
    }
    return 0;
}

extern "C" int whaleui_app_set_text_scale(whaleui_app_t* app, float scale)
{
    if (!app || scale <= 0.0f || scale > 4.0f) {
        return -1;
    }
    for (whaleui_window_t* win : app->windows) {
        if (win->render) {
            /* applied by the frame IN PLACE on the existing tree (font
             * scale + geometry reflow) - NOT a whole-tree rebuild */
            std::lock_guard<std::recursive_mutex> lk(win->render->tree_mx);
            win->render->text_scale = scale;
            win->render->font_scale_pending = 1;
        }
    }
    return 0;
}

extern "C" int whaleui_app_set_reduced_motion(whaleui_app_t* app, int reduce)
{
    if (!app) {
        return -1;
    }
    app->reduced_motion = reduce ? 1 : 0;
    for (whaleui_window_t* win : app->windows) {
        whaleui_window_refresh_css(win);
    }
    return 0;
}





















