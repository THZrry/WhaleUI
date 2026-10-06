// test_style: CSS API contract tests.
#include "whaleui.h"
#include "style/css.h"  /* rule struct for element access in this white-box test */
#include "style/style.h" /* selector matching + cascade + var resolution */
#include "style/theme.h" /* built-in theme stylesheets + variable tables */
#include "test_util.h"

#include <lexbor/dom/dom.h>

#include <cassert>
#include <cstring>
#include <map>
#include <set>
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
        whaleui_style_state st;
        assert(whaleui_style_match(".card", card_el, &st));
        assert(whaleui_style_match("#card", card_el, &st));
        assert(whaleui_style_match("div.card", card_el, &st));
        assert(whaleui_style_match("body #card", card_el, &st));
        assert(whaleui_style_match("div .note", p_el, &st));
        assert(whaleui_style_match("body > div", card_el, &st));
        assert(!whaleui_style_match(".nope", card_el, &st));
        assert(!whaleui_style_match("span", card_el, &st));
        st.hover = p_el;
        assert(whaleui_style_match(".note:hover", p_el, &st));      /* hover active */
        st.hover = nullptr;
        assert(!whaleui_style_match(".note:hover", p_el, &st));     /* no hover */
        st.focus = p_el;
        assert(whaleui_style_match(".note:focus", p_el, &st));     /* focus active */
        assert(!whaleui_style_match(".note:focus", card_el, &st)); /* focus elsewhere */
        st.pressed = p_el;
        assert(whaleui_style_match(".note:active", p_el, &st));    /* pressed active */
        st.pressed = nullptr;
        assert(!whaleui_style_match(".note:active", p_el, &st));
        /* :not() - single-level negation (static + state forms) */
        assert(whaleui_style_match(".note:not(.nope)", p_el, &st));
        assert(!whaleui_style_match(".note:not(.note)", p_el, &st));
        assert(whaleui_style_match("p:not(span)", p_el, &st));
        assert(!whaleui_style_match("p:not(p)", p_el, &st));
        assert(whaleui_style_match(".note:not([missing])", p_el, &st));
        /* p is the only element child of #card: both first- and
         * last-child, so both negations fail */
        assert(!whaleui_style_match("p:not(:first-child)", p_el, &st));
        assert(!whaleui_style_match("p:not(:last-child)", p_el, &st));
        st.hover = p_el;
        assert(!whaleui_style_match(".note:not(:hover)", p_el, &st));
        st.hover = card_el;
        assert(whaleui_style_match(".note:not(:hover)", p_el, &st));
        st.hover = nullptr;
        assert(whaleui_style_match(".note:not(:hover)", p_el, &st));
        /* attribute substring operators */
        assert(whaleui_style_match("div[class^=\"ca\"]", card_el, &st));
        assert(whaleui_style_match("div[class$=\"ard\"]", card_el, &st));
        assert(whaleui_style_match("div[class*=\"ar\"]", card_el, &st));
        assert(!whaleui_style_match("div[class^=\"nope\"]", card_el, &st));
        assert(!whaleui_style_match("div[class$=\"nope\"]", card_el, &st));
        st = whaleui_style_state();
        /* regression: a bare tag selector must NOT match descendants */
        assert(!whaleui_style_match("body", card_el, &st));
        assert(!whaleui_style_match("html", card_el, &st));
        assert(!whaleui_style_match("div", p_el, &st));
        assert(!whaleui_style_match("div", body_el, &st));

    /* form/link/empty states + attr() content (own document) */
    {
        const char* html2 =
            "<html><body>"
            "<input id=\"a\" type=\"text\">"
            "<input id=\"b\" type=\"text\" disabled>"
            "<input id=\"c\" type=\"text\" required>"
            "<a id=\"d\" href=\"http://x/\">x</a>"
            "<a id=\"e\">y</a>"
            "<span id=\"f\"></span>"
            "<span id=\"g\" data-v=\"hello\">z</span>"
            "</body></html>";
        whaleui_dom_document_t* doc2 =
            whaleui_dom_parse_html(html2, std::strlen(html2));
        assert(doc2 != nullptr);
        whaleui_style_state st2;
        lxb_dom_element* a = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(doc2, "a"));
        lxb_dom_element* b = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(doc2, "b"));
        lxb_dom_element* c = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(doc2, "c"));
        lxb_dom_element* d = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(doc2, "d"));
        lxb_dom_element* e = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(doc2, "e"));
        lxb_dom_element* f = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(doc2, "f"));
        assert(a && b && c && d && e && f);
        assert(whaleui_style_match("input:enabled", a, &st2));
        assert(!whaleui_style_match("input:enabled", b, &st2));
        assert(whaleui_style_match("input:disabled", b, &st2));
        assert(whaleui_style_match("input:required", c, &st2));
        assert(whaleui_style_match("input:optional", a, &st2));
        assert(!whaleui_style_match("input:optional", c, &st2));
        assert(whaleui_style_match("a:link", d, &st2));
        assert(!whaleui_style_match("a:link", e, &st2)); /* no href */
        assert(whaleui_style_match("span:empty", f, &st2));
        assert(!whaleui_style_match("a:empty", d, &st2)); /* has text */
        assert(whaleui_style_match("input[type^=\"tex\"]", a, &st2));
        assert(whaleui_style_match("input[type$=\"ext\"]", a, &st2));
        assert(whaleui_style_match("input[type*=\"ex\"]", a, &st2));
        assert(!whaleui_style_match("input[type~=\"tex\"]", a, &st2));
        /* :is() - any inner match wins */
        lxb_dom_element* g = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_get_element_by_id(doc2, "g"));
        assert(g != nullptr);
        assert(whaleui_style_match("span:is(.nope, #g)", g, &st2));
        assert(!whaleui_style_match("span:is(.nope, #nope)", g, &st2));
        assert(whaleui_style_match("span:where(span)", g, &st2));
        /* :has() - parent/sibling conditionals (own document) */
        {
            const char* html3 =
                "<html><body>"
                "<div id=\"p1\"><span id=\"c1\">x</span></div>"
                "<div id=\"p2\"><em>y</em></div>"
                "<p id=\"s1\">a</p><b id=\"s2\">b</b>"
                "</body></html>";
            whaleui_dom_document_t* doc3 =
                whaleui_dom_parse_html(html3, std::strlen(html3));
            assert(doc3 != nullptr);
            whaleui_style_state st3;
            lxb_dom_element* p1 = reinterpret_cast<lxb_dom_element*>(
                whaleui_dom_get_element_by_id(doc3, "p1"));
            lxb_dom_element* p2 = reinterpret_cast<lxb_dom_element*>(
                whaleui_dom_get_element_by_id(doc3, "p2"));
            lxb_dom_element* s1 = reinterpret_cast<lxb_dom_element*>(
                whaleui_dom_get_element_by_id(doc3, "s1"));
            assert(p1 && p2 && s1);
            /* descendant */
            assert(whaleui_style_match("div:has(span)", p1, &st3));
            assert(!whaleui_style_match("div:has(span)", p2, &st3));
            /* child (direct only) */
            assert(whaleui_style_match("div:has(> span)", p1, &st3));
            assert(!whaleui_style_match("div:has(> em)", p1, &st3));
            assert(whaleui_style_match("div:has(> em)", p2, &st3));
            /* adjacent / general sibling */
            assert(whaleui_style_match("p:has(+ b)", s1, &st3));
            assert(whaleui_style_match("p:has(~ b)", s1, &st3));
            assert(!whaleui_style_match("p:has(+ span)", s1, &st3));
            /* negation of the conditional */
            assert(whaleui_style_match("div:not(:has(span))", p2, &st3));
            assert(!whaleui_style_match("div:not(:has(span))", p1, &st3));
            /* bare :has(span) matches any element with a span descendant */
            assert(whaleui_style_match(":has(span)", p1, &st3));
            assert(!whaleui_style_match(":has(span)", p2, &st3));
            whaleui_dom_document_destroy(doc3);
        }
        /* :focus-within - focus on the child matches the parent */
        lxb_dom_element* body2 = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_query_selector(doc2, "body"));
        assert(body2 != nullptr);
        st2.focus = a; /* focus the input */
        assert(whaleui_style_match("body:focus-within", body2, &st2));
        assert(!whaleui_style_match("span:focus-within", g, &st2));
        st2.focus = nullptr;
        assert(!whaleui_style_match("body:focus-within", body2, &st2));
        whaleui_dom_document_destroy(doc2);
    }

        /* media */
        assert(whaleui_style_media_ok(nullptr, WHALEUI_THEME_DARK, 800, 0));
        assert(whaleui_style_media_ok("(prefers-color-scheme: dark)", WHALEUI_THEME_DARK, 800, 0));
        assert(!whaleui_style_media_ok("(prefers-color-scheme: dark)", WHALEUI_THEME_LIGHT, 800, 0));
        assert(whaleui_style_media_ok("(min-width: 600px)", WHALEUI_THEME_LIGHT, 800, 0));
        assert(!whaleui_style_media_ok("(min-width: 600px)", WHALEUI_THEME_LIGHT, 400, 0));

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

        WhaleUIComputedStyle cs = whaleui_style_compute(card_el, rules, count, vars, nullptr);
        assert(cs["color"] == "purple");      /* !important wins */
        assert(cs["font-size"] == "12px");    /* inherited default from div */
        assert(cs["background"] == "#ffffff");/* var() resolved */
        assert(cs["display"].empty());        /* not set -> no key */

        /* inline style overrides non-important rules */
        assert(whaleui_dom_set_style(card, "color", "yellow") == 0);
        cs = whaleui_style_compute(card_el, rules, count, vars, nullptr);
        assert(cs["color"] == "purple"); /* !important still wins */
        assert(whaleui_dom_set_style(card, "font-size", "20px") == 0);
        cs = whaleui_style_compute(card_el, rules, count, vars, nullptr);
        assert(cs["font-size"] == "20px");

        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    /* shorthand expansion (font / border-top) + * + :last-child + sibling */
    {
        const char* css =
            "* { margin: 0; }\n"
            "body p + p { margin-top: 14px; }\n"
            ".last:last-child { color: red; }\n"
            ".f { font: 900 20px/1.5 \"Noto Serif SC\", serif; }\n"
            ".bt { border-top: 2px solid #ff0000; }\n"
            ".bb { border-bottom: 1px solid #00ff00; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<html><body><div class=\"f bt\">x</div>"
            "<p>a</p><p>b</p><p class=\"last bb\">c</p></body></html>",
            83);
        assert(doc != nullptr);
        std::map<std::string, std::string> vars;
        lxb_dom_element* body_el =
            reinterpret_cast<lxb_dom_element*>(whaleui_dom_query_selector(doc, "body"));
        lxb_dom_element* div_el =
            reinterpret_cast<lxb_dom_element*>(whaleui_dom_query_selector(doc, ".f"));
        assert(body_el != nullptr && div_el != nullptr);

        /* font shorthand expands to longhands */
        WhaleUIComputedStyle cs =
            whaleui_style_compute(div_el, rules, count, vars, nullptr);
        assert(cs["font-size"] == "20px");
        assert(cs["font-weight"] == "900");
        assert(cs["line-height"] == "1.5");
        assert(cs["font-family"].find("serif") != std::string::npos);
        assert(cs["font-family"].find("Noto Serif SC") != std::string::npos);

        /* border-top shorthand -> width + color */
        assert(cs["border-top-width"] == "2px");
        assert(cs["border-color"] == "#ff0000");

        /* universal selector applies */
        assert(cs["margin"] == "0");

        /* adjacent sibling: second p only */
        lxb_dom_element* p2 =
            reinterpret_cast<lxb_dom_element*>(whaleui_dom_query_selector(doc, "p"));
        lxb_dom_element* p3 = nullptr;
        lxb_dom_node* n2 = p2->node.next;
        while (n2) {
            if (n2->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                p3 = lxb_dom_interface_element(n2);
                break;
            }
            n2 = n2->next;
        }
        assert(p2 != nullptr && p3 != nullptr);
        cs = whaleui_style_compute(p2, rules, count, vars, nullptr);
        assert(cs.find("margin-top") == cs.end()); /* first p: no sibling rule */
        cs = whaleui_style_compute(p3, rules, count, vars, nullptr);
        assert(cs["margin-top"] == "14px");

        /* :last-child pseudo-class */
        lxb_dom_element* last =
            reinterpret_cast<lxb_dom_element*>(whaleui_dom_query_selector(doc, ".last"));
        assert(last != nullptr);
        cs = whaleui_style_compute(last, rules, count, vars, nullptr);
        assert(cs["color"] == "red");
        assert(cs["border-bottom-width"] == "1px");
        assert(cs["border-color"] == "#00ff00"); /* bb overrides bt color */

        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    /* ::selection background surfaces in the computed style */
    {
        const char* css = "::selection { background: #ff0000; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><p>hi</p></body>", 24);
        assert(doc != nullptr);
        std::map<std::string, std::string> vars;
        lxb_dom_element* p = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_query_selector(doc, "p"));
        assert(p != nullptr);
        WhaleUIComputedStyle cs =
            whaleui_style_compute(p, rules, count, vars, nullptr);
        assert(cs["selection-bg"] == "#ff0000");
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    /* built-in theme stylesheets: every theme parses cleanly, covers the
     * renderer's native-control vars, and the var tables define every
     * var() the stylesheet references (light + dark, default + custom
     * accent) */
    {
        int n = whaleui_theme_count();
        assert(n >= 7); /* fluent metro material aero gtk macos browser */
        for (int i = 0; i < n; ++i) {
            const char* name = whaleui_theme_name(i);
            assert(name && *name);
            assert(whaleui_theme_label(i) && *whaleui_theme_label(i));
            assert(std::strcmp(whaleui_theme_resolve(name), name) == 0);
            assert(std::strcmp(whaleui_theme_resolve("no-such-theme"),
                               "fluent") == 0);

            const char* css = whaleui_theme_default_css(name);
            assert(css && std::strlen(css) > 500);
            whaleui_css_rule_t* rules = nullptr;
            size_t count = 0;
            whaleui_css_keyframes_t kf = {nullptr, 0};
            assert(whaleui_css_parse_full(css, std::strlen(css), &rules,
                                          &count, &kf) == 0);
            assert(count > 40);
            assert(kf.count >= 4); /* wui-fade-in/rise/pulse/spin */

            /* UA baseline: block/inline/table/form-control defaults present */
            std::set<std::string> sels;
            for (size_t j = 0; j < count; ++j) {
                const char* s = whaleui_css_selector(&rules[j]);
                if (s) {
                    sels.insert(s);
                }
            }
            assert(sels.count("button"));
            assert(sels.count("input, select, textarea") ||
                   sels.count("input"));
            assert(sels.count("table"));
            assert(sels.count(".card"));
            assert(sels.count(".btn-primary"));

            /* var tables: renderer's native-control keys + every var()
             * referenced by the stylesheet resolve */
            for (int mode = 0; mode < 2; ++mode) {
                std::map<std::string, std::string> vars;
                whaleui_theme_vars(name,
                                   mode == 0 ? WHALEUI_THEME_LIGHT
                                             : WHALEUI_THEME_DARK,
                                   nullptr, vars);
                for (const char* k : {"--accent", "--field", "--border",
                                      "--card", "--bg", "--fg", "--btn-bg"}) {
                    assert(vars.count(k));
                }
                /* scan var(--x) references */
                const char* p = css;
                while ((p = std::strstr(p, "var(--")) != nullptr) {
                    p += 4; /* skip "var(", keep "--name" */
                    const char* e = std::strchr(p, ')');
                    assert(e && e - p < 64);
                    std::string key(p, static_cast<size_t>(e - p));
                    if (!vars.count(key) || vars[key].empty()) {
                        std::fprintf(stderr, "theme=%s mode=%d missing var %s\n",
                                     name, mode, key.c_str());
                    }
                    assert(vars.count(key) &&
                           !vars[key].empty()); /* defined + non-empty */
                    p = e;
                }
            }

            /* custom accent propagates to the accent family */
            std::map<std::string, std::string> vars;
            whaleui_theme_vars(name, WHALEUI_THEME_LIGHT, "#ff8800", vars);
            assert(vars["--accent"] == "#ff8800");

            whaleui_css_rules_destroy(rules, count);
            whaleui_css_keyframes_destroy(&kf);
        }
        /* theme ids are stable, index 0 = fluent */
        assert(std::strcmp(whaleui_theme_name(0), "fluent") == 0);
    }

    /* ::after rules must NOT leak into the element's own style: the
     * pseudo-element suffix makes the rule match nothing (a bare
     * "input::after { position:absolute; height:2px }" used to apply
     * position/height to every input and broke layout) */
    {
        const char* css =
            "input::after { content: \"\"; position: absolute; height: 2px; }\n"
            "input { color: red; }\n"
            "input:focus::after { transform: scaleX(1); }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><input></body>", 18);
        assert(doc != nullptr);
        std::map<std::string, std::string> vars;
        lxb_dom_element* input = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_query_selector(doc, "input"));
        assert(input != nullptr);
        WhaleUIComputedStyle cs =
            whaleui_style_compute(input, rules, count, vars, nullptr);
        assert(cs["color"] == "red");
        assert(cs.find("position") == cs.end());
        assert(cs.find("height") == cs.end());
        /* the ::after content still surfaces for paint via match_pseudo */
        whaleui_style_state st;
        int pseudo = 0;
        assert(whaleui_style_match_pseudo("input::after", input, &st,
                                          &pseudo) == 1 && pseudo == 2);
        assert(whaleui_style_match("input::after", input, &st) == 0);
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    /* ::selection colors ride as virtual selection-bg/selection-fg props */
    {
        const char* css =
            "::selection { background: #ff0000; color: #00ff00; }\n"
            "p { color: black; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><p>hi</p></body>", 18);
        assert(doc != nullptr);
        std::map<std::string, std::string> vars;
        lxb_dom_element* p = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_query_selector(doc, "p"));
        assert(p != nullptr);
        WhaleUIComputedStyle cs =
            whaleui_style_compute(p, rules, count, vars, nullptr);
        assert(cs["selection-bg"] == "#ff0000");
        assert(cs["selection-fg"] == "#00ff00");
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    /* :is() takes its most-specific argument; :where() is zero;
     * :not() likewise takes its argument (not the pseudo-class) */
    {
        const char* css =
            ".x { color: red; }\n"
            ":where(.x) { color: blue; }\n"
            ":is(#y, .x) { color: green; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><p class=\"x\" id=\"y\">hi</p></body>", 38);
        assert(doc != nullptr);
        std::map<std::string, std::string> vars;
        lxb_dom_element* p = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_query_selector(doc, "p"));
        assert(p != nullptr);
        WhaleUIComputedStyle cs =
            whaleui_style_compute(p, rules, count, vars, nullptr);
        /* :is(#y,.x) = (1,0,0) beats .x = (0,1,0) beats :where = (0,0,0) */
        assert(cs["color"] == "green");
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }
    {
        const char* css =
            ".x { color: red; }\n"
            ":where(#y) { color: blue; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><p class=\"x\" id=\"y\">hi</p></body>", 38);
        assert(doc != nullptr);
        std::map<std::string, std::string> vars;
        lxb_dom_element* p = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_query_selector(doc, "p"));
        assert(p != nullptr);
        WhaleUIComputedStyle cs =
            whaleui_style_compute(p, rules, count, vars, nullptr);
        /* :where(#y) is zero: the plain class wins (old code counted the
         * inner # and the colon, so blue won) */
        assert(cs["color"] == "red");
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }
    {
        const char* css =
            ".x { color: red; }\n"
            ":not(#z) { color: green; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><p class=\"x\">hi</p></body>", 26);
        assert(doc != nullptr);
        std::map<std::string, std::string> vars;
        lxb_dom_element* p = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_query_selector(doc, "p"));
        assert(p != nullptr);
        WhaleUIComputedStyle cs =
            whaleui_style_compute(p, rules, count, vars, nullptr);
        /* :not(#z) = (1,0,0): green wins */
        assert(cs["color"] == "green");
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    /* @media extended conditions (direct evaluator white-box) */
    {
        /* contain/isolation/will-change survive the cascade (dead hints
         * except contain:paint, which the paint pass clips on) */
        const char* css = "#u { contain: paint; isolation: isolate; "
                          "will-change: transform; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><div id=\"u\">x</div></body>", 27);
        assert(doc != nullptr);
        std::map<std::string, std::string> vars;
        lxb_dom_element* u = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_query_selector(doc, "#u"));
        assert(u != nullptr);
        WhaleUIComputedStyle cs =
            whaleui_style_compute(u, rules, count, vars, nullptr);
        std::printf("[contain] cascade contain=[%s] isolation=[%s] "
                    "will-change=[%s]\n",
                    cs["contain"].c_str(), cs["isolation"].c_str(),
                    cs["will-change"].c_str());
        std::fflush(stdout);
        assert(cs["contain"] == "paint");
        assert(cs["isolation"] == "isolate");
        assert(cs["will-change"] == "transform");
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }
    {
        /* width gates (pre-existing) */
        assert(whaleui_style_media_ok("(min-width: 100px)", 0, 200, 0, 400));
        assert(!whaleui_style_media_ok("(max-width: 100px)", 0, 200, 0, 400));
        /* height gates */
        assert(whaleui_style_media_ok("(min-height: 100px)", 0, 200, 0, 400));
        assert(!whaleui_style_media_ok("(min-height: 500px)", 0, 200, 0, 400));
        assert(whaleui_style_media_ok("(max-height: 500px)", 0, 200, 0, 400));
        assert(!whaleui_style_media_ok("(max-height: 100px)", 0, 200, 0, 400));
        /* unknown height: height-gated conditions fail closed */
        assert(!whaleui_style_media_ok("(min-height: 100px)", 0, 200, 0, -1));
        /* orientation (200x400 = portrait) */
        assert(whaleui_style_media_ok("(orientation: portrait)", 0, 200, 0,
                                      400));
        assert(!whaleui_style_media_ok("(orientation: landscape)", 0, 200, 0,
                                       400));
        assert(whaleui_style_media_ok("(orientation: landscape)", 0, 400, 0,
                                      200));
        /* aspect-ratio 1/2 on a 200x400 viewport */
        assert(whaleui_style_media_ok("(aspect-ratio: 1/2)", 0, 200, 0, 400));
        assert(!whaleui_style_media_ok("(aspect-ratio: 1/1)", 0, 200, 0, 400));
        assert(whaleui_style_media_ok("(min-aspect-ratio: 1/4)", 0, 200, 0,
                                      400));
        assert(!whaleui_style_media_ok("(min-aspect-ratio: 1/1)", 0, 200, 0,
                                       400));
        assert(whaleui_style_media_ok("(max-aspect-ratio: 1/1)", 0, 200, 0,
                                      400));
        /* desktop constants: hover + fine pointer match */
        assert(whaleui_style_media_ok("(hover: hover)", 0, 200, 0, 400));
        assert(!whaleui_style_media_ok("(hover: none)", 0, 200, 0, 400));
        assert(whaleui_style_media_ok("(pointer: fine)", 0, 200, 0, 400));
        assert(!whaleui_style_media_ok("(pointer: coarse)", 0, 200, 0, 400));
        /* resolution: 96dpi reference */
        assert(whaleui_style_media_ok("(min-resolution: 72dpi)", 0, 200, 0,
                                      400));
        assert(!whaleui_style_media_ok("(min-resolution: 300dpi)", 0, 200, 0,
                                       400));
        assert(whaleui_style_media_ok("(max-resolution: 300dpi)", 0, 200, 0,
                                      400));
        std::printf("[media] extended OK\n");
        std::fflush(stdout);
    }

    return 0;
}

