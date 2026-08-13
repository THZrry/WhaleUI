// test_font: font API contract tests.
#include "whaleui.h"
#include "test_util.h"

#include <cassert>
#include <cstring>

int main(void)
{
    /* memory registration */
    {
        static const unsigned char fake_ttf[] = {0, 1, 0, 0, 0, 1, 0, 0};
        const char* fam = whaleui_font_register_memory(fake_ttf, sizeof(fake_ttf));
        assert(fam != nullptr);
        assert(std::strncmp(fam, "memory-", 7) == 0);

        assert(std::strstr(whaleui_font_list(), fam) != nullptr);
    }

    /* register from file via VFS */
    {
        const char* fam = whaleui_font_register(TEST_URI_RAW("tests/data/font.ttf"));
        assert(fam != nullptr);
        assert(std::strcmp(fam, "font") == 0); /* base name, no ext */

        assert(std::strstr(whaleui_font_list(), "font") != nullptr);
    }

    /* default font */
    {
        assert(std::strcmp(whaleui_font_get_default(), "sans-serif") == 0);
        assert(whaleui_font_set_default("font") == 0);
        assert(std::strcmp(whaleui_font_get_default(), "font") == 0);
        assert(whaleui_font_set_default(nullptr) != 0);
    }

    /* null-safety */
    {
        assert(whaleui_font_register(nullptr) == nullptr);
        assert(whaleui_font_register_memory(nullptr, 0) == nullptr);
        assert(whaleui_font_register_memory(nullptr, 16) == nullptr);
    }

    return 0;
}

