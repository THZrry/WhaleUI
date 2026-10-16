// test_web: web-platform APIs — clipboard, timers/rAF, visual viewport,
// Web Audio placeholder, gamepad, and the richer event payloads.
#include "whaleui.h"
#include "dom/events.h" /* internal full-payload dispatch */
#include "core/clipboard.h"
#include "core/timers.h"

#include <cassert>
#include <cstring>
#include <string>

namespace {

/* --- timer probes --- */
int g_timeout_calls = 0;
double g_timeout_ts = -1.0;
void on_timeout(double ts, void* ud) {
    ++g_timeout_calls;
    g_timeout_ts = ts;
    (void)ud;
}

int g_interval_calls = 0;
long long g_interval_id = 0;
void on_interval(double ts, void* ud) {
    (void)ts;
    (void)ud;
    ++g_interval_calls;
    if (g_interval_calls >= 3) {
        whaleui_clear_interval(g_interval_id); /* self-clearing like the browser */
    }
}

int g_raf_calls = 0;
void on_frame(double ts, void* ud) {
    (void)ts;
    (void)ud;
    ++g_raf_calls;
}

/* --- event payload probe --- */
struct Payload {
    static int mods;
    static int ctrl, shift, alt, meta;
    static std::string key, code;
    static int repeat, detail;
    static float mvx, mvy;
    static whaleui_dom_element_t* related;
    static std::string clip;
    static void on(whaleui_dom_event_t* ev, void* ud) {
        (void)ud;
        mods = whaleui_dom_event_mods(ev);
        ctrl = whaleui_dom_event_ctrl_key(ev);
        shift = whaleui_dom_event_shift_key(ev);
        alt = whaleui_dom_event_alt_key(ev);
        meta = whaleui_dom_event_meta_key(ev);
        const char* k = whaleui_dom_event_key(ev);
        key = k ? k : "";
        const char* c = whaleui_dom_event_code(ev);
        code = c ? c : "";
        repeat = whaleui_dom_event_repeat(ev);
        detail = whaleui_dom_event_detail(ev);
        mvx = whaleui_dom_event_movement_x(ev);
        mvy = whaleui_dom_event_movement_y(ev);
        related = whaleui_dom_event_related_target(ev);
        const char* ct = whaleui_dom_event_clipboard_text(ev);
        clip = ct ? ct : "";
    }
};
int Payload::mods = 0;
int Payload::ctrl = 0, Payload::shift = 0, Payload::alt = 0, Payload::meta = 0;
std::string Payload::key, Payload::code;
int Payload::repeat = 0, Payload::detail = 0;
float Payload::mvx = 0, Payload::mvy = 0;
whaleui_dom_element_t* Payload::related = nullptr;
std::string Payload::clip;

/* --- clipboard async probe --- */
std::string g_clip_cb_text;
int g_clip_cb_calls = 0;
void on_clip(const char* text, void* ud) {
    (void)ud;
    ++g_clip_cb_calls;
    g_clip_cb_text = text ? text : "";
}

} // namespace

