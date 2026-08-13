/* Window core: public C API implementation.
 * Step 2: contract implementation (state kept, real window = stub). */

#include "core/window.h"
#include "dom/dom.h"
#include "fs/fs.h"

#include <cstdlib>
#include <cstring>

namespace {
char* dup_str(const char* s)
{
    size_t n = std::strlen(s) + 1;
    char* d = static_cast<char*>(std::malloc(n));
    if (d) {
        std::memcpy(d, s, n);
    }
    return d;
}
} // namespace

extern "C" whaleui_window_t* whaleui_window_create(whaleui_app_t* app,
                                                   const char* title, int width, int height)
{
    if (!app || !title || width <= 0 || height <= 0) {
        return nullptr;
    }
    whaleui_window_t* win = new whaleui_window_t;
    win->app = app;
    win->title = dup_str(title);
    win->width = width;
    win->height = height;
    win->visible = 0;
    win->document = nullptr;
    return win;
}

extern "C" void whaleui_window_destroy(whaleui_window_t* win)
{
    if (!win) {
        return;
    }
    if (win->document) {
        whaleui_dom_document_destroy(win->document);
    }
    std::free(win->title);
    delete win;
}

extern "C" int whaleui_window_show(whaleui_window_t* win)
{
    if (!win) {
        return -1;
    }
    win->visible = 1;
    return 0;
}

extern "C" int whaleui_window_hide(whaleui_window_t* win)
{
    if (!win) {
        return -1;
    }
    win->visible = 0;
    return 0;
}

extern "C" int whaleui_window_close(whaleui_window_t* win)
{
    if (!win) {
        return -1;
    }
    win->visible = 0;
    return 0;
}

extern "C" int whaleui_window_set_title(whaleui_window_t* win, const char* title)
{
    if (!win || !title) {
        return -1;
    }
    char* t = dup_str(title);
    if (!t) {
        return -1;
    }
    std::free(win->title);
    win->title = t;
    return 0;
}

extern "C" const char* whaleui_window_get_title(const whaleui_window_t* win)
{
    return win ? win->title : nullptr;
}

extern "C" int whaleui_window_set_size(whaleui_window_t* win, int width, int height)
{
    if (!win || width <= 0 || height <= 0) {
        return -1;
    }
    win->width = width;
    win->height = height;
    return 0;
}

extern "C" int whaleui_window_get_size(const whaleui_window_t* win, int* w, int* h)
{
    if (!win || !w || !h) {
        return -1;
    }
    *w = win->width;
    *h = win->height;
    return 0;
}

extern "C" int whaleui_window_load_html(whaleui_window_t* win, const char* html)
{
    if (!win || !html) {
        return -1;
    }
    if (win->document) {
        whaleui_dom_document_destroy(win->document);
    }
    win->document = whaleui_dom_parse_html(html, std::strlen(html));
    return win->document ? 0 : -2;
}

extern "C" int whaleui_window_load_uri(whaleui_window_t* win, const char* uri)
{
    if (!win || !uri) {
        return -1;
    }
    char* buf = nullptr;
    size_t len = 0;
    if (whaleui_fs_load(uri, &buf, &len) != 0) {
        return -2;
    }
    int rc = whaleui_window_load_html(win, buf);
    std::free(buf);
    return rc;
}

extern "C" whaleui_dom_document_t* whaleui_window_get_document(whaleui_window_t* win)
{
    return win ? win->document : nullptr;
}

