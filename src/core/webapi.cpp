/* Web platform APIs outside the DOM/render modules.
 *
 *  - matchMedia: MediaQueryList built on the style engine's @media evaluator
 *    (whaleui_style_media_ok), so a query answered here means the same thing
 *    the cascade means. Change listeners fire when the window resizes or the
 *    theme changes across the breakpoint.
 *  - CSS.supports: probed through the project's own CSS parser - a declaration
 *    is "supported" when the parser keeps it (the only honest answer, since
 *    the parser IS the supported subset).
 *  - screen / navigator: platform facts via SDL.
 *  - Web Storage: localStorage persists to a JSON file under the platform's
 *    user-data dir; sessionStorage is memory-only.
 *  - queueMicrotask / requestIdleCallback: queued and drained by the event
 *    loop (microtasks before timers, idle when nothing else is pending).
 *
 * Deliberately absent (documented in doc/external/09-api-web.md): Blob /
 * FileReader (the VFS already reads bytes; File covers drops), location /
 * history (navigation is load_uri), structuredClone (plain C data), Pointer /
 * Touch events (SDL gives mouse + gamepad; touch is a follow-up). */

#include "core/webapi.h"
#include "core/app.h"
#include "core/window.h"
#include "style/css.h"
#include "style/style.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

/* ======================= matchMedia ======================= */

struct MediaListener
{
    whaleui_media_query_cb cb;
    void* userdata;
};

struct whaleui_media_query_list
{
    std::string query;
    const whaleui_window_t* win; /* viewport source (may outlive? see destroy) */
    int matches;
    std::vector<MediaListener> listeners;
};

namespace {

std::vector<whaleui_media_query_list*>& media_lists()
{
    static std::vector<whaleui_media_query_list*> l;
    return l;
}

int eval_media(const whaleui_window_t* win, const std::string& query)
{
    int theme = win && win->app ? static_cast<int>(whaleui_app_resolved_theme(win->app))
                                : (int)WHALEUI_THEME_LIGHT;
    int w = win ? win->width : 0;
    int h = win ? win->height : 0;
    int reduced = (win && win->app) ? win->app->reduced_motion : 0;
    return whaleui_style_media_ok(query.c_str(), theme, w, reduced, h) ? 1 : 0;
}

} // namespace

extern "C" whaleui_media_query_list_t* whaleui_window_match_media(const whaleui_window_t* win,
                                                                  const char* query)
{
    if (!win || !query) {
        return nullptr;
    }
    whaleui_media_query_list_t* l = new whaleui_media_query_list_t;
    l->query = query;
    l->win = win;
    l->matches = eval_media(win, l->query);
    media_lists().push_back(l);
    return l;
}

extern "C" void whaleui_media_query_list_destroy(whaleui_media_query_list_t* list)
{
    if (!list) {
        return;
    }
    for (size_t i = 0; i < media_lists().size(); ++i) {
        if (media_lists()[i] == list) {
            media_lists().erase(media_lists().begin() + static_cast<long>(i));
            break;
        }
    }
    delete list;
}

extern "C" int whaleui_media_query_list_matches(const whaleui_media_query_list_t* list)
{
    return list ? list->matches : 0;
}

extern "C" const char* whaleui_media_query_list_media(const whaleui_media_query_list_t* list)
{
    return list ? list->query.c_str() : "";
}

extern "C" int whaleui_media_query_list_add_listener(whaleui_media_query_list_t* list,
                                                     whaleui_media_query_cb cb,
                                                     void* userdata)
{
    if (!list || !cb) {
        return -1;
    }
    for (size_t i = 0; i < list->listeners.size(); ++i) {
        if (list->listeners[i].cb == cb && list->listeners[i].userdata == userdata) {
            return 0; /* duplicate: no-op success */
        }
    }
    MediaListener l;
    l.cb = cb;
    l.userdata = userdata;
    list->listeners.push_back(l);
    return 0;
}

