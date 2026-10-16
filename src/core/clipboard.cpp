/* Clipboard: navigator.clipboard subset backed by SDL.
 *
 * SDL owns the system clipboard (SDL_GetClipboardText / SDL_SetClipboardText),
 * so the content is shared with the rest of the desktop - the same thing a
 * browser's clipboard access does. The browser API is async (promises); the
 * engine has no promise objects, so the pair is exposed synchronously plus a
 * callback form. The callback is queued onto the event loop rather than
 * invoked inline, so a caller observes the same "resolves later" ordering a
 * promise would give (no listener can be surprised by re-entrancy). */

#include "core/clipboard.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct PendingOp
{
    int write;                       /* 1 = write, 0 = read */
    std::string text;                /* for a write */
    whaleui_clipboard_text_cb cb;
    void* userdata;
};

std::vector<PendingOp>& pending()
{
    static std::vector<PendingOp> q;
    return q;
}

/* the last read/written text: the returned pointer must stay valid until the
 * next clipboard call (documented C contract) */
std::string& text_cache()
{
    static std::string s;
    return s;
}

} // namespace

extern "C" int whaleui_clipboard_has_text(void)
{
    return SDL_HasClipboardText() ? 1 : 0;
}

extern "C" const char* whaleui_clipboard_get_text(void)
{
    char* t = SDL_GetClipboardText();
    text_cache() = t ? t : "";
    SDL_free(t);
    return text_cache().c_str();
}

extern "C" int whaleui_clipboard_set_text(const char* text)
{
    if (!text) {
        return -1;
    }
    if (!SDL_SetClipboardText(text)) {
        return -2;
    }
    text_cache() = text;
    return 0;
}

extern "C" int whaleui_clipboard_read_text_async(whaleui_clipboard_text_cb cb,
                                                 void* userdata)
{
    if (!cb) {
        return -1;
    }
    PendingOp op;
    op.write = 0;
    op.cb = cb;
    op.userdata = userdata;
    pending().push_back(op);
    return 0;
}

extern "C" int whaleui_clipboard_write_text_async(const char* text,
                                                  whaleui_clipboard_text_cb cb,
                                                  void* userdata)
{
    if (!text) {
        return -1;
    }
    if (whaleui_clipboard_set_text(text) != 0) {
        return -2;
    }
    if (cb) {
        PendingOp op;
        op.write = 1;
        op.text = text;
        op.cb = cb;
        op.userdata = userdata;
        pending().push_back(op);
    }
    return 0;
}

/* called once per loop iteration (app.cpp): run the queued clipboard
 * callbacks. Readbacks hit the OS here, not at call time. */
extern "C" int whaleui_clipboard_pump(void)
{
    if (pending().empty()) {
        return 0;
    }
    std::vector<PendingOp> ops;
    ops.swap(pending());
    for (size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].write) {
            ops[i].cb(ops[i].text.c_str(), ops[i].userdata);
        } else {
            const char* t = whaleui_clipboard_get_text();
            ops[i].cb(t && *t ? t : nullptr, ops[i].userdata);
        }
    }
    return (int)ops.size();
}
