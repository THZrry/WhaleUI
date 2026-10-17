/* MutationObserver.
 *
 * The DOM mutation APIs record into a pending queue; the observer pump (one
 * call per loop iteration, before the frame) matches each record against the
 * live observers and delivers a batch, then clears. That gives the browser's
 * essential contract - records are batched per turn, not delivered inline -
 * which is what pages rely on (a listener that mutates the DOM in response
 * sees its own mutation in the NEXT batch, so there is no re-entrancy).
 *
 * Each record carries the type ("attributes"/"characterData"/"childList"),
 * the target element, and the attribute name for attribute records; childList
 * records report whether nodes were added and/or removed. `attributeOldValue`
 * and `characterDataOldValue` are not tracked (documented).
 *
 * Scope: observe(el) watches el's SUBTREE; observe(NULL) watches the whole
 * document. An observer created without a document watches nothing. */

#include "dom/observers.h"
#include "dom/dom.h"
#include "render/render.h"

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Record
{
    int type;              /* 0 attributes, 1 characterData, 2 childList */
    lxb_dom_element* target;
    lxb_dom_document* doc;
    std::string attr;      /* attribute name (type 0) */
    int added;
    int removed;
};

struct Observer
{
    whaleui_dom_document_t* doc;
    lxb_dom_element* scope;   /* NULL = whole document */
    whaleui_mutation_cb cb;
    void* userdata;
    /* queued records for this observer (built at pump time) */
    std::vector<Record> batch;
    int delivering;
};

std::vector<Record>& pending()
{
    static std::vector<Record> p;
    return p;
}

std::vector<Observer*>& observers()
{
    static std::vector<Observer*> o;
    return o;
}

/* is `el` inside `scope` (or equal)? scope NULL means "the whole document",
 * checked by comparing owner documents instead. */
bool in_scope(lxb_dom_element* el, lxb_dom_element* scope)
{
    if (!scope) {
        return true;
    }
    for (lxb_dom_node* n = &el->node; n; n = n->parent) {
        if (n == &scope->node) {
            return true;
        }
    }
    return false;
}

} // namespace

extern "C" void whaleui_dom_notify_mutation(lxb_dom_element* target, int type,
                                            const char* attr, int added,
                                            int removed)
{
    if (!target) {
        return;
    }
    if (observers().empty()) {
        return; /* nobody watching: keep the queue empty */
    }
    Record r;
    r.type = type;
    r.target = target;
    r.doc = target->node.owner_document;
    r.attr = attr ? attr : "";
    r.added = added;
    r.removed = removed;
    pending().push_back(r);
}

extern "C" int whaleui_dom_observers_pending(void)
{
    return pending().empty() ? 0 : 1;
}

extern "C" int whaleui_dom_observers_pump(void)
{
    if (pending().empty() || observers().empty()) {
        pending().clear();
        return 0;
    }
    std::vector<Record> recs;
    recs.swap(pending());
    /* match each record to the observers that watch it. An observer whose
     * callback is already running (delivering) does not receive its own
     * mutation in the same turn - the browser queues it for the next batch. */
    int delivered = 0;
    for (size_t oi = 0; oi < observers().size(); ++oi) {
        Observer* o = observers()[oi];
        /* !cb = disconnected: it stays in the registry (cheap, and it may be
         * re-observed) but must never be called - the callback is NULL, so
         * delivering to it would jump to address 0. */
        if (o->delivering || !o->doc || !o->cb) {
            continue;
        }
        o->batch.clear();
        lxb_dom_document* want =
            reinterpret_cast<lxb_html_document*>(o->doc)
                ? &reinterpret_cast<lxb_html_document*>(o->doc)->dom_document
                : nullptr;
        for (size_t i = 0; i < recs.size(); ++i) {
            if (recs[i].doc != want) {
                continue;
            }
            /* scope NULL = the whole document; otherwise the element's
             * subtree (the scope element itself included) */
            if (o->scope && !in_scope(recs[i].target, o->scope)) {
                continue;
            }
            o->batch.push_back(recs[i]);
        }
        if (o->batch.empty()) {
            continue;
        }
        /* deliver a copy: the callback may mutate the DOM (queuing new
         * records) or remove the observer */
        std::vector<Record> out = o->batch;
        o->batch.clear();
        o->delivering = 1;
        for (size_t i = 0; i < out.size(); ++i) {
            whaleui_dom_element_t* target =
                reinterpret_cast<whaleui_dom_element_t*>(out[i].target);
            o->cb(target, out[i].type, out[i].attr.empty() ? nullptr
                                                          : out[i].attr.c_str(),
                  out[i].added, out[i].removed, o->userdata);
            ++delivered;
        }
        o->delivering = 0;
    }
    return delivered;
}

