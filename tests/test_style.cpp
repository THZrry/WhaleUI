// test_style: CSS API contract tests.
#include "whaleui.h"
#include "style/css.h"  /* rule struct for element access in this white-box test */
#include "style/style.h" /* selector matching + cascade + var resolution */
#include "test_util.h"

#include <lexbor/dom/dom.h>

#include <cassert>
#include <cstring>
#include <map>
#include <string>

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

    /* comments, comma selectors, !important, keyframes, media */
    {
        const char* css = "/* a comment */\n"
                          ".a, .b { color: green; margin: 2px !important; }\n"
                          "@media (prefers-color-scheme: dark) { .a { color: white; } }\n"
                          "@keyframes spin { from { opacity: 0; } to { opacity: 1; } }\n"
                          ":root { --accent: #ff0000; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        whaleui_css_keyframes_t kf = {nullptr, 0};
        int rc = whaleui_css_parse_full(css, std::strlen(css), &rules, &count, &kf);
        assert(rc == 0);
        assert(count == 4); /* .a, .b -> 2; media rule; :root */
        assert(kf.count == 1);
        assert(std::strcmp(kf.items[0].name, "spin") == 0);
        assert(kf.items[0].frame_count == 2);

        /* comma-split rules carry the same decls */
        assert(std::strcmp(whaleui_css_selector(&rules[0]), ".a") == 0);
        assert(std::strcmp(whaleui_css_selector(&rules[1]), ".b") == 0);
        assert(std::strcmp(whaleui_css_get_property(&rules[0], "color"), "green") == 0);
        assert(std::strcmp(whaleui_css_get_property(&rules[0], "margin"), "2px") == 0);
        assert(rules[0].important == 1);

        /* media rule tagged */
        const whaleui_css_rule_t* media_rule = &rules[2];
        assert(media_rule->media != nullptr);
        assert(std::strstr(media_rule->media, "dark") != nullptr);

        /* :root carries custom property */
        const whaleui_css_rule_t* root_rule = &rules[3];
        assert(std::strcmp(root_rule->selector, ":root") == 0);
        assert(std::strcmp(whaleui_css_get_property(root_rule, "--accent"), "#ff0000") == 0);

        whaleui_css_rules_destroy(rules, count);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* selector matching + cascade + var resolution (white-box) */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<html><body><div id=\"card\" class=\"card\"><p class=\"note\">hi</p></div></body></html>",
            79);
        assert(doc != nullptr);
        whaleui_dom_element_t* card = whaleui_dom_get_element_by_id(doc, "card");
        assert(card != nullptr);
        whaleui_dom_element_t* p = whaleui_dom_query_selector(doc, ".note");
        assert(p != nullptr);
        whaleui_dom_element_t* body = whaleui_dom_query_selector(doc, "body");
        assert(body != nullptr);
        lxb_dom_element* card_el = reinterpret_cast<lxb_dom_element*>(card);
        lxb_dom_element* p_el = reinterpret_cast<lxb_dom_element*>(p);
        lxb_dom_element* body_el = reinterpret_cast<lxb_dom_element*>(body);

        /* matching */
        assert(whaleui_style_match(".card", card_el));
        assert(whaleui_style_match("#card", card_el));
        assert(whaleui_style_match("div.card", card_el));
        assert(whaleui_style_match("body #card", card_el));
        assert(whaleui_style_match("div .note", p_el));
        assert(whaleui_style_match("body > div", card_el));
        assert(!whaleui_style_match(".nope", card_el));
        assert(!whaleui_style_match("span", card_el));
        assert(whaleui_style_match(".note:hover", p_el)); /* pseudo stripped */
        /* regression: a bare tag selector must NOT match descendants */
        assert(!whaleui_style_match("body", card_el));
        assert(!whaleui_style_match("html", card_el));
        assert(!whaleui_style_match("div", p_el));
        assert(!whaleui_style_match("div", body_el));

        /* media */
        assert(whaleui_style_media_ok(nullptr, WHALEUI_THEME_DARK, 800));
        assert(whaleui_style_media_ok("(prefers-color-scheme: dark)", WHALEUI_THEME_DARK, 800));
        assert(!whaleui_style_media_ok("(prefers-color-scheme: dark)", WHALEUI_THEME_LIGHT, 800));
        assert(whaleui_style_media_ok("(min-width: 600px)", WHALEUI_THEME_LIGHT, 800));
        assert(!whaleui_style_media_ok("(min-width: 600px)", WHALEUI_THEME_LIGHT, 400));

        /* cascade: specificity + !important + inline */
        const char* css = "div { color: red; font-size: 12px; }\n"
                          ".card { color: blue; }\n"
                          "#card { color: green; }\n"
                          ".card { color: purple !important; }\n"
                          ":root { --bg: #ffffff; }\n"
                          ".card { background: var(--bg); }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);

        std::map<std::string, std::string> vars;
        whaleui_dom_element_t* html = whaleui_dom_document_element(doc);
        whaleui_style_collect_vars_full(reinterpret_cast<lxb_dom_element*>(html),
                                        rules, count, vars);
        assert(vars["--bg"] == "#ffffff");

        WhaleUIComputedStyle cs = whaleui_style_compute(card_el, rules, count, vars);
        assert(cs["color"] == "purple");      /* !important wins */
        assert(cs["font-size"] == "12px");    /* inherited default from div */
        assert(cs["background"] == "#ffffff");/* var() resolved */
        assert(cs["display"].empty());        /* not set -> no key */

        /* inline style overrides non-important rules */
        assert(whaleui_dom_set_style(card, "color", "yellow") == 0);
        cs = whaleui_style_compute(card_el, rules, count, vars);
        assert(cs["color"] == "purple"); /* !important still wins */
        assert(whaleui_dom_set_style(card, "font-size", "20px") == 0);
        cs = whaleui_style_compute(card_el, rules, count, vars);
        assert(cs["font-size"] == "20px");

        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    return 0;
}

