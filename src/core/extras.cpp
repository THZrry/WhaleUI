/* Web Audio (placeholder) + Gamepad (SDL3-backed) + visual viewport.
 *
 * Audio: the project defers audio/video (rules/goal.md), so the AudioContext
 * surface exists for feature detection but creates no device and reports
 * "unsupported". The shapes are frozen so wiring a real backend later does not
 * change the API.
 *
 * Gamepad: SDL3 provides full gamepad support, so this is real - a
 * browser-shaped subset (dense indices, axes + a button bitmask + triggers)
 * over SDL_OpenGamepad/SDL_GetGamepadAxis/SDL_GetGamepadButton. Connect and
 * disconnect fire "gamepadconnected"/"gamepaddisconnected" on the document
 * element (see the pump below, driven from app.cpp).
 *
 * Visual viewport: the engine has no pinch zoom, so it equals the layout
 * viewport. The struct still reports the right values (and would keep working
 * if zoom lands later). */

#include "core/extras.h"
#include "core/window.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

struct lxb_dom_element; /* opaque here (gamepad event target) */

/* ======================= Web Audio (placeholder) ======================= */

struct whaleui_audio_context
{
    whaleui_audio_state_t state;
    float sample_rate;
    double created_ms;
};

extern "C" int whaleui_audio_is_supported(void)
{
    return 0; /* no audio backend yet (see rules/goal.md) */
}

extern "C" whaleui_audio_context_t* whaleui_audio_context_create(void)
{
    /* The context object exists (so a caller can hold the shape and query
     * state), but the state stays SUSPENDED forever: no device is opened and
     * resume() cannot succeed. Feature-detect with whaleui_audio_is_supported. */
    whaleui_audio_context_t* c = new whaleui_audio_context_t;
    c->state = WHALEUI_AUDIO_SUSPENDED;
    c->sample_rate = 48000.0f; /* the conventional default */
    c->created_ms = 0.0;
    return c;
}

extern "C" void whaleui_audio_context_destroy(whaleui_audio_context_t* ctx)
{
    delete ctx;
}

extern "C" whaleui_audio_state_t whaleui_audio_context_state(const whaleui_audio_context_t* ctx)
{
    return ctx ? ctx->state : WHALEUI_AUDIO_CLOSED;
}

extern "C" int whaleui_audio_context_resume(whaleui_audio_context_t* ctx)
{
    if (!ctx || ctx->state == WHALEUI_AUDIO_CLOSED) {
        return -1;
    }
    return -2; /* unsupported: nothing to resume */
}

extern "C" int whaleui_audio_context_suspend(whaleui_audio_context_t* ctx)
{
    if (!ctx || ctx->state == WHALEUI_AUDIO_CLOSED) {
        return -1;
    }
    ctx->state = WHALEUI_AUDIO_SUSPENDED;
    return 0;
}

extern "C" int whaleui_audio_context_close(whaleui_audio_context_t* ctx)
{
    if (!ctx) {
        return -1;
    }
    ctx->state = WHALEUI_AUDIO_CLOSED;
    return 0;
}

extern "C" float whaleui_audio_context_sample_rate(const whaleui_audio_context_t* ctx)
{
    return ctx ? ctx->sample_rate : 0.0f;
}

extern "C" double whaleui_audio_context_current_time(const whaleui_audio_context_t* ctx)
{
    (void)ctx;
    return 0.0; /* the clock never advances while unsupported */
}

/* ======================= Gamepad (SDL3-backed) ======================= */

namespace {

/* open gamepads by dense index; the SDL instance id is kept for the connect
 * event payload */
struct Pad
{
    SDL_JoystickID id;
    SDL_Gamepad* gp;
    std::string name;
    int connected;
};

std::vector<Pad>& pads()
{
    static std::vector<Pad> p;
    return p;
}

/* which document element gets the connect/disconnect events (set by app.cpp
 * on load); NULL before any document exists */
lxb_dom_element* g_gamepad_target = nullptr;

} // namespace

extern "C" void whaleui_gamepad_set_event_target(void* doc_element)
{
    g_gamepad_target = reinterpret_cast<lxb_dom_element*>(doc_element);
}

/* Open/close pads to match SDL's current device list; returns the number of
 * state changes (the caller repaints / fires events). Called from app.cpp on
 * gamepad add/remove events. */
extern "C" int whaleui_gamepad_sync(void)
{
    int changes = 0;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    /* mark all closed, then re-open the present ones (simplest correct sync) */
    for (size_t i = 0; i < pads().size(); ++i) {
        if (pads()[i].connected) {
            pads()[i].connected = 0;
            if (pads()[i].gp) {
                SDL_CloseGamepad(pads()[i].gp);
                pads()[i].gp = nullptr;
            }
            ++changes;
        }
    }
    for (int i = 0; i < count; ++i) {
        bool known = false;
        for (size_t j = 0; j < pads().size(); ++j) {
            if (pads()[j].id == ids[i] && pads()[j].gp) {
                known = true;
                break;
            }
        }
        if (known) {
            continue;
        }
        SDL_Gamepad* gp = SDL_OpenGamepad(ids[i]);
        if (!gp) {
            continue;
        }
        Pad p;
        p.id = ids[i];
        p.gp = gp;
        const char* nm = SDL_GetGamepadName(gp);
        p.name = nm ? nm : "";
        p.connected = 1;
        pads().push_back(p);
        ++changes;
    }
    SDL_free(ids);
    return changes;
}

