/* Timers: setTimeout / setInterval / requestAnimationFrame.
 *
 * The browser's timer queue maps onto the engine's event loop. Timers live in
 * a global registry guarded by a mutex (they may be created from application
 * code before app_run, or from inside a DOM listener on the worker thread),
 * and are pumped on the loop thread - the same thread that renders and
 * dispatches DOM events - so a callback may touch the DOM directly.
 *
 * The loop asks for the next due time so it can sleep exactly that long
 * (whaleui_timers_next_due_ms), and keeps running while any timer is live
 * (whaleui_timers_pending): a pending setTimeout is enough to keep the app
 * awake, like the browser.
 *
 * requestAnimationFrame is separate: those callbacks run once per RENDERED
 * frame (after the frame's paint), not on a wall clock, and also keep the
 * loop alive until they fire. */

#include "core/timers.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace {

enum TimerKind
{
    kTimerTimeout = 0,
    kTimerInterval,
    kTimerFrame
};

struct Timer
{
    long long id;
    TimerKind kind;
    whaleui_timer_cb cb;      /* timeout/interval */
    whaleui_frame_cb fcb;     /* frame */
    void* userdata;
    double due_ms;            /* next fire time (whaleui_now domain) */
    double interval_ms;       /* 0 for one-shot / frame */
    bool dead;
};

std::mutex& lock()
{
    static std::mutex m;
    return m;
}

std::vector<Timer>& timers()
{
    static std::vector<Timer> t;
    return t;
}

long long& next_id()
{
    static long long n = 1;
    return n;
}

/* monotonic clock origin (first call pins it; SDL_GetTicks is monotonic) */
unsigned long long& origin_ticks()
{
    static unsigned long long o = 0;
    if (o == 0) {
        o = SDL_GetTicks();
    }
    return o;
}

} // namespace

extern "C" double whaleui_now(void)
{
    unsigned long long now = SDL_GetTicks();
    unsigned long long o = origin_ticks();
    return static_cast<double>(now - o);
}

static long long add_timer(TimerKind kind, whaleui_timer_cb cb,
                           whaleui_frame_cb fcb, void* userdata,
                           double delay_ms, double interval_ms)
{
    std::lock_guard<std::mutex> lk(lock());
    if (kind != kTimerFrame && !cb) {
        return 0;
    }
    if (kind == kTimerFrame && !fcb) {
        return 0;
    }
    if (delay_ms < 0.0) {
        delay_ms = 0.0;
    }
    Timer t;
    t.id = next_id()++;
    t.kind = kind;
    t.cb = cb;
    t.fcb = fcb;
    t.userdata = userdata;
    t.due_ms = whaleui_now() + delay_ms;
    t.interval_ms = interval_ms;
    t.dead = false;
    timers().push_back(t);
    return t.id;
}

extern "C" long long whaleui_set_timeout(whaleui_timer_cb cb, void* userdata,
                                         double delay_ms)
{
    return add_timer(kTimerTimeout, cb, nullptr, userdata, delay_ms, 0.0);
}

extern "C" long long whaleui_set_interval(whaleui_timer_cb cb, void* userdata,
                                          double interval_ms)
{
    if (interval_ms < 0.0) {
        interval_ms = 0.0;
    }
    return add_timer(kTimerInterval, cb, nullptr, userdata, interval_ms,
                     interval_ms);
}