int main(void)
{
    /* SDL must be initialized before the clipboard / gamepad subsystems can
     * be used, so the app comes first (its create() does SDL_Init). */
    whaleui_app_t* app = whaleui_app_create();
    assert(app != nullptr);

    /* ---- clipboard (sync + async queue) ---- */
    {
        assert(whaleui_clipboard_set_text("whaleui-clip") == 0);
        assert(whaleui_clipboard_has_text() == 1);
        assert(std::strcmp(whaleui_clipboard_get_text(), "whaleui-clip") == 0);
        /* null rejected */
        assert(whaleui_clipboard_set_text(nullptr) != 0);
        /* async write then read: callbacks are queued, not inline */
        g_clip_cb_calls = 0;
        g_clip_cb_text.clear();
        assert(whaleui_clipboard_write_text_async("async-text", on_clip,
                                                  nullptr) == 0);
        assert(g_clip_cb_calls == 0); /* not inline: runs on the pump */
        assert(whaleui_clipboard_read_text_async(on_clip, nullptr) == 0);
        assert(g_clip_cb_calls == 0);
        whaleui_clipboard_pump(); /* the loop would call this each iteration */
        assert(g_clip_cb_calls == 2);
        assert(g_clip_cb_text == "async-text"); /* read saw the write */
        assert(whaleui_clipboard_read_text_async(nullptr, nullptr) != 0);
        assert(whaleui_clipboard_write_text_async(nullptr, on_clip, nullptr) != 0);
    }

    /* ---- timers ---- */
    {
        assert(whaleui_timer_count() == 0);
        /* now() is monotonic and non-negative */
        double t0 = whaleui_now();
        assert(t0 >= 0.0);

        long long to = whaleui_set_timeout(on_timeout, nullptr, 0.0);
        assert(to > 0);
        assert(whaleui_timer_count() == 1);
        assert(whaleui_timers_pending() == 1);
        /* due immediately */
        assert(whaleui_timers_next_due_ms() == 0.0);
        assert(whaleui_timers_pump() == 1);
        assert(g_timeout_calls == 1);
        assert(g_timeout_ts >= 0.0);
        /* a one-shot is gone after firing */
        assert(whaleui_timer_count() == 0);
        assert(whaleui_timers_pump() == 0);

        /* a cleared timeout never fires */
        long long to2 = whaleui_set_timeout(on_timeout, nullptr, 0.0);
        assert(to2 > 0);
        whaleui_clear_timeout(to2);
        assert(whaleui_timers_pump() == 0);
        assert(g_timeout_calls == 1);
        assert(whaleui_timer_count() == 0);

        /* an interval fires repeatedly until cleared */
        g_interval_calls = 0;
        g_interval_id = whaleui_set_interval(on_interval, nullptr, 0.0);
        assert(g_interval_id > 0);
        assert(whaleui_timers_pump() == 1);
        assert(g_interval_calls == 1);
        assert(whaleui_timers_pump() == 1);
        assert(g_interval_calls == 2);
        assert(whaleui_timers_pump() == 1); /* third call clears itself */
        assert(g_interval_calls == 3);
        assert(whaleui_timer_count() == 0);
        assert(whaleui_timers_pump() == 0);

        /* clearTimeout also clears an interval (browser behavior) */
        g_interval_calls = 0;
        long long iv2 = whaleui_set_interval(on_interval, nullptr, 100000.0);
        assert(iv2 > 0);
        whaleui_clear_timeout(iv2);
        assert(whaleui_timer_count() == 0);

        /* a future timeout reports its remaining delay */
        long long far = whaleui_set_timeout(on_timeout, nullptr, 60000.0);
        assert(far > 0);
        double due = whaleui_timers_next_due_ms();
        assert(due > 50000.0 && due <= 60000.0);
        whaleui_clear_timeout(far);
        assert(whaleui_timers_next_due_ms() < 0.0); /* none pending */

        /* invalid ids are ignored */
        whaleui_clear_timeout(0);
        whaleui_clear_timeout(-5);
        whaleui_clear_interval(0);
    }

    /* ---- requestAnimationFrame ---- */
    {
        assert(whaleui_animation_frame_count() == 0);
        long long f1 = whaleui_request_animation_frame(on_frame, nullptr);
        long long f2 = whaleui_request_animation_frame(on_frame, nullptr);
        assert(f1 > 0 && f2 > 0 && f1 != f2);
        assert(whaleui_animation_frame_count() == 2);
        /* frame callbacks are one-shot: they fire once per frame */
        assert(whaleui_animation_frames_pump() == 2);
        assert(g_raf_calls == 2);
        assert(whaleui_animation_frame_count() == 0);
        /* cancel prevents the fire */
        long long f3 = whaleui_request_animation_frame(on_frame, nullptr);
        whaleui_cancel_animation_frame(f3);
        assert(whaleui_animation_frames_pump() == 0);
        assert(g_raf_calls == 2);
        /* the timer pump does not fire frame callbacks and vice versa */
        long long f4 = whaleui_request_animation_frame(on_frame, nullptr);
        assert(whaleui_timers_pump() == 0);
        assert(whaleui_animation_frame_count() == 1);
        whaleui_cancel_animation_frame(f4);
    }

    /* ---- visual viewport + devicePixelRatio ---- */
    {
        whaleui_window_t* win = whaleui_window_create(app, "vp", 640, 480);
        assert(win != nullptr);
        whaleui_visual_viewport_t vp;
        assert(whaleui_window_visual_viewport(win, &vp) == 0);
        assert(vp.width == 640.0f && vp.height == 480.0f);
        assert(vp.scale == 1.0f); /* no pinch zoom */
        assert(vp.offset_left == 0.0f && vp.offset_top == 0.0f);
        assert(whaleui_window_device_pixel_ratio(win) >= 1.0f);
        assert(whaleui_window_visual_viewport(nullptr, &vp) != 0);
        whaleui_window_destroy(win);
    }

    /* ---- Web Audio placeholder: feature-detect + shape ---- */
    {
        assert(whaleui_audio_is_supported() == 0); /* unimplemented for now */
        whaleui_audio_context_t* ac = whaleui_audio_context_create();
        assert(ac != nullptr);
        assert(whaleui_audio_context_state(ac) == WHALEUI_AUDIO_SUSPENDED);
        assert(whaleui_audio_context_sample_rate(ac) == 48000.0f);
        assert(whaleui_audio_context_current_time(ac) == 0.0);
        assert(whaleui_audio_context_resume(ac) != 0); /* cannot resume */
        assert(whaleui_audio_context_state(ac) == WHALEUI_AUDIO_SUSPENDED);
        assert(whaleui_audio_context_suspend(ac) == 0);
        assert(whaleui_audio_context_close(ac) == 0);
        assert(whaleui_audio_context_state(ac) == WHALEUI_AUDIO_CLOSED);
        assert(whaleui_audio_context_resume(ac) != 0); /* closed */
        whaleui_audio_context_destroy(ac);
        whaleui_audio_context_destroy(nullptr);
        assert(whaleui_audio_context_state(nullptr) == WHALEUI_AUDIO_CLOSED);
    }

    /* ---- gamepad: shape + out-of-range handling (no pad attached) ---- */
    {
        int n = whaleui_gamepad_count();
        assert(n >= 0);
        whaleui_gamepad_state_t st;
        /* an index past the end fails cleanly */
        assert(whaleui_gamepad_poll(n + 5, &st) != 0);
        assert(whaleui_gamepad_poll(-1, &st) != 0);
        assert(whaleui_gamepad_connected(n + 5) == 0);
        assert(std::strcmp(whaleui_gamepad_name(n + 5), "") == 0);
        assert(whaleui_gamepad_rumble(n + 5, 0.5f, 0.5f, 100) != 0);
        assert(whaleui_gamepad_poll(0, nullptr) != 0);
    }

    /* ---- richer event payloads ---- */
    {
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html("<html><body><div id=\"d\"></div></body></html>", 0);
        assert(doc != nullptr);
        whaleui_dom_element_t* d = whaleui_dom_get_element_by_id(doc, "d");
        assert(d != nullptr);
        assert(whaleui_dom_add_event_listener(d, "keydown", Payload::on, nullptr) == 0);
        assert(whaleui_dom_add_event_listener(d, "click", Payload::on, nullptr) == 0);
        assert(whaleui_dom_add_event_listener(d, "mousemove", Payload::on, nullptr) == 0);
        assert(whaleui_dom_add_event_listener(d, "copy", Payload::on, nullptr) == 0);

        whaleui_event_init_t in;
        std::memset(&in, 0, sizeof(in));
        in.key_code = 'a';
        in.key = "A";
        in.code = "KeyA";
        in.repeat = 1;
        in.mods = WHALEUI_MOD_SHIFT | WHALEUI_MOD_CTRL;
        int prevented = 0;
        assert(whaleui_dom_dispatch_input(d, "keydown", &in, &prevented) == 0);
        assert(prevented == 0);
        assert(Payload::mods == (WHALEUI_MOD_SHIFT | WHALEUI_MOD_CTRL));
        assert(Payload::shift == 1 && Payload::ctrl == 1);
        assert(Payload::alt == 0 && Payload::meta == 0);
        assert(Payload::key == "A" && Payload::code == "KeyA");
        assert(Payload::repeat == 1);

        /* meta/alt bits */
        std::memset(&in, 0, sizeof(in));
        in.mods = WHALEUI_MOD_ALT | WHALEUI_MOD_META;
        assert(whaleui_dom_dispatch_input(d, "keydown", &in, nullptr) == 0);
        assert(Payload::alt == 1 && Payload::meta == 1);
        assert(Payload::ctrl == 0 && Payload::shift == 0);

        /* mouse: detail + movement */
        std::memset(&in, 0, sizeof(in));
        in.mouse_x = 40;
        in.mouse_y = 50;
        in.mouse_button = 0;
        in.detail = 2;
        in.movement_x = 3.5f;
        in.movement_y = -1.5f;
        assert(whaleui_dom_dispatch_input(d, "click", &in, nullptr) == 0);
        assert(Payload::detail == 2);
        assert(Payload::mvx == 3.5f && Payload::mvy == -1.5f);

        /* relatedTarget on a non-bubbling enter */
        whaleui_dom_element_t* other = whaleui_dom_create_element(doc, "span");
        std::memset(&in, 0, sizeof(in));
        in.related_target = other;
        assert(whaleui_dom_dispatch_input_nonbubble(d, "mousemove", &in,
                                                    nullptr) == 0);
        assert(Payload::related == other);

        /* copy carries the payload text; the listener may replace it */
        std::memset(&in, 0, sizeof(in));
        in.clipboard_text = "engine-text";
        const char* supplied = nullptr;
        assert(whaleui_dom_dispatch_input_clipboard(d, "copy", &in, &prevented,
                                                    &supplied) == 0);
        assert(Payload::clip == "engine-text");
        assert(supplied == nullptr); /* listener did not supply its own */

        whaleui_dom_element_destroy(other);
        whaleui_dom_document_destroy(doc);
    }

    whaleui_app_destroy(app);
    return 0;
}