extern "C" int whaleui_gamepad_count(void)
{
    int n = 0;
    for (size_t i = 0; i < pads().size(); ++i) {
        if (pads()[i].connected) {
            ++n;
        }
    }
    return n;
}

extern "C" int whaleui_gamepad_connected(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= pads().size()) {
        return 0;
    }
    return pads()[static_cast<size_t>(index)].connected;
}

extern "C" const char* whaleui_gamepad_name(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= pads().size() ||
        !pads()[static_cast<size_t>(index)].connected) {
        return "";
    }
    return pads()[static_cast<size_t>(index)].name.c_str();
}

extern "C" int whaleui_gamepad_poll(int index, whaleui_gamepad_state_t* out)
{
    if (!out) {
        return -1;
    }
    std::memset(out, 0, sizeof(*out));
    if (index < 0 || static_cast<size_t>(index) >= pads().size() ||
        !pads()[static_cast<size_t>(index)].connected) {
        return -2;
    }
    SDL_Gamepad* gp = pads()[static_cast<size_t>(index)].gp;
    if (!gp) {
        return -3;
    }
    SDL_UpdateGamepads();
    out->connected = 1;
    /* W3C axes: 0 = left X, 1 = left Y, 2 = right X, 3 = right Y */
    static const SDL_GamepadAxis kAxes[] = {
        SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY,
        SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY
    };
    out->axis_count = 0;
    for (int i = 0; i < 4 && i < WHALEUI_GAMEPAD_MAX_AXES; ++i) {
        Sint16 v = SDL_GetGamepadAxis(gp, kAxes[i]);
        out->axes[i] = v < 0 ? static_cast<float>(v) / 32768.0f
                             : static_cast<float>(v) / 32767.0f;
        out->axis_count = i + 1;
    }
    /* W3C button order: 0 A/south, 1 B/east, 2 X/west, 3 Y/north, then the
     * shoulders/triggers/stick clicks/dpad in the spec's order. SDL's own
     * enum already follows the standard layout for the first ten. */
    static const SDL_GamepadButton kButtons[] = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_GUIDE
    };
    out->buttons = 0;
    out->button_count = 0;
    for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); ++i) {
        if (i < WHALEUI_GAMEPAD_MAX_BUTTONS &&
            SDL_GetGamepadButton(gp, kButtons[i])) {
            out->buttons |= (1u << i);
        }
        out->button_count = static_cast<int>(i) + 1;
    }
    out->trigger_left =
        static_cast<float>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) /
        32767.0f;
    out->trigger_right =
        static_cast<float>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) /
        32767.0f;
    return 0;
}

extern "C" int whaleui_gamepad_rumble(int index, float low_freq,
                                      float high_freq,
                                      unsigned int duration_ms)
{
    if (index < 0 || static_cast<size_t>(index) >= pads().size() ||
        !pads()[static_cast<size_t>(index)].connected) {
        return -1;
    }
    SDL_Gamepad* gp = pads()[static_cast<size_t>(index)].gp;
    if (!gp) {
        return -2;
    }
    Uint16 lo = static_cast<Uint16>(low_freq <= 0.0f ? 0
                                  : low_freq >= 1.0f ? 0xFFFF
                                  : low_freq * 65535.0f);
    Uint16 hi = static_cast<Uint16>(high_freq <= 0.0f ? 0
                                  : high_freq >= 1.0f ? 0xFFFF
                                  : high_freq * 65535.0f);
    return SDL_RumbleGamepad(gp, lo, hi, duration_ms) ? 0 : -3;
}

/* ======================= visual viewport ======================= */

extern "C" int whaleui_window_visual_viewport(const whaleui_window_t* win,
                                              whaleui_visual_viewport_t* out)
{
    if (!win || !out) {
        return -1;
    }
    out->width = static_cast<float>(win->width);
    out->height = static_cast<float>(win->height);
    out->scale = 1.0f; /* no pinch zoom */
    out->offset_left = 0.0f;
    out->offset_top = 0.0f;
    out->page_left = 0.0f;
    out->page_top = 0.0f;
    return 0;
}

extern "C" float whaleui_window_device_pixel_ratio(const whaleui_window_t* win)
{
    if (!win) {
        return 1.0f;
    }
    /* physical / CSS pixels; falls back to 1.0 when SDL cannot report it */
    float s = win->sdl ? SDL_GetWindowDisplayScale(win->sdl) : 1.0f;
    return s > 0.0f ? s : 1.0f;
}
