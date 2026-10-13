/* DOM events: add/remove/dispatch + transient event object.
 *
 * Listeners are stored in a side map keyed by the lexbor element (no event
 * slots on the nodes). Dispatch follows the DOM three-phase model (capture
 * down the ancestor chain, target, bubble back up); the callback receives a
 * transient whaleui_dom_event_t valid for the call. */

#include "dom/events.h"

#include <lexbor/dom/dom.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

/* event phases (mirror the DOM spec values) */
enum
{
    WUI_PHASE_CAPTURE = 1,
    WUI_PHASE_TARGET = 2,
    WUI_PHASE_BUBBLE = 3
};

/* the transient event object (defined here; whaleui.h keeps it opaque).
 * Must live at global scope so the handle type resolves. */
struct whaleui_dom_event
{
    lxb_dom_element* target;   /* dispatch target (never changes) */
    lxb_dom_element* current;  /* currentTarget: node whose listeners run */
    const char* type;
    int phase;                 /* 1 = capture, 2 = target, 3 = bubble */
    int prevented;
    int stopped;
    int key_code;
    int mouse_x, mouse_y, mouse_button;
    float wheel_x, wheel_y;
};

namespace {

struct Listener
{
    std::string type;
    whaleui_event_cb cb;
    void* userdata;
    bool capture; /* capture-phase listener (useCapture) */
};

std::map<lxb_dom_element*, std::vector<Listener>>& listeners()
{
    static std::map<lxb_dom_element*, std::vector<Listener>> m;
    return m;
}

whaleui_dom_event_t* make_event(lxb_dom_element* el, const char* type)
{
    whaleui_dom_event_t* ev = new whaleui_dom_event_t;
    ev->target = el;
    ev->current = el;
    ev->type = type;
    ev->phase = WUI_PHASE_TARGET;
    ev->prevented = 0;
    ev->stopped = 0;
    ev->key_code = 0;
    ev->mouse_x = ev->mouse_y = ev->mouse_button = 0;
    ev->wheel_x = ev->wheel_y = 0;
    return ev;
}

/* run the matching listeners of one node for one phase. Capture listeners
 * fire on the way down AND at the target; bubble listeners fire at the
 * target AND on the way up. Copy the vector first: a listener may
 * add/remove listeners (including itself) while iterating. */
void run_listeners(whaleui_dom_event_t* ev, lxb_dom_element* node, bool cap)
{
    if (!node) {
        return;
    }
    std::map<lxb_dom_element*, std::vector<Listener>>::iterator it =
        listeners().find(node);
    if (it == listeners().end() || it->second.empty()) {
        return;
    }
    std::vector<Listener> v = it->second;
    ev->current = node;
    for (size_t i = 0; i < v.size(); ++i) {
        if (ev->stopped) {
            break;
        }
        if (v[i].capture != cap) {
            continue;
        }
        if (v[i].type != ev->type) {
            continue;
        }
        v[i].cb(reinterpret_cast<whaleui_dom_event_t*>(ev), v[i].userdata);
    }
}

} // namespace

extern "C" void whaleui_events_clear_element(lxb_dom_element* el)
{
    if (el) {
        listeners().erase(el);
    }
}

extern "C" void whaleui_events_clear_all(void)
{
    listeners().clear();
}

extern "C" int whaleui_dom_add_event_listener(whaleui_dom_element_t* el,
                                              const char* type,
                                              whaleui_event_cb cb, void* userdata)
{
    return whaleui_dom_add_event_listener_ex(el, type, cb, userdata, 0);
}

extern "C" int whaleui_dom_add_event_listener_ex(whaleui_dom_element_t* el,
                                                 const char* type,
                                                 whaleui_event_cb cb,
                                                 void* userdata, int capture)
{
    lxb_dom_element* e = reinterpret_cast<lxb_dom_element*>(el);
    if (!e || !type || !cb) {
        return -1;
    }
    std::vector<Listener>& v = listeners()[e];
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].type == type && v[i].cb == cb &&
            v[i].userdata == userdata && v[i].capture == (capture != 0)) {
            return 0; /* duplicate: no-op success */
        }
    }
    Listener l;
    l.type = type;
    l.cb = cb;
    l.userdata = userdata;
    l.capture = capture != 0;
    v.push_back(l);
    return 0;
}

extern "C" int whaleui_dom_remove_event_listener(whaleui_dom_element_t* el,
                                                 const char* type,
                                                 whaleui_event_cb cb,
                                                 void* userdata)
{
    lxb_dom_element* e = reinterpret_cast<lxb_dom_element*>(el);
    if (!e || !type || !cb) {
        return -1;
    }
    std::map<lxb_dom_element*, std::vector<Listener>>::iterator it =
        listeners().find(e);
    if (it == listeners().end()) {
        return -2;
    }
    std::vector<Listener>& v = it->second;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].type == type && v[i].cb == cb && v[i].userdata == userdata) {
            v.erase(v.begin() + static_cast<long>(i));
            return 0;
        }
    }
    return -2;
}

extern "C" int whaleui_dom_dispatch_event_full(whaleui_dom_element_t* el,
                                               const char* type,
                                               int key_code, int mouse_x,
                                               int mouse_y, int mouse_button,
                                               float wheel_x, float wheel_y)
{
    int prevented = 0;
    int rc = whaleui_dom_dispatch_phased(el, type, key_code, mouse_x,
                                         mouse_y, mouse_button, wheel_x,
                                         wheel_y, &prevented);
    (void)prevented; /* compat entry: callers that need the default-behavior
                      * hook use _phased directly */
    return rc;
}