/* clearTimeout/clearInterval accept either id (browser behavior) */
static void clear_any(long long id)
{
    if (id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(lock());
    for (size_t i = 0; i < timers().size(); ++i) {
        if (timers()[i].id == id) {
            timers()[i].dead = true;
            break;
        }
    }
}

extern "C" void whaleui_clear_timeout(long long id)
{
    clear_any(id);
}

extern "C" void whaleui_clear_interval(long long id)
{
    clear_any(id);
}

extern "C" long long whaleui_request_animation_frame(whaleui_frame_cb cb,
                                                     void* userdata)
{
    return add_timer(kTimerFrame, nullptr, cb, userdata, 0.0, 0.0);
}

extern "C" void whaleui_cancel_animation_frame(long long id)
{
    clear_any(id);
}

extern "C" int whaleui_timer_count(void)
{
    std::lock_guard<std::mutex> lk(lock());
    int n = 0;
    for (size_t i = 0; i < timers().size(); ++i) {
        if (!timers()[i].dead) {
            ++n;
        }
    }
    return n;
}

extern "C" int whaleui_animation_frame_count(void)
{
    std::lock_guard<std::mutex> lk(lock());
    int n = 0;
    for (size_t i = 0; i < timers().size(); ++i) {
        if (!timers()[i].dead && timers()[i].kind == kTimerFrame) {
            ++n;
        }
    }
    return n;
}

/* Is any timer live? The loop stays awake while one is (a pending setTimeout
 * must keep the app running, like the browser). */
extern "C" int whaleui_timers_pending(void)
{
    return whaleui_timer_count() > 0 ? 1 : 0;
}

/* Milliseconds until the next due TIMEOUT/INTERVAL (frame callbacks have no
 * wall-clock deadline), or -1 when none is pending. The loop uses this as its
 * wait timeout. */
extern "C" double whaleui_timers_next_due_ms(void)
{
    std::lock_guard<std::mutex> lk(lock());
    double now = whaleui_now();
    double best = -1.0;
    for (size_t i = 0; i < timers().size(); ++i) {
        const Timer& t = timers()[i];
        if (t.dead || t.kind == kTimerFrame) {
            continue;
        }
        double d = t.due_ms - now;
        if (d < 0.0) {
            d = 0.0;
        }
        if (best < 0.0 || d < best) {
            best = d;
        }
    }
    return best;
}

/* Fire every due timeout/interval. Runs on the loop thread; callbacks are
 * invoked OUTSIDE the lock so a callback may create/clear timers. Returns the
 * number fired. */
extern "C" int whaleui_timers_pump(void)
{
    struct Fire
    {
        whaleui_timer_cb cb;
        void* userdata;
        double ts;
    };
    std::vector<Fire> fire;
    {
        std::lock_guard<std::mutex> lk(lock());
        double now = whaleui_now();
        std::vector<Timer> keep;
        keep.reserve(timers().size());
        for (size_t i = 0; i < timers().size(); ++i) {
            Timer t = timers()[i];
            if (t.dead) {
                continue; /* cleared: drop */
            }
            if (t.kind == kTimerFrame || now < t.due_ms) {
                keep.push_back(t);
                continue;
            }
            Fire f;
            f.cb = t.cb;
            f.userdata = t.userdata;
            f.ts = now;
            fire.push_back(f);
            if (t.kind == kTimerInterval) {
                /* reschedule from the due time, not from now, so a slow
                 * callback does not drift the interval */
                t.due_ms += t.interval_ms > 0.0 ? t.interval_ms : 0.0;
                if (t.due_ms <= now && t.interval_ms <= 0.0) {
                    t.due_ms = now; /* 0ms interval: once per pump */
                }
                keep.push_back(t);
            }
            /* a one-shot timeout is dropped here */
        }
        timers().swap(keep);
    }
    for (size_t i = 0; i < fire.size(); ++i) {
        if (fire[i].cb) {
            fire[i].cb(fire[i].ts, fire[i].userdata);
        }
    }
    return static_cast<int>(fire.size());
}

/* Fire the pending requestAnimationFrame callbacks (once per rendered frame).
 * Callbacks may re-request a frame. Returns the number fired. */
extern "C" int whaleui_animation_frames_pump(void)
{
    struct Fire
    {
        whaleui_frame_cb cb;
        void* userdata;
        double ts;
    };
    std::vector<Fire> fire;
    {
        std::lock_guard<std::mutex> lk(lock());
        double now = whaleui_now();
        std::vector<Timer> keep;
        keep.reserve(timers().size());
        for (size_t i = 0; i < timers().size(); ++i) {
            Timer t = timers()[i];
            if (t.dead) {
                continue;
            }
            if (t.kind != kTimerFrame) {
                keep.push_back(t);
                continue;
            }
            Fire f;
            f.cb = t.fcb;
            f.userdata = t.userdata;
            f.ts = now;
            fire.push_back(f);
            /* one-shot: re-requesting is the caller's job (like the browser) */
        }
        timers().swap(keep);
    }
    for (size_t i = 0; i < fire.size(); ++i) {
        if (fire[i].cb) {
            fire[i].cb(fire[i].ts, fire[i].userdata);
        }
    }
    return static_cast<int>(fire.size());
}