/* ======================= public API ======================= */

extern "C" whaleui_mutation_observer_t* whaleui_dom_create_mutation_observer(whaleui_dom_document_t* doc)
{
    Observer* o = new Observer;
    o->doc = doc;
    o->scope = nullptr;
    o->cb = nullptr;
    o->userdata = nullptr;
    o->delivering = 0;
    observers().push_back(o);
    return reinterpret_cast<whaleui_mutation_observer_t*>(o);
}

extern "C" void whaleui_dom_mutation_observer_destroy(whaleui_mutation_observer_t* obs)
{
    Observer* o = reinterpret_cast<Observer*>(obs);
    if (!o) {
        return;
    }
    for (size_t i = 0; i < observers().size(); ++i) {
        if (observers()[i] == o) {
            observers().erase(observers().begin() + static_cast<long>(i));
            break;
        }
    }
    delete o;
}

extern "C" int whaleui_dom_mutation_observer_observe(whaleui_mutation_observer_t* obs,
                                                     whaleui_dom_element_t* el,
                                                     whaleui_mutation_cb cb,
                                                     void* userdata)
{
    Observer* o = reinterpret_cast<Observer*>(obs);
    if (!o || !cb) {
        return -1;
    }
    /* el NULL = the whole document; otherwise the element's subtree */
    o->scope = el ? reinterpret_cast<lxb_dom_element*>(el) : nullptr;
    o->cb = cb;
    o->userdata = userdata;
    return 0;
}

extern "C" int whaleui_dom_mutation_observer_disconnect(whaleui_mutation_observer_t* obs)
{
    Observer* o = reinterpret_cast<Observer*>(obs);
    if (!o) {
        return -1;
    }
    o->cb = nullptr;
    o->userdata = nullptr;
    o->scope = nullptr;
    o->batch.clear();
    return 0;
}

extern "C" int whaleui_dom_mutation_observer_take_records(whaleui_mutation_observer_t* obs)
{
    Observer* o = reinterpret_cast<Observer*>(obs);
    if (!o) {
        return 0;
    }
    int n = static_cast<int>(o->batch.size());
    o->batch.clear();
    return n;
}

/* ======================= ResizeObserver =======================
 *
 * The engine's layout tree is rebuilt per frame; a ResizeObserver therefore
 * works by sampling: once per frame, each observed element's laid-out
 * border-box is compared with the size last delivered, and a change fires the
 * callback. That gives the browser's observable behavior (fires after layout,
 * once per size change) without a layout-side hook, at the cost of one tree
 * lookup per observed element per frame - which only happens when an observer
 * exists at all. */

namespace {

struct ResizeObs
{
    whaleui_dom_document_t* doc;
    struct Entry { lxb_dom_element* el; float w, h; int seen; };
    std::vector<Entry> entries;
    whaleui_resize_cb cb;
    void* userdata;
    int in_cb;
};

std::vector<ResizeObs*>& resize_observers()
{
    static std::vector<ResizeObs*> v;
    return v;
}

/* the element's border box from the most recent layout, or false when it is
 * not in the tree (display:none / detached). by_el, not an arena scan: an
 * incremental relayout leaves the replaced nodes in the arena, so the scan
 * reported a stale size and ResizeObserver stopped firing after an edit. */
bool element_box(lxb_dom_element* el, float* w, float* h)
{
    whaleui_layout_tree_t* tree = whaleui_render_last_tree();
    if (!tree || !el) {
        return false;
    }
    std::unordered_map<lxb_dom_element*, whaleui_layout_node_t*>::const_iterator
        it = tree->by_el.find(el);
    if (it == tree->by_el.end() || it->second->is_text) {
        return false;
    }
    *w = static_cast<float>(it->second->border.w);
    *h = static_cast<float>(it->second->border.h);
    return true;
}

} // namespace

