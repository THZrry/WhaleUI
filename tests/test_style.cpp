// test_style: CSS API contract tests.
#include "whaleui.h"
#include "style/css.h" /* rule struct for element access in this white-box test */
#include "test_util.h"

#include <cassert>
#include <cstring>

int main(void)
{
    /* parse declarations */
    {
        const char* css = ".card { width: 100px; color: red; }\n"
                          "#main { display: flex; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        int rc = whaleui_css_parse(&rules, &count, css, std::strlen(css));
        assert(rc == 0);
        assert(rules != nullptr);
        assert(count == 2);

        assert(std::strcmp(whaleui_css_selector(&rules[0]), ".card") == 0);
        assert(std::strcmp(whaleui_css_get_property(&rules[0], "width"), "100px") == 0);
        assert(std::strcmp(whaleui_css_get_property(&rules[0], "color"), "red") == 0);
        assert(whaleui_css_has_property(&rules[0], "width"));
        assert(!whaleui_css_has_property(&rules[0], "display"));

        assert(std::strcmp(whaleui_css_selector(&rules[1]), "#main") == 0);
        assert(std::strcmp(whaleui_css_get_property(&rules[1], "display"), "flex") == 0);

        whaleui_css_rules_destroy(rules, count);
    }

    /* empty / no rules */
    {
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, "", 0) == 0);
        assert(count == 0);
        assert(rules == nullptr);
        whaleui_css_rules_destroy(rules, count);
    }

    /* load from file via VFS */
    {
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        int rc = whaleui_css_load(&rules, &count, TEST_URI_RAW("tests/data/style.css"));
        assert(rc == 0);
        assert(count == 1);
        assert(std::strcmp(whaleui_css_get_property(&rules[0], "color"), "blue") == 0);
        whaleui_css_rules_destroy(rules, count);
    }

    /* null-safety */
    {
        assert(whaleui_css_parse(nullptr, nullptr, nullptr, 0) != 0);
        assert(whaleui_css_get_property(nullptr, "color") == nullptr);
        assert(whaleui_css_selector(nullptr) == nullptr);
        whaleui_css_rules_destroy(nullptr, 0);
    }

    return 0;
}

