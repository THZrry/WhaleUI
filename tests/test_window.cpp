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
        whaleui_window_destroy(nullptr);
    }

    whaleui_app_destroy(app);
    return 0;
}

