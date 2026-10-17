// test_web: web-platform APIs — clipboard, timers/rAF, visual viewport,
// Web Audio placeholder, gamepad, and the richer event payloads.
#include "whaleui.h"
#include "dom/events.h" /* internal full-payload dispatch */
#include "dom/observers.h"
#include "core/clipboard.h"
#include "core/timers.h"
#include "core/webapi.h"
#include "test_util.h"

#include <SDL3/SDL.h> /* SDL_GetError for the clipboard-availability probe */

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

/* --- microtask chain probe (queues itself once) --- */
int g_micro_chain = 0;
void micro_chain_step(void*) { ++g_micro_chain; }

} // namespace

int main(void)
{
    /* SDL must be initialized before the clipboard / gamepad subsystems can
     * be used, so the app comes first (its create() does SDL_Init). */
    whaleui_app_t* app = whaleui_app_create();
    assert(app != nullptr);

    /* ---- clipboard (sync + async queue) ----
     *
     * The OS clipboard can be unavailable to the process (a session without a
     * window station - headless CI, a service context): SDL then fails to open
     * it and every write returns false. That is an environment fact, not an
     * engine bug, so probe once and skip the block instead of failing. */
    const bool clipboard_ok = whaleui_clipboard_set_text("whaleui-probe") == 0;
    if (!clipboard_ok) {
        std::fprintf(stderr,
                     "clipboard unavailable in this environment (%s), "
                     "skipping clipboard checks\n",
                     SDL_GetError());
    }
    if (clipboard_ok) {
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

    /* ---- drag & drop: DataTransfer / File + drag events ---- */
    {
        whaleui_data_transfer_t* dt = whaleui_data_transfer_create();
        assert(dt != nullptr);
        assert(whaleui_data_transfer_type_count(dt) == 0);
        assert(whaleui_data_transfer_set_text(dt, "dragged") == 0);
        assert(std::strcmp(whaleui_data_transfer_get_text(dt), "dragged") == 0);
        assert(whaleui_data_transfer_has_format(dt, "text/plain") == 1);
        assert(whaleui_data_transfer_has_format(dt, "text/html") == 0);
        assert(whaleui_data_transfer_get_data(dt, "text/html") == nullptr);
        assert(whaleui_data_transfer_set_data(dt, "text/html", "<b>x</b>") == 0);
        assert(whaleui_data_transfer_type_count(dt) == 2);
        assert(std::strcmp(whaleui_data_transfer_type_item(dt, 0), "text/plain") == 0);
        assert(std::strcmp(whaleui_data_transfer_type_item(dt, 1), "text/html") == 0);
        /* set replaces an existing format in place */
        assert(whaleui_data_transfer_set_text(dt, "replaced") == 0);
        assert(whaleui_data_transfer_type_count(dt) == 2);
        assert(std::strcmp(whaleui_data_transfer_get_text(dt), "replaced") == 0);
        /* effects */
        whaleui_data_transfer_set_effect_allowed(dt, 2);
        assert(whaleui_data_transfer_effect_allowed(dt) == 2);
        whaleui_data_transfer_set_drop_effect(dt, 1);
        assert(whaleui_data_transfer_drop_effect(dt) == 1);
        assert(whaleui_data_transfer_clear_data(dt) == 0);
        assert(whaleui_data_transfer_type_count(dt) == 0);

        /* files: name/uri + lazy size through the VFS */
        assert(whaleui_data_transfer_add_file(dt, "page.html",
                                              TEST_URI_RAW("tests/data/page.html")) == 0);
        assert(whaleui_data_transfer_file_count(dt) == 1);
        const whaleui_file_t* f = whaleui_data_transfer_file_item(dt, 0);
        assert(f != nullptr);
        assert(std::strcmp(whaleui_file_name(f), "page.html") == 0);
        assert(whaleui_file_size(f) > 0);
        char* fdata = nullptr;
        size_t flen = 0;
        assert(whaleui_file_read(f, &fdata, &flen) == 0);
        assert(fdata != nullptr && flen > 0);
        std::free(fdata);
        /* out of range */
        assert(whaleui_data_transfer_file_item(dt, 9) == nullptr);
        assert(whaleui_file_size(nullptr) == -1);
        /* bad args */
        assert(whaleui_data_transfer_set_data(dt, "", "x") != 0);
        assert(whaleui_data_transfer_add_file(dt, "n", nullptr) != 0);
        whaleui_data_transfer_destroy(dt);
        whaleui_data_transfer_destroy(nullptr);

        /* drag events: dragover preventDefault marks the drop target */
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<html><body><div id=\"src\" draggable=\"true\">drag me</div>"
            "<div id=\"tgt\"></div></body></html>", 0);
        assert(doc != nullptr);
        whaleui_dom_element_t* src = whaleui_dom_get_element_by_id(doc, "src");
        whaleui_dom_element_t* tgt = whaleui_dom_get_element_by_id(doc, "tgt");
        assert(src && tgt);

        static int dragstart_n = 0, dragenter_n = 0, dragover_n = 0;
        static int drop_n = 0, dragend_n = 0, dragleave_n = 0;
        static std::string drop_text;
        static bool dragstart_ok = false;
        struct Dnd {
            static void on_start(whaleui_dom_event_t* ev, void*) {
                ++dragstart_n;
                whaleui_data_transfer_t* d = whaleui_dom_event_data_transfer(ev);
                if (d) { whaleui_data_transfer_set_text(d, "from-src"); dragstart_ok = true; }
            }
            static void on_enter(whaleui_dom_event_t* ev, void*) {
                ++dragenter_n;
                (void)whaleui_dom_event_data_transfer(ev);
            }
            static void on_over(whaleui_dom_event_t* ev, void*) {
                ++dragover_n;
                whaleui_dom_event_prevent_default(ev); /* accept the drop */
            }
            static void on_drop(whaleui_dom_event_t* ev, void*) {
                ++drop_n;
                whaleui_data_transfer_t* d = whaleui_dom_event_data_transfer(ev);
                const char* t = d ? whaleui_data_transfer_get_text(d) : nullptr;
                drop_text = t ? t : "";
            }
            static void on_end(whaleui_dom_event_t* ev, void*) {
                ++dragend_n;
                (void)ev;
            }
            static void on_leave(whaleui_dom_event_t* ev, void*) {
                ++dragleave_n;
                (void)ev;
            }
        };
        assert(whaleui_dom_add_event_listener(src, "dragstart", Dnd::on_start, nullptr) == 0);
        assert(whaleui_dom_add_event_listener(tgt, "dragenter", Dnd::on_enter, nullptr) == 0);
        assert(whaleui_dom_add_event_listener(tgt, "dragover", Dnd::on_over, nullptr) == 0);
        assert(whaleui_dom_add_event_listener(tgt, "drop", Dnd::on_drop, nullptr) == 0);
        assert(whaleui_dom_add_event_listener(tgt, "dragleave", Dnd::on_leave, nullptr) == 0);
        assert(whaleui_dom_add_event_listener(src, "dragend", Dnd::on_end, nullptr) == 0);

        whaleui_data_transfer_t* live = whaleui_data_transfer_create();
        assert(whaleui_data_transfer_set_text(live, "payload") == 0);
        int prevented = 0;
        assert(whaleui_dom_dispatch_drag(src, "dragstart", live, 5, 5, 0, &prevented) == 0);
        assert(prevented == 0 && dragstart_n == 1 && dragstart_ok);
        assert(whaleui_dom_dispatch_drag(tgt, "dragenter", live, 50, 50, 0, nullptr) == 0);
        assert(dragenter_n == 1);
        assert(whaleui_dom_dispatch_drag(tgt, "dragover", live, 50, 50, 0, &prevented) == 0);
        assert(dragover_n == 1 && prevented == 1);
        assert(whaleui_dom_dispatch_drag(tgt, "drop", live, 50, 50, 0, nullptr) == 0);
        /* the dragstart listener replaced the text, so the drop sees that */
        assert(drop_n == 1 && drop_text == "from-src");
        assert(whaleui_dom_dispatch_drag(src, "dragend", live, 50, 50, 0, nullptr) == 0);
        assert(dragend_n == 1);
        whaleui_data_transfer_destroy(live);

        /* a drag event bubbles (drop on an ancestor listener) */
        static int ancestor_drop = 0;
        struct B {
            static void on(whaleui_dom_event_t* ev, void*) {
                (void)ev;
                ++ancestor_drop;
            }
        };
        whaleui_dom_element_t* body = whaleui_dom_body(doc);
        assert(whaleui_dom_add_event_listener(body, "dragover", B::on, nullptr) == 0);
        whaleui_data_transfer_t* bdt = whaleui_data_transfer_create();
        assert(whaleui_dom_dispatch_drag(tgt, "dragover", bdt, 1, 1, 0, nullptr) == 0);
        assert(ancestor_drop == 1);
        whaleui_data_transfer_destroy(bdt);

        /* no transfer attached -> NULL (not a drag event) */
        assert(whaleui_dom_dispatch_event(tgt, "click") == 0);

        whaleui_dom_document_destroy(doc);
    }

    /* ---- matchMedia / CSS.supports / screen / navigator ---- */
    {
        whaleui_window_t* win = whaleui_window_create(app, "mm", 800, 600);
        assert(win != nullptr);
        /* min-width:500px matches at 800 wide, not at 400 */
        whaleui_media_query_list_t* mq =
            whaleui_window_match_media(win, "(min-width: 500px)");
        assert(mq != nullptr);
        assert(whaleui_media_query_list_matches(mq) == 1);
        assert(std::strcmp(whaleui_media_query_list_media(mq),
                           "(min-width: 500px)") == 0);
        static int mq_fires = 0;
        struct MQ {
            static void on(whaleui_media_query_list_t* l, void*) {
                ++mq_fires;
                (void)l;
            }
        };
        assert(whaleui_media_query_list_add_listener(mq, MQ::on, nullptr) == 0);
        assert(whaleui_media_query_list_add_listener(mq, MQ::on, nullptr) == 0); /* dup */
        /* resize across the breakpoint + refresh (app does this on resize) */
        assert(whaleui_window_set_size(win, 400, 600) == 0);
        whaleui_webapi_refresh_media();
        assert(whaleui_media_query_list_matches(mq) == 0);
        assert(mq_fires == 1);
        /* no change -> no fire */
        whaleui_webapi_refresh_media();
        assert(mq_fires == 1);
        assert(whaleui_media_query_list_remove_listener(mq, MQ::on, nullptr) == 0);
        assert(whaleui_media_query_list_remove_listener(mq, MQ::on, nullptr) != 0);
        whaleui_media_query_list_destroy(mq);
        whaleui_media_query_list_destroy(nullptr);
        assert(whaleui_window_match_media(nullptr, "(x)") == nullptr);
        assert(whaleui_window_match_media(win, nullptr) == nullptr);

        /* CSS.supports: property support is a table (the CSS parser keeps
         * unknown declarations, so probing it alone answered 1 to everything),
         * and the value is checked with the engine's own parsers. */
        assert(whaleui_css_supports("display", "flex") == 1);
        assert(whaleui_css_supports("color", "red") == 1);
        assert(whaleui_css_supports("nonsense-property", "1px") == 0);
        assert(whaleui_css_supports("", "red") == 0);
        assert(whaleui_css_supports("color", nullptr) == 0);
        /* a supported property with an unsupported value */
        assert(whaleui_css_supports("display", "not-a-real-display") == 0);
        assert(whaleui_css_supports("color", "not-a-color") == 0);
        assert(whaleui_css_supports("position", "sideways") == 0);
        assert(whaleui_css_supports("overflow", "hidden") == 1);
        /* property names are ASCII case-insensitive */
        assert(whaleui_css_supports("DISPLAY", "FLEX") == 1);
        /* custom properties are always accepted */
        assert(whaleui_css_supports("--brand", "12px") == 1);
        /* values the engine resolves at cascade time are accepted */
        assert(whaleui_css_supports("width", "calc(100% - 2px)") == 1);
        assert(whaleui_css_supports("color", "var(--c)") == 1);
        /* length values are validated */
        assert(whaleui_css_supports("width", "12px") == 1);
        assert(whaleui_css_supports("width", "12qu") == 0);
        assert(whaleui_css_supports("width", "auto") == 1);
        /* no value at all */
        assert(whaleui_css_supports("color", "") == 0);

        /* screen + navigator */
        int sw = 0, sh = 0;
        assert(whaleui_window_screen_size(win, &sw, &sh) == 0);
        assert(sw > 0 && sh > 0);
        int sx = 0, sy = 0, aw = 0, ah = 0;
        assert(whaleui_window_screen_work_area(win, &sx, &sy, &aw, &ah) == 0);
        assert(aw > 0 && ah > 0);
        assert(whaleui_window_screen_size(win, nullptr, nullptr) != 0);
        assert(std::strstr(whaleui_navigator_user_agent(), "WhaleUI") != nullptr);
        assert(std::strlen(whaleui_navigator_platform()) > 0);
        assert(std::strlen(whaleui_navigator_language()) > 0);

        whaleui_window_destroy(win);
    }

    /* ---- Web Storage ---- */
    {
        /* sessionStorage: memory only */
        assert(whaleui_storage_clear(1) == 0);
        assert(whaleui_storage_length(1) == 0);
        assert(whaleui_storage_set_item(1, "k", "v") == 0);
        assert(whaleui_storage_length(1) == 1);
        assert(std::strcmp(whaleui_storage_get_item(1, "k"), "v") == 0);
        assert(whaleui_storage_get_item(1, "missing") == nullptr);
        assert(std::strcmp(whaleui_storage_key(1, 0), "k") == 0);
        assert(whaleui_storage_key(1, 5) == nullptr);
        assert(whaleui_storage_remove_item(1, "k") == 0);
        assert(whaleui_storage_remove_item(1, "k") != 0);
        assert(whaleui_storage_length(1) == 0);
        assert(whaleui_storage_set_item(1, nullptr, "v") != 0);
        assert(whaleui_storage_set_item(1, "k", nullptr) != 0);

        /* localStorage: same API, persisted (value with a newline round-trips
         * through the escape) */
        assert(whaleui_storage_clear(0) == 0);
        assert(whaleui_storage_set_item(0, "multi", "a\nb") == 0);
        assert(whaleui_storage_length(0) == 1);
        assert(std::strcmp(whaleui_storage_get_item(0, "multi"), "a\nb") == 0);
        assert(whaleui_storage_clear(0) == 0);
        assert(whaleui_storage_length(0) == 0);
    }

    /* ---- queueMicrotask / requestIdleCallback ---- */
    {
        static int micro_n = 0, idle_n = 0;
        struct T {
            static void micro(void*) { ++micro_n; }
            static void idle(void*) { ++idle_n; }
        };
        assert(whaleui_webapi_has_microtasks() == 0);
        assert(whaleui_queue_microtask(T::micro, nullptr) == 0);
        assert(whaleui_webapi_has_microtasks() == 1);
        assert(whaleui_webapi_pump_microtasks() == 1);
        assert(micro_n == 1);
        assert(whaleui_webapi_has_microtasks() == 0);
        /* a microtask that queues another microtask drains in the same pump */
        assert(whaleui_queue_microtask(micro_chain_step, nullptr) == 0);
        assert(whaleui_webapi_pump_microtasks() == 1);
        assert(g_micro_chain == 1);
        assert(whaleui_queue_microtask(nullptr, nullptr) != 0);

        assert(whaleui_webapi_has_idle() == 0);
        assert(whaleui_request_idle_callback(T::idle, nullptr) == 0);
        assert(whaleui_webapi_has_idle() == 1);
        assert(whaleui_webapi_pump_idle() == 1);
        assert(idle_n == 1);
        assert(whaleui_webapi_has_idle() == 0);
        assert(whaleui_request_idle_callback(nullptr, nullptr) != 0);
    }

    /* ---- MutationObserver ---- */
    {
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html("<html><body><div id=\"a\"></div></body></html>", 0);
        assert(doc != nullptr);
        whaleui_dom_element_t* a = whaleui_dom_get_element_by_id(doc, "a");
        assert(a != nullptr);

        static int mut_n = 0, mut_type = -1, mut_added = 0, mut_removed = 0;
        static std::string mut_attr;
        struct MO {
            static void on(whaleui_dom_element_t* target, int type,
                           const char* attr, int added, int removed, void*) {
                (void)target;
                ++mut_n;
                mut_type = type;
                mut_attr = attr ? attr : "";
                mut_added = added;
                mut_removed = removed;
            }
        };
        whaleui_mutation_observer_t* mo =
            whaleui_dom_create_mutation_observer(doc);
        assert(mo != nullptr);
        assert(whaleui_dom_mutation_observer_observe(mo, a, MO::on, nullptr) == 0);
        /* records are queued, not delivered inline */
        assert(whaleui_dom_set_attribute(a, "data-x", "1") == 0);
        assert(mut_n == 0);
        assert(whaleui_dom_observers_pump() >= 1);
        assert(mut_n == 1);
        assert(mut_type == WHALEUI_MUTATION_ATTRIBUTES);
        assert(mut_attr == "data-x");
        /* a structural edit reports childList with added=1 */
        whaleui_dom_element_t* child = whaleui_dom_create_element(doc, "span");
        mut_n = 0;
        assert(whaleui_dom_append_child(a, child) == 0);
        assert(whaleui_dom_observers_pump() >= 1);
        assert(mut_n >= 1);
        assert(mut_type == WHALEUI_MUTATION_CHILD_LIST);
        assert(mut_added == 1);
        /* a mutation outside the observed subtree is not delivered */
        whaleui_dom_element_t* other = whaleui_dom_create_element(doc, "b");
        assert(whaleui_dom_append_child(whaleui_dom_body(doc), other) == 0);
        mut_n = 0;
        whaleui_dom_observers_pump();
        assert(mut_n == 0);
        /* disconnect stops delivery. Regression: the disconnected observer
         * stays in the registry with a NULL callback, and the pump used to
         * call it anyway - it jumped to address 0 on the next mutation
         * (SIGSEGV, seen as a bare crash with no output). */
        assert(whaleui_dom_mutation_observer_disconnect(mo) == 0);
        assert(whaleui_dom_set_attribute(a, "data-y", "2") == 0);
        mut_n = 0;
        whaleui_dom_observers_pump();
        assert(mut_n == 0);
        /* a second mutation while disconnected must not deliver either */
        assert(whaleui_dom_set_attribute(a, "data-z", "3") == 0);
        whaleui_dom_observers_pump();
        assert(mut_n == 0);
        /* re-observing after disconnect resumes delivery */
        assert(whaleui_dom_mutation_observer_observe(mo, a, MO::on, nullptr) == 0);
        mut_n = 0;
        assert(whaleui_dom_set_attribute(a, "data-w", "4") == 0);
        assert(whaleui_dom_observers_pump() >= 1);
        assert(mut_n == 1);
        assert(mut_attr == "data-w");
        /* ONE record per attribute edit: the named record and the generic one
         * used to both fire, so an observer saw every setAttribute twice. */
        whaleui_dom_mutation_observer_destroy(mo);
        whaleui_dom_mutation_observer_destroy(nullptr);
        whaleui_dom_document_destroy(doc);
    }

    /* ---- MutationObserver reports the attribute name for EVERY mutating API,
     *      not just setAttribute ----
     *
     * classList / dataset / style edits all rewrite an attribute, so a record
     * for them must name it ("class"/"data-*"/"style"). They used to reach the
     * observer through the generic mark_dirty path with attr == NULL, which
     * made a listener that filters on the name (the common pattern) miss them
     * entirely. */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<html><body><div id=\"a\"></div></body></html>", 0);
        assert(doc != nullptr);
        whaleui_dom_element_t* a = whaleui_dom_get_element_by_id(doc, "a");
        assert(a != nullptr);
        static std::string last_attr;
        static int rec_n = 0;
        struct NameObs {
            static void on(whaleui_dom_element_t*, int type, const char* attr,
                           int, int, void*) {
                if (type == WHALEUI_MUTATION_ATTRIBUTES) {
                    ++rec_n;
                    last_attr = attr ? attr : "(null)";
                }
            }
        };
        whaleui_mutation_observer_t* mo =
            whaleui_dom_create_mutation_observer(doc);
        assert(mo != nullptr);
        assert(whaleui_dom_mutation_observer_observe(mo, a, NameObs::on,
                                                     nullptr) == 0);

        assert(whaleui_dom_class_add(a, "x") == 0);
        rec_n = 0;
        whaleui_dom_observers_pump();
        assert(rec_n == 1);
        assert(last_attr == "class");

        assert(whaleui_dom_dataset_set(a, "ownerId", "7") == 0);
        rec_n = 0;
        whaleui_dom_observers_pump();
        assert(rec_n == 1);
        assert(last_attr == "data-owner-id");

        assert(whaleui_dom_set_style(a, "color", "red") == 0);
        rec_n = 0;
        whaleui_dom_observers_pump();
        assert(rec_n == 1);
        assert(last_attr == "style");

        assert(whaleui_dom_style_remove_property(a, "color") == 0);
        rec_n = 0;
        whaleui_dom_observers_pump();
        assert(rec_n == 1);
        assert(last_attr == "style");

        assert(whaleui_dom_toggle_attribute(a, "hidden", -1) == 1);
        rec_n = 0;
        whaleui_dom_observers_pump();
        assert(rec_n == 1);
        assert(last_attr == "hidden");

        whaleui_dom_mutation_observer_destroy(mo);
        whaleui_dom_document_destroy(doc);
        std::printf("[web] observer attribute names OK\n");
    }

    /* ---- ResizeObserver: initial size, then only on change ---- */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<html><body style=\"margin:0\">"
            "<div id=\"a\" style=\"width:40px;height:30px\"></div>"
            "</body></html>", 0);
        assert(doc != nullptr);
        whaleui_dom_element_t* a = whaleui_dom_get_element_by_id(doc, "a");
        assert(a != nullptr);
        static int rz_n = 0;
        static float rz_w = 0.0f, rz_h = 0.0f;
        struct RO {
            static void on(whaleui_dom_element_t*, float w, float h, void*) {
                ++rz_n;
                rz_w = w;
                rz_h = h;
            }
        };
        whaleui_resize_observer_t* ro = whaleui_dom_create_resize_observer(doc);
        assert(ro != nullptr);
        assert(whaleui_dom_resize_observer_observe(ro, a, RO::on, nullptr) == 0);
        assert(whaleui_dom_resize_observer_observe(ro, nullptr, RO::on,
                                                   nullptr) != 0);
        /* no layout tree yet: nothing to sample (the pump reports no work) */
        whaleui_dom_resize_observers_pump();
        assert(rz_n == 0);
        /* unobserve/disconnect are safe and idempotent */
        assert(whaleui_dom_resize_observer_unobserve(ro, a) == 0);
        assert(whaleui_dom_resize_observer_disconnect(ro) == 0);
        whaleui_dom_resize_observer_destroy(ro);
        whaleui_dom_resize_observer_destroy(nullptr);
        whaleui_dom_document_destroy(doc);
    }

    whaleui_app_destroy(app);
    return 0;
}