namespace {

/* shared dispatch body. `bubble` selects whether the bubble phase runs
 * (false for the browser's non-bubbling events: mouseenter/leave,
 * focus/blur). */
int dispatch_impl(whaleui_dom_element_t* el, const char* type, int key_code,
                  int mouse_x, int mouse_y, int mouse_button, float wheel_x,
                  float wheel_y, int* out_prevented, bool bubble)
{
    if (out_prevented) {
        *out_prevented = 0;
    }
    lxb_dom_element* e = reinterpret_cast<lxb_dom_element*>(el);
    if (!e || !type) {
        return -1;
    }
    /* ancestor chain (element parents up to the document root) */
    std::vector<lxb_dom_element*> chain;
    for (lxb_dom_node* a = e->node.parent; a; a = a->parent) {
        if (a->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            chain.push_back(lxb_dom_interface_element(a));
        }
    }
    whaleui_dom_event_t* ev = make_event(e, type);
    ev->key_code = key_code;
    ev->mouse_x = mouse_x;
    ev->mouse_y = mouse_y;
    ev->mouse_button = mouse_button;
    ev->wheel_x = wheel_x;
    ev->wheel_y = wheel_y;
    /* 1. capture: root-most ancestor down to the target's parent */
    ev->phase = WUI_PHASE_CAPTURE;
    for (size_t i = chain.size(); i-- > 0;) {
        if (ev->stopped) {
            break;
        }
        run_listeners(ev, chain[i], true);
    }
    /* 2. target: capture listeners first, then bubble listeners */
    if (!ev->stopped) {
        ev->phase = WUI_PHASE_TARGET;
        run_listeners(ev, e, true);
    }
    if (!ev->stopped) {
        run_listeners(ev, e, false);
    }
    /* 3. bubble: parent back up to the root (skipped for non-bubbling types) */
    if (bubble && !ev->stopped) {
        ev->phase = WUI_PHASE_BUBBLE;
        for (size_t i = 0; i < chain.size(); ++i) {
            if (ev->stopped) {
                break;
            }
            run_listeners(ev, chain[i], false);
        }
    }
    if (out_prevented) {
        *out_prevented = ev->prevented;
    }
    delete ev;
    return 0;
}

} // namespace

extern "C" int whaleui_dom_dispatch_phased(whaleui_dom_element_t* el,
                                           const char* type,
                                           int key_code, int mouse_x,
                                           int mouse_y, int mouse_button,
                                           float wheel_x, float wheel_y,
                                           int* out_prevented)
{
    return dispatch_impl(el, type, key_code, mouse_x, mouse_y, mouse_button,
                         wheel_x, wheel_y, out_prevented, true);
}

extern "C" int whaleui_dom_dispatch_nonbubble(whaleui_dom_element_t* el,
                                              const char* type,
                                              int key_code, int mouse_x,
                                              int mouse_y, int mouse_button,
                                              float wheel_x, float wheel_y,
                                              int* out_prevented)
{
    return dispatch_impl(el, type, key_code, mouse_x, mouse_y, mouse_button,
                         wheel_x, wheel_y, out_prevented, false);
}

extern "C" int whaleui_dom_dispatch_event(whaleui_dom_element_t* el,
                                          const char* type)
{
    return whaleui_dom_dispatch_event_full(el, type, 0, 0, 0, 0, 0, 0);
}

/* --- event object accessors --- */

extern "C" whaleui_dom_element_t* whaleui_dom_event_target(const whaleui_dom_event_t* ev)
{
    return ev ? reinterpret_cast<whaleui_dom_element_t*>(ev->target) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_event_current_target(
    const whaleui_dom_event_t* ev)
{
    return ev ? reinterpret_cast<whaleui_dom_element_t*>(ev->current) : nullptr;
}

extern "C" int whaleui_dom_event_phase(const whaleui_dom_event_t* ev)
{
    return ev ? ev->phase : 0;
}

extern "C" const char* whaleui_dom_event_type(const whaleui_dom_event_t* ev)
{
    return ev ? ev->type : nullptr;
}

extern "C" int whaleui_dom_event_prevent_default(whaleui_dom_event_t* ev)
{
    if (!ev) {
        return -1;
    }
    ev->prevented = 1;
    return 0;
}

extern "C" int whaleui_dom_event_stop_propagation(whaleui_dom_event_t* ev)
{
    if (!ev) {
        return -1;
    }
    ev->stopped = 1;
    return 0;
}

extern "C" int whaleui_dom_event_default_prevented(const whaleui_dom_event_t* ev)
{
    return ev ? ev->prevented : 0;
}

extern "C" int whaleui_dom_event_key_code(const whaleui_dom_event_t* ev)
{
    return ev ? ev->key_code : 0;
}

extern "C" int whaleui_dom_event_mouse_x(const whaleui_dom_event_t* ev)
{
    return ev ? ev->mouse_x : 0;
}

extern "C" int whaleui_dom_event_mouse_y(const whaleui_dom_event_t* ev)
{
    return ev ? ev->mouse_y : 0;
}

extern "C" int whaleui_dom_event_mouse_button(const whaleui_dom_event_t* ev)
{
    return ev ? ev->mouse_button : 0;
}

extern "C" float whaleui_dom_event_wheel_delta_x(const whaleui_dom_event_t* ev)
{
    return ev ? ev->wheel_x : 0;
}

extern "C" float whaleui_dom_event_wheel_delta_y(const whaleui_dom_event_t* ev)
{
    return ev ? ev->wheel_y : 0;
}