extern "C" int whaleui_media_query_list_remove_listener(whaleui_media_query_list_t* list,
                                                        whaleui_media_query_cb cb,
                                                        void* userdata)
{
    if (!list || !cb) {
        return -1;
    }
    for (size_t i = 0; i < list->listeners.size(); ++i) {
        if (list->listeners[i].cb == cb && list->listeners[i].userdata == userdata) {
            list->listeners.erase(list->listeners.begin() + static_cast<long>(i));
            return 0;
        }
    }
    return -2;
}

extern "C" void whaleui_webapi_refresh_media(void)
{
    std::vector<whaleui_media_query_list*>& all = media_lists();
    for (size_t i = 0; i < all.size(); ++i) {
        whaleui_media_query_list_t* l = all[i];
        if (!l->win) {
            continue;
        }
        int now = eval_media(l->win, l->query);
        if (now == l->matches) {
            continue;
        }
        l->matches = now;
        /* copy: a listener may add/remove listeners */
        std::vector<MediaListener> ls = l->listeners;
        for (size_t k = 0; k < ls.size(); ++k) {
            ls[k].cb(l, ls[k].userdata);
        }
    }
}

/* ======================= CSS.supports ======================= */

extern "C" int whaleui_css_supports(const char* property, const char* value)
{
    if (!property || !*property || !value || !*value) {
        return 0;
    }
    /* Two questions, both required:
     *  1. is the property one the engine reads? The CSS parser keeps unknown
     *     declarations on purpose, so probing it alone answered 1 for
     *     "no-such-property: 1px" - a support oracle that says yes to
     *     everything is worse than none. The supported set is an explicit
     *     table (whaleui_style_property_supported).
     *  2. does the VALUE survive the parser? That part IS honest through the
     *     parser: a malformed value is dropped from the declaration list.
     * Case: CSS property names are ASCII case-insensitive, so compare lowered. */
    std::string prop(property);
    for (size_t i = 0; i < prop.size(); ++i) {
        char c = prop[i];
        if (c >= 'A' && c <= 'Z') {
            prop[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (!whaleui_style_property_supported(prop.c_str())) {
        return 0;
    }
    /* the value is checked with the engine's own parsers (color/length/keyword
     * sets) instead of the permissive CSS parser, which keeps any value it is
     * handed - so "display: not-a-real-display" used to answer 1 as well. */
    return whaleui_style_value_supported(prop.c_str(), value) ? 1 : 0;
}

/* ======================= screen / navigator ======================= */

extern "C" int whaleui_window_screen_size(const whaleui_window_t* win, int* w, int* h)
{
    if (!w || !h) {
        return -1;
    }
    SDL_DisplayID did = 0;
    if (win && win->sdl) {
        did = SDL_GetDisplayForWindow(win->sdl);
    }
    if (!did) {
        did = SDL_GetPrimaryDisplay();
    }
    SDL_Rect b;
    b.x = 0; b.y = 0; b.w = 0; b.h = 0;
    if (did && SDL_GetDisplayBounds(did, &b)) {
        *w = b.w;
        *h = b.h;
        return 0;
    }
    return -2;
}

extern "C" int whaleui_window_screen_work_area(const whaleui_window_t* win, int* x, int* y,
                                               int* w, int* h)
{
    if (!x || !y || !w || !h) {
        return -1;
    }
    SDL_DisplayID did = 0;
    if (win && win->sdl) {
        did = SDL_GetDisplayForWindow(win->sdl);
    }
    if (!did) {
        did = SDL_GetPrimaryDisplay();
    }
    SDL_Rect b;
    b.x = 0; b.y = 0; b.w = 0; b.h = 0;
    if (did && SDL_GetDisplayUsableBounds(did, &b)) {
        *x = b.x; *y = b.y; *w = b.w; *h = b.h;
        return 0;
    }
    return -2;
}

extern "C" const char* whaleui_navigator_user_agent(void)
{
    /* A truthful product string (the engine is not a browser); pages sniffing
     * for a browser should feature-detect instead. */
    static std::string ua = "WhaleUI/" WHALEUI_VERSION " (SDL3)";
    return ua.c_str();
}

extern "C" const char* whaleui_navigator_platform(void)
{
    static std::string p = SDL_GetPlatform();
    return p.c_str();
}

extern "C" const char* whaleui_navigator_language(void)
{
    /* SDL exposes the system locale; return the language part ("en", "zh"). */
    static std::string lang;
    if (lang.empty()) {
        int count = 0;
        SDL_Locale** locales = SDL_GetPreferredLocales(&count);
        if (locales && count > 0 && locales[0] && locales[0]->language) {
            lang = locales[0]->language;
        } else {
            lang = "en";
        }
        SDL_free(locales);
    }
    return lang.c_str();
}

/* ======================= Web Storage ======================= */

namespace {

/* localStorage lives in one process-wide map persisted to a JSON-ish file;
 * sessionStorage is a separate memory-only map. Keys are namespaced by
 * "origin" (the engine has no origins: one global store, documented). */
std::map<std::string, std::string>& local_map()
{
    static std::map<std::string, std::string> m;
    return m;
}

std::map<std::string, std::string>& session_map()
{
    static std::map<std::string, std::string> m;
    return m;
}

std::string storage_path()
{
    const char* dir = SDL_GetPrefPath("WhaleUI", "WhaleUI");
    if (!dir) {
        return std::string();
    }
    std::string p = dir;
    SDL_free((void*)dir);
    p += "localStorage.txt";
    return p;
}

/* line-oriented storage: "key<TAB>value\n" (a value containing a newline is
 * escaped as \n, so the format round-trips) */
std::string esc(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            o += "\\n";
        } else if (s[i] == '\\') {
            o += "\\\\";
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string unesc(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            o += (s[i] == 'n') ? '\n' : s[i];
        } else {
            o += s[i];
        }
    }
    return o;
}

std::mutex& storage_mx()
{
    static std::mutex m;
    return m;
}

void local_flush()
{
    std::string path = storage_path();
    if (path.empty()) {
        return;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    for (std::map<std::string, std::string>::iterator it = local_map().begin();
         it != local_map().end(); ++it) {
        std::string line = esc(it->first) + "\t" + esc(it->second) + "\n";
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
}

void local_load()
{
    static bool loaded = false;
    if (loaded) {
        return;
    }
    loaded = true;
    std::string path = storage_path();
    if (path.empty()) {
        return;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return;
    }
    std::string all;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        all.append(buf, n);
    }
    std::fclose(f);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? std::string::npos
                                                                   : nl - pos);
        size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            local_map()[unesc(line.substr(0, tab))] = unesc(line.substr(tab + 1));
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
}

std::map<std::string, std::string>& pick(int session)
{
    return session ? session_map() : local_map();
}

std::string& ret_cache()
{
    static std::string s;
    return s;
}

} // namespace

extern "C" int whaleui_storage_length(int session)
{
    std::lock_guard<std::mutex> lk(storage_mx());
    if (!session) {
        local_load();
    }
    return static_cast<int>(pick(session).size());
}

extern "C" const char* whaleui_storage_key(int session, int index)
{
    std::lock_guard<std::mutex> lk(storage_mx());
    if (!session) {
        local_load();
    }
    std::map<std::string, std::string>& m = pick(session);
    if (index < 0 || static_cast<size_t>(index) >= m.size()) {
        return nullptr;
    }
    std::map<std::string, std::string>::iterator it = m.begin();
    std::advance(it, index);
    ret_cache() = it->first;
    return ret_cache().c_str();
}

extern "C" const char* whaleui_storage_get_item(int session, const char* key)
{
    if (!key) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(storage_mx());
    if (!session) {
        local_load();
    }
    std::map<std::string, std::string>& m = pick(session);
    std::map<std::string, std::string>::iterator it = m.find(key);
    if (it == m.end()) {
        return nullptr;
    }
    ret_cache() = it->second;
    return ret_cache().c_str();
}

extern "C" int whaleui_storage_set_item(int session, const char* key, const char* value)
{
    if (!key || !value) {
        return -1;
    }
    std::lock_guard<std::mutex> lk(storage_mx());
    if (!session) {
        local_load();
    }
    pick(session)[key] = value;
    if (!session) {
        local_flush();
    }
    return 0;
}

extern "C" int whaleui_storage_remove_item(int session, const char* key)
{
    if (!key) {
        return -1;
    }
    std::lock_guard<std::mutex> lk(storage_mx());
    if (!session) {
        local_load();
    }
    std::map<std::string, std::string>& m = pick(session);
    std::map<std::string, std::string>::iterator it = m.find(key);
    if (it == m.end()) {
        return -2;
    }
    m.erase(it);
    if (!session) {
        local_flush();
    }
    return 0;
}

extern "C" int whaleui_storage_clear(int session)
{
    std::lock_guard<std::mutex> lk(storage_mx());
    pick(session).clear();
    if (!session) {
        local_flush();
    }
    return 0;
}

/* ======================= microtasks / idle ======================= */

namespace {

struct WebTask
{
    whaleui_web_task_cb cb;
    void* userdata;
};

std::mutex& task_mx()
{
    static std::mutex m;
    return m;
}

std::vector<WebTask>& microtasks()
{
    static std::vector<WebTask> v;
    return v;
}

std::vector<WebTask>& idles()
{
    static std::vector<WebTask> v;
    return v;
}

} // namespace

extern "C" int whaleui_queue_microtask(whaleui_web_task_cb cb, void* userdata)
{
    if (!cb) {
        return -1;
    }
    std::lock_guard<std::mutex> lk(task_mx());
    WebTask t;
    t.cb = cb;
    t.userdata = userdata;
    microtasks().push_back(t);
    return 0;
}

extern "C" int whaleui_request_idle_callback(whaleui_web_task_cb cb, void* userdata)
{
    if (!cb) {
        return -1;
    }
    std::lock_guard<std::mutex> lk(task_mx());
    WebTask t;
    t.cb = cb;
    t.userdata = userdata;
    idles().push_back(t);
    return 0;
}

extern "C" int whaleui_webapi_has_microtasks(void)
{
    std::lock_guard<std::mutex> lk(task_mx());
    return microtasks().empty() ? 0 : 1;
}

extern "C" int whaleui_webapi_has_idle(void)
{
    std::lock_guard<std::mutex> lk(task_mx());
    return idles().empty() ? 0 : 1;
}

extern "C" int whaleui_webapi_pump_microtasks(void)
{
    /* A microtask may queue another microtask: keep draining (the browser
     * drains to empty before the next task). Bounded to avoid a runaway
     * self-queue wedging the loop - the cap is far above any real use. */
    int ran = 0;
    for (int round = 0; round < 64; ++round) {
        std::vector<WebTask> batch;
        {
            std::lock_guard<std::mutex> lk(task_mx());
            if (microtasks().empty()) {
                break;
            }
            batch.swap(microtasks());
        }
        for (size_t i = 0; i < batch.size(); ++i) {
            batch[i].cb(batch[i].userdata);
            ++ran;
        }
    }
    return ran;
}

extern "C" int whaleui_webapi_pump_idle(void)
{
    /* idle callbacks run when nothing else is pending: the loop only calls
     * this on an otherwise-quiet iteration (see app.cpp). */
    std::vector<WebTask> batch;
    {
        std::lock_guard<std::mutex> lk(task_mx());
        if (idles().empty()) {
            return 0;
        }
        batch.swap(idles());
    }
    for (size_t i = 0; i < batch.size(); ++i) {
        batch[i].cb(batch[i].userdata);
    }
    return static_cast<int>(batch.size());
}