extern "C" int whaleui_dom_resize_observers_pending(void)
{
    /* Only an UNDELIVERED initial size holds the loop awake: once every
     * observed element has been reported once, a ResizeObserver must not keep
     * the app spinning - it fires on the next change like the browser. */
    for (size_t oi = 0; oi < resize_observers().size(); ++oi) {
        ResizeObs* o = resize_observers()[oi];
        if (!o->cb) {
            continue;
        }
        for (size_t i = 0; i < o->entries.size(); ++i) {
            if (!o->entries[i].seen) {
                return 1;
            }
        }
    }
    return 0;
}

extern "C" int whaleui_dom_resize_observers_pump(void)
{
    int fired = 0;
    for (size_t oi = 0; oi < resize_observers().size(); ++oi) {
        ResizeObs* o = resize_observers()[oi];
        if (!o->cb || o->in_cb) {
            continue;
        }
        for (size_t i = 0; i < o->entries.size(); ++i) {
            float w = 0, h = 0;
            if (!element_box(o->entries[i].el, &w, &h)) {
                continue; /* not laid out: no size to report */
            }
            if (o->entries[i].seen && w == o->entries[i].w &&
                h == o->entries[i].h) {
                continue;
            }
            o->entries[i].w = w;
            o->entries[i].h = h;
            o->entries[i].seen = 1;
            o->in_cb = 1;
            o->cb(reinterpret_cast<whaleui_dom_element_t*>(o->entries[i].el),
                  w, h, o->userdata);
            o->in_cb = 0;
            ++fired;
        }
    }
    return fired;
}

extern "C" whaleui_resize_observer_t* whaleui_dom_create_resize_observer(whaleui_dom_document_t* doc)
{
    ResizeObs* o = new ResizeObs;
    o->doc = doc;
    o->cb = nullptr;
    o->userdata = nullptr;
    o->in_cb = 0;
    resize_observers().push_back(o);
    return reinterpret_cast<whaleui_resize_observer_t*>(o);
}

extern "C" void whaleui_dom_resize_observer_destroy(whaleui_resize_observer_t* obs)
{
    ResizeObs* o = reinterpret_cast<ResizeObs*>(obs);
    if (!o) {
        return;
    }
    for (size_t i = 0; i < resize_observers().size(); ++i) {
        if (resize_observers()[i] == o) {
            resize_observers().erase(resize_observers().begin() +
                                     static_cast<long>(i));
            break;
        }
    }
    delete o;
}

extern "C" int whaleui_dom_resize_observer_observe(whaleui_resize_observer_t* obs,
                                                   whaleui_dom_element_t* el,
                                                   whaleui_resize_cb cb,
                                                   void* userdata)
{
    ResizeObs* o = reinterpret_cast<ResizeObs*>(obs);
    if (!o || !el || !cb) {
        return -1;
    }
    lxb_dom_element* e = reinterpret_cast<lxb_dom_element*>(el);
    for (size_t i = 0; i < o->entries.size(); ++i) {
        if (o->entries[i].el == e) {
            o->cb = cb; /* re-observing updates the callback */
            o->userdata = userdata;
            return 0;
        }
    }
    ResizeObs::Entry en;
    en.el = e;
    en.w = 0;
    en.h = 0;
    en.seen = 0; /* the first pump delivers the initial size */
    o->entries.push_back(en);
    o->cb = cb;
    o->userdata = userdata;
    return 0;
}

extern "C" int whaleui_dom_resize_observer_unobserve(whaleui_resize_observer_t* obs,
                                                     whaleui_dom_element_t* el)
{
    ResizeObs* o = reinterpret_cast<ResizeObs*>(obs);
    if (!o || !el) {
        return -1;
    }
    lxb_dom_element* e = reinterpret_cast<lxb_dom_element*>(el);
    for (size_t i = 0; i < o->entries.size(); ++i) {
        if (o->entries[i].el == e) {
            o->entries.erase(o->entries.begin() + static_cast<long>(i));
            return 0;
        }
    }
    return -2;
}

extern "C" int whaleui_dom_resize_observer_disconnect(whaleui_resize_observer_t* obs)
{
    ResizeObs* o = reinterpret_cast<ResizeObs*>(obs);
    if (!o) {
        return -1;
    }
    o->entries.clear();
    o->cb = nullptr;
    o->userdata = nullptr;
    return 0;
}
