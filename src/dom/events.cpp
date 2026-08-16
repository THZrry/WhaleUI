/* DOM events: add/remove/dispatch + transient event object.
 *
 * Listeners are stored in a side map keyed by the lexbor element (no event
 * slots on the nodes). Dispatch walks the target's listeners only; the
 * callback receives a transient whaleui_dom_event_t valid for the call. */

#include "dom/events.h"

#include <lexbor/dom/dom.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

/* the transient event object (defined here; whaleui.h keeps it opaque).
 * Must live at global scope so the handle type resolves. */
struct whaleui_dom_event
{
    lxb_dom_element* target;
    const char* type;
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
    ev->type = type;
    ev->prevented = 0;
    ev->stopped = 0;
    ev->key_code = 0;
    ev->mouse_x = ev->mouse_y = ev->mouse_button = 0;
    ev->wheel_x = ev->wheel_y = 0;
    return ev;
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
    lxb_dom_element* e = reinterpret_cast<lxb_dom_element*>(el);
    if (!e || !type || !cb) {
        return -1;
    }
    std::vector<Listener>& v = listeners()[e];
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].type == type && v[i].cb == cb && v[i].userdata == userdata) {
            return 0; /* duplicate: no-op success */
        }
    }
    Listener l;
    l.type = type;
    l.cb = cb;
    l.userdata = userdata;
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

extern "C" int whaleui_dom_dispatch_event_full(lxb_dom_element* el,
                                               const char* type,
                                               int key_code, int mouse_x,
                                               int mouse_y, int mouse_button,
                                               float wheel_x, float wheel_y)
{
    if (!el || !type) {
        return -1;
    }
    std::map<lxb_dom_element*, std::vector<Listener>>::iterator it =
        listeners().find(el);
    if (it == listeners().end() || it->second.empty()) {
        return 0; /* nothing listening: still a valid dispatch */
    }
    whaleui_dom_event_t* ev = make_event(el, type);
    ev->key_code = key_code;
    ev->mouse_x = mouse_x;
    ev->mouse_y = mouse_y;
    ev->mouse_button = mouse_button;
    ev->wheel_x = wheel_x;
    ev->wheel_y = wheel_y;
    /* copy the vector: a listener may remove itself while iterating */
    std::vector<Listener> v = it->second;
    for (size_t i = 0; i < v.size(); ++i) {
        if (ev->stopped) {
            break;
        }
        v[i].cb(reinterpret_cast<whaleui_dom_event_t*>(ev), v[i].userdata);
    }
    delete ev;
    return 0;
}

extern "C" int whaleui_dom_dispatch_event(whaleui_dom_element_t* el,
                                          const char* type)
{
    return whaleui_dom_dispatch_event_full(
        reinterpret_cast<lxb_dom_element*>(el), type, 0, 0, 0, 0, 0, 0);
}

/* --- event object accessors --- */

extern "C" whaleui_dom_element_t* whaleui_dom_event_target(const whaleui_dom_event_t* ev)
{
    return ev ? reinterpret_cast<whaleui_dom_element_t*>(ev->target) : nullptr;
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
