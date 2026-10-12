// test_window: window API contract tests.
#include "whaleui.h"
#include "test_util.h"

#include <cassert>
#include <cstring>

int main(void)
{
    whaleui_app_t* app = whaleui_app_create();
    assert(app != nullptr);

    /* create */
    {
        whaleui_window_t* win = whaleui_window_create(app, "test", 800, 600);
        assert(win != nullptr);
        assert(std::strcmp(whaleui_window_get_title(win), "test") == 0);

        int w = 0, h = 0;
        assert(whaleui_window_get_size(win, &w, &h) == 0);
        assert(w == 800 && h == 600);

        /* set title/size */
        assert(whaleui_window_set_title(win, "renamed") == 0);
        assert(std::strcmp(whaleui_window_get_title(win), "renamed") == 0);
        assert(whaleui_window_set_size(win, 1024, 768) == 0);
        assert(whaleui_window_get_size(win, &w, &h) == 0);
        assert(w == 1024 && h == 768);

        /* show/hide: show needs a GPU backend (D3D11/Vulkan/GL); without one
         * it fails cleanly (-3) and the window stays in stub state */
        int rc = whaleui_window_show(win);
        assert(rc == 0 || rc == -3);
        assert(whaleui_window_hide(win) == 0);
        assert(whaleui_window_close(win) == 0);

        /* load html */
        assert(whaleui_window_load_html(win, "<div id=\"a\">x</div>") == 0);
        whaleui_dom_document_t* doc = whaleui_window_get_document(win);
        assert(doc != nullptr);
        assert(whaleui_dom_document_element(doc) != nullptr);

        /* load uri through VFS */
        assert(whaleui_window_load_uri(win, TEST_URI_RAW("tests/data/page.html")) == 0);
        assert(whaleui_window_get_document(win) != nullptr);

        /* missing file -> failure */
        assert(whaleui_window_load_uri(win, TEST_URI_RAW("tests/data/nope.html")) != 0);

        whaleui_window_destroy(win);
    }

    /* window extension wrappers: headless-safe (unshown window uses cache) */
    {
        whaleui_window_t* win = whaleui_window_create(app, "ext", 800, 600);
        assert(win != nullptr);
        assert(whaleui_window_is_visible(win) == 0);
        assert(whaleui_window_is_minimized(win) == 0);
        assert(whaleui_window_is_maximized(win) == 0);
        assert(whaleui_window_is_focused(win) == 0);
        int pw = 0, ph = 0;
        assert(whaleui_window_get_size_in_pixels(win, &pw, &ph) == 0);
        assert(pw == 800 && ph == 600);
        assert(whaleui_window_set_position(win, 10, 20) == 0);
        int px = -1, py = -1;
        assert(whaleui_window_get_position(win, &px, &py) == 0);
        assert(whaleui_window_center(win) == 0);
        assert(whaleui_window_set_min_size(win, 200, 150) == 0);
        int mw = 0, mh = 0;
        assert(whaleui_window_get_min_size(win, &mw, &mh) == 0);
        assert(mw == 200 && mh == 150);
        assert(whaleui_window_set_max_size(win, 1920, 1080) == 0);
        assert(whaleui_window_get_max_size(win, &mw, &mh) == 0);
        assert(mw == 1920 && mh == 1080);
        assert(whaleui_window_set_min_size(win, -1, 0) != 0);
        assert(whaleui_window_set_bordered(win, 0) == 0);
        assert(whaleui_window_set_bordered(win, 1) == 0);
        assert(whaleui_window_set_resizable(win, 0) == 0);
        assert(whaleui_window_set_resizable(win, 1) == 0);
        assert(whaleui_window_set_always_on_top(win, 1) == 0);
        assert(whaleui_window_set_always_on_top(win, 0) == 0);
        assert(whaleui_window_set_opacity(win, 0.8f) == 0);
        assert(whaleui_window_get_opacity(win) > 0.79f);
        assert(whaleui_window_get_opacity(win) < 0.81f);
        assert(whaleui_window_set_opacity(win, 2.0f) != 0);
        assert(whaleui_window_set_mouse_grab(win, 0) == 0);
        assert(whaleui_window_get_mouse_grab(win) == 0);
        assert(whaleui_window_set_keyboard_grab(win, 0) == 0);
        assert(whaleui_window_get_keyboard_grab(win) == 0);
        assert(whaleui_window_set_focusable(win, 1) == 0);
        assert(whaleui_window_set_fullscreen(win, WHALEUI_FULLSCREEN_OFF) == 0);
        assert(whaleui_window_get_fullscreen(win) == WHALEUI_FULLSCREEN_OFF);
        assert(whaleui_window_display_id(win) == -1);
        assert(whaleui_window_display_scale(win) == 1.0f);
        assert(whaleui_window_set_event_callback(win, nullptr, nullptr) == 0);
        assert(whaleui_window_set_hit_test(win, nullptr, nullptr) == 0);
        assert(whaleui_window_flash(win, WHALEUI_FLASH_BRIEFLY) != 0);
        assert(whaleui_window_set_parent(win, nullptr) == 0);
        whaleui_window_destroy(win);
    }

    /* invalid args */
    {
        assert(whaleui_window_create(nullptr, "t", 10, 10) == nullptr);
        assert(whaleui_window_create(app, nullptr, 10, 10) == nullptr);
        assert(whaleui_window_create(app, "t", 0, 10) == nullptr);
        assert(whaleui_window_get_title(nullptr) == nullptr);
        assert(whaleui_window_set_title(nullptr, "x") != 0);
        assert(whaleui_window_set_size(nullptr, 1, 1) != 0);
        assert(whaleui_window_get_size(nullptr, nullptr, nullptr) != 0);
        assert(whaleui_window_load_html(nullptr, "<div></div>") != 0);
        assert(whaleui_window_load_uri(nullptr, "file://x") != 0);
        assert(whaleui_window_is_visible(nullptr) == 0);
        assert(whaleui_window_get_size_in_pixels(nullptr, nullptr, nullptr) != 0);
        assert(whaleui_window_set_event_callback(nullptr, nullptr, nullptr) != 0);
        assert(whaleui_window_set_hit_test(nullptr, nullptr, nullptr) != 0);
        whaleui_window_destroy(nullptr);
    }

    whaleui_app_destroy(app);
    return 0;
}

