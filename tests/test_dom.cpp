// test_dom: DOM API contract tests.
#include "whaleui.h"
#include "dom/events.h" /* internal three-phase + non-bubbling dispatch */

#include <cassert>
#include <cstring>
#include <string>

/* Regression: document/stylesheet encodings that Windows editors write.
 *
 * A UTF-16 file ("Unicode" in Notepad) used to be decoded as UTF-8: 0xFF/0xFE
 * are illegal lead bytes, every 0x00 suffix reads as a string terminator, so
 * the whole buffer collapsed to its first ASCII character (a 414-line UTF-16
 * log rendered as a single "C"). The length handed to the parser must be the
 * REAL byte count too - strlen() stops at the first NUL, which is how the
 * conversion saw only three bytes and produced exactly one character.
 *
 * The two cases below cover both halves: the BOM conversion at the parser
 * edge and the byte count surviving the load path. */
static void test_dom_utf16_and_bom_input(void)
{
    /* "<p id=\"u\">A你</p>" as UTF-16LE + BOM (what Notepad writes) */
    static const unsigned char u16le[] = {
        0xFF, 0xFE, '<', 0, 'p', 0, ' ', 0, 'i', 0, 'd', 0, '=', 0, '"', 0,
        'u',  0,    '"', 0, '>', 0, 'A', 0, 0x60, 0x4F, '<', 0, '/', 0, 'p',
        0,    '>',  0};
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            reinterpret_cast<const char*>(u16le), sizeof(u16le));
        assert(doc != nullptr);
        /* before the fix this document degenerated to a single "C"-style
         * character and the element never existed */
        assert(whaleui_dom_get_element_by_id(doc, "u") != nullptr);
        whaleui_dom_document_destroy(doc);
    }

    /* same markup as UTF-16BE + BOM */
    static const unsigned char u16be[] = {
        0xFE, 0xFF, 0, '<', 0, 'p', 0, ' ', 0, 'i', 0, 'd', 0, '=', 0, '"',
        0,    'u',  0,   '"', 0, '>', 0, 'A', 0x4F, 0x60, 0, '<', 0, '/', 0,
        'p',  0,    '>', 0};
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            reinterpret_cast<const char*>(u16be), sizeof(u16be));
        assert(doc != nullptr);
        assert(whaleui_dom_get_element_by_id(doc, "u") != nullptr);
        whaleui_dom_document_destroy(doc);
    }

    /* UTF-8 BOM: the bytes must be skipped, not parsed as leading text (a
     * leading text node before <html> also broke stylesheet scoping) */
    {
        const char* utf8bom = "\xEF\xBB\xBF<p id=\"b\">hi</p>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(utf8bom, std::strlen(utf8bom));
        assert(doc != nullptr);
        assert(whaleui_dom_get_element_by_id(doc, "b") != nullptr);
        whaleui_dom_document_destroy(doc);
    }
}

int main(void)
{
    /* parse + document element */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html("<div id=\"box\">hi</div>", 23);
        assert(doc != nullptr);

        whaleui_dom_element_t* root = whaleui_dom_document_element(doc);
        assert(root != nullptr);
        assert(std::strcmp(whaleui_dom_tag_name(root), "html") == 0);

        whaleui_dom_document_destroy(doc);
    }

    /* create/append/query */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html("", 0);
        assert(doc != nullptr);

        whaleui_dom_element_t* box = whaleui_dom_create_element(doc, "div");
        assert(box != nullptr);
        assert(std::strcmp(whaleui_dom_tag_name(box), "div") == 0);

        /* lexbor always builds html > head/body; attach into body */
        whaleui_dom_element_t* body = whaleui_dom_query_selector(doc, "body");
        assert(body != nullptr);
        assert(whaleui_dom_append_child(body, box) == 0);
        assert(whaleui_dom_parent(box) == body);
        assert(whaleui_dom_first_child(body) == box);

        /* id query */
        assert(whaleui_dom_set_attribute(box, "id", "box") == 0);
        assert(whaleui_dom_get_element_by_id(doc, "box") == box);
        assert(whaleui_dom_query_selector(doc, "#box") == box);

        /* remove */
        assert(whaleui_dom_remove_child(body, box) == 0);
        assert(whaleui_dom_parent(box) == nullptr);
        assert(whaleui_dom_first_child(body) == nullptr);
        assert(whaleui_dom_get_element_by_id(doc, "box") == nullptr);

        whaleui_dom_element_destroy(box);
        whaleui_dom_document_destroy(doc);
    }

    /* attributes/text/style */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html("", 0);
        whaleui_dom_element_t* el = whaleui_dom_create_element(doc, "div");
        assert(el != nullptr);

        assert(whaleui_dom_set_attribute(el, "class", "theme-card") == 0);
        assert(std::strcmp(whaleui_dom_get_attribute(el, "class"), "theme-card") == 0);

        assert(whaleui_dom_set_text(el, "hello") == 0);
        assert(std::strcmp(whaleui_dom_get_text(el), "hello") == 0);

        assert(whaleui_dom_set_style(el, "width", "100px") == 0);
        assert(std::strcmp(whaleui_dom_get_style(el, "width"), "100px") == 0);

        /* CSSStyleDeclaration C-port: set/get/remove/cssText/length/item */
        assert(whaleui_dom_style_set_property(el, "height", "50px") == 0);
        char buf[64];
        assert(whaleui_dom_style_get_property(el, "height", buf, sizeof(buf)) == 0);
        assert(std::strcmp(buf, "50px") == 0);
        assert(whaleui_dom_style_length(el) == 2);
        char name[64];
        assert(whaleui_dom_style_item(el, 0, name, sizeof(name)) == 0);
        assert(whaleui_dom_style_remove_property(el, "height") == 0);
        assert(whaleui_dom_style_get_property(el, "height", buf, sizeof(buf)) != 0);
        assert(whaleui_dom_style_length(el) == 1);
        char css[128];
        assert(whaleui_dom_style_get_css_text(el, css, sizeof(css)) == 0);
        assert(whaleui_dom_style_set_css_text(el, "color: red; margin: 0") == 0);
        assert(whaleui_dom_style_length(el) == 2);
        assert(whaleui_dom_style_set_css_text(nullptr, "x") != 0);
        assert(whaleui_dom_style_item(el, 99, name, sizeof(name)) != 0);
        /* computed style snapshot (falls back to inline before layout) */
        whaleui_style_declaration_t* cs = whaleui_dom_get_computed_style(el, nullptr);
        assert(cs != nullptr);
        assert(whaleui_style_declaration_length(cs) >= 1);
        assert(whaleui_style_declaration_get(cs, "color", buf, sizeof(buf)) == 0);
        assert(std::strcmp(buf, "red") == 0);
        whaleui_style_declaration_destroy(cs);
        whaleui_style_declaration_destroy(nullptr);

        /* unknown key -> null */
        assert(whaleui_dom_get_attribute(el, "nope") == nullptr);
        assert(whaleui_dom_get_style(el, "nope") == nullptr);

        /* boolean attributes: presence without a value (<details open>) */
        assert(!whaleui_dom_has_attribute(el, "open"));
        assert(whaleui_dom_toggle_attribute(el, "open", -1) == 1);
        assert(whaleui_dom_has_attribute(el, "open"));
        assert(whaleui_dom_get_attribute(el, "open") == nullptr ||
               std::strcmp(whaleui_dom_get_attribute(el, "open"), "") == 0);
        assert(whaleui_dom_toggle_attribute(el, "open", -1) == 0);
        assert(!whaleui_dom_has_attribute(el, "open"));
        assert(whaleui_dom_toggle_attribute(el, "open", 1) == 1);
        assert(whaleui_dom_has_attribute(el, "open"));
        assert(whaleui_dom_remove_attribute(el, "open") == 0);
        assert(!whaleui_dom_has_attribute(el, "open"));

        whaleui_dom_element_destroy(el);
        whaleui_dom_document_destroy(doc);
    }

    /* real lexbor parsing: tree structure, classes, text */
    {
        const char* html = "<html><body><div id=\"card\" class=\"theme-card\">"
                           "<h1>Title</h1><p class=\"note\">Hello <strong>world</strong></p>"
                           "</div></body></html>";
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);

        /* document element is html */
        whaleui_dom_element_t* root = whaleui_dom_document_element(doc);
        assert(root != nullptr);
        assert(std::strcmp(whaleui_dom_tag_name(root), "html") == 0);

        /* id lookup works anywhere in the tree */
        whaleui_dom_element_t* card = whaleui_dom_get_element_by_id(doc, "card");
        assert(card != nullptr);
        assert(std::strcmp(whaleui_dom_tag_name(card), "div") == 0);
        assert(std::strcmp(whaleui_dom_get_attribute(card, "class"), "theme-card") == 0);

        /* class selector */
        whaleui_dom_element_t* note = whaleui_dom_query_selector(doc, ".note");
        assert(note != nullptr);
        assert(std::strcmp(whaleui_dom_tag_name(note), "p") == 0);

        /* descendant selector: div .note */
        assert(whaleui_dom_query_selector(doc, "div .note") == note);
        /* card IS the div, not its descendant: body #card matches, div #card does not */
        assert(whaleui_dom_query_selector(doc, "body #card") == card);
        assert(whaleui_dom_query_selector(doc, "div #card") == nullptr);
        assert(whaleui_dom_query_selector(doc, "span") == nullptr);

        /* text: element text content */
        assert(whaleui_dom_get_text(card) != nullptr);
        assert(std::strcmp(whaleui_dom_get_text(card), "TitleHello world") == 0);

        /* parent/child/sibling traversal */
        assert(whaleui_dom_parent(card) == whaleui_dom_query_selector(doc, "body"));
        whaleui_dom_element_t* h1 = whaleui_dom_query_selector(doc, "h1");
        assert(h1 != nullptr);
        assert(whaleui_dom_first_child(card) == h1);
        whaleui_dom_element_t* p = whaleui_dom_query_selector(doc, "p");
        assert(p != nullptr);
        assert(whaleui_dom_next_sibling(h1) == p);

        /* attribute mutation reflects in queries */
        assert(whaleui_dom_set_attribute(card, "id", "renamed") == 0);
        assert(whaleui_dom_get_element_by_id(doc, "renamed") == card);
        assert(whaleui_dom_get_element_by_id(doc, "card") == nullptr);
        assert(whaleui_dom_set_attribute(card, "id", "card") == 0);

        whaleui_dom_document_destroy(doc);
    }

    /* null-safety */
    {
        assert(whaleui_dom_parse_html(nullptr, 0) != nullptr); /* stub tolerates */
        whaleui_dom_document_destroy(nullptr);
        assert(whaleui_dom_document_element(nullptr) == nullptr);
        assert(whaleui_dom_create_element(nullptr, "div") == nullptr);
        assert(whaleui_dom_append_child(nullptr, nullptr) != 0);
        assert(whaleui_dom_set_attribute(nullptr, "a", "b") != 0);
        assert(whaleui_dom_get_text(nullptr) == nullptr);
        assert(whaleui_dom_tag_name(nullptr) == nullptr);
    }

    /* full DOM API surface: navigation, lists, classList, innerHTML,
     * values, titles, events */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<html><head><title>T0</title></head><body>"
            "<div id=\"a\" class=\"x y\"><p>one</p><p>two</p></div>"
            "<input id=\"in\" value=\"v1\">"
            "<select id=\"sel\"><option value=\"A\" selected>A</option>"
            "<option value=\"B\">B</option></select>"
            "<textarea id=\"ta\">ta-text</textarea>"
            "</body></html>",
            0);
        assert(doc != nullptr);

        /* body/head */
        assert(whaleui_dom_body(doc) != nullptr);
        assert(std::strcmp(whaleui_dom_tag_name(whaleui_dom_body(doc)), "body") == 0);
        assert(std::strcmp(whaleui_dom_tag_name(whaleui_dom_head(doc)), "head") == 0);
        assert(whaleui_dom_active_element(doc) == whaleui_dom_body(doc));

        /* title */
        assert(std::strcmp(whaleui_dom_get_title(doc), "T0") == 0);
        assert(whaleui_dom_set_title(doc, "T1") == 0);
        assert(std::strcmp(whaleui_dom_get_title(doc), "T1") == 0);

        /* document-level event target: registering on the document handle
         * (the browser's document.addEventListener) lands on the
         * documentElement; events dispatched inside the tree bubble to it */
        {
            static int doc_hits = 0;
            struct DL {
                static void on(whaleui_dom_event_t* ev, void* ud) {
                    (void)ud;
                    ++doc_hits;
                    assert(whaleui_dom_event_phase(ev) == 3); /* bubble */
                }
            };
            whaleui_dom_element_t* tdiv = whaleui_dom_get_element_by_id(doc, "a");
            assert(tdiv != nullptr);
            assert(whaleui_dom_add_event_listener(
                       reinterpret_cast<whaleui_dom_element_t*>(doc), "click",
                       DL::on, nullptr) == 0);
            /* the listener sits on documentElement; an inner click bubbles up */
            assert(whaleui_dom_dispatch_event(tdiv, "click") == 0);
            assert(doc_hits == 1);
            assert(whaleui_dom_remove_event_listener(
                       reinterpret_cast<whaleui_dom_element_t*>(doc), "click",
                       DL::on, nullptr) == 0);
            assert(whaleui_dom_dispatch_event(tdiv, "click") == 0);
            assert(doc_hits == 1);
        }

        /* focus()/blur() are real now: they move the activeElement mirror
         * and fire the non-bubbling focus/blur pair */
        {
            static std::string fev;
            struct F {
                static void on(whaleui_dom_event_t* ev, void* ud) {
                    (void)ud;
                    fev += whaleui_dom_event_type(ev);
                    fev += ';';
                }
            };
            whaleui_dom_element_t* inp = whaleui_dom_get_element_by_id(doc, "in");
            whaleui_dom_element_t* tdiv = whaleui_dom_get_element_by_id(doc, "a");
            assert(inp && tdiv);
            assert(whaleui_dom_add_event_listener(inp, "focus", F::on, nullptr) == 0);
            assert(whaleui_dom_add_event_listener(tdiv, "blur", F::on, nullptr) == 0);
            assert(whaleui_dom_focus(inp) == 0);
            assert(whaleui_dom_active_element(doc) == inp);
            /* refocusing the same element is a no-op (no second event) */
            assert(whaleui_dom_focus(inp) == 0);
            /* moving focus elsewhere blurs the previous element first
             * (a's blur listener only sees blur(a): inp has no blur listener) */
            assert(whaleui_dom_focus(tdiv) == 0);
            assert(whaleui_dom_active_element(doc) == tdiv);
            /* blur() only fires on the ACTIVE element */
            assert(whaleui_dom_blur(inp) == 0); /* not active: no blur event */
            assert(whaleui_dom_blur(tdiv) == 0);
            assert(whaleui_dom_active_element(doc) == whaleui_dom_body(doc));
            assert(fev == "focus;blur;");
            assert(whaleui_dom_remove_event_listener(inp, "focus", F::on, nullptr) == 0);
            assert(whaleui_dom_remove_event_listener(tdiv, "blur", F::on, nullptr) == 0);
        }

        /* element-scoped query */
        whaleui_dom_element_t* a = whaleui_dom_get_element_by_id(doc, "a");
        assert(a != nullptr);
        assert(whaleui_dom_element_query_selector(a, "p") != nullptr);
        assert(whaleui_dom_closest(whaleui_dom_query_selector(doc, ".y"), "div") == a);
        assert(whaleui_dom_matches(a, "div.x"));
        assert(!whaleui_dom_matches(a, "p"));
        /* full selector language: child/adjacent/attr/comma */
        assert(whaleui_dom_query_selector(doc, "div > p") != nullptr);
        assert(whaleui_dom_query_selector(doc, "p + p") != nullptr);
        assert(whaleui_dom_query_selector(doc, "div[id=\"a\"]") == a);
        assert(whaleui_dom_query_selector(doc, "span, div > p") != nullptr);
        assert(whaleui_dom_query_selector(doc, "div:first-child") == a);
        assert(whaleui_dom_matches(a, "div[id]"));
        /* compound fast-path: two classes must split at the '.' (#id +
         * .class too): ".x.y" is x AND y, not a token "x.y" */
        assert(whaleui_dom_matches(a, "div.x.y"));
        assert(!whaleui_dom_matches(a, "div.x.z"));
        {
            whaleui_dom_list_t* lst = whaleui_dom_query_selector_all(doc, ".x.y");
            assert(whaleui_dom_list_length(lst) == 1);
            whaleui_dom_list_destroy(lst);
        }
        assert(whaleui_dom_query_selector(doc, "") == nullptr);
        assert(whaleui_dom_query_selector_all(doc, "") == nullptr);
        /* element-scoped excludes the root itself */
        assert(whaleui_dom_element_query_selector(a, "div") == nullptr);
        assert(whaleui_dom_element_query_selector(a, "div > p") != nullptr);
        whaleui_dom_list_t* all_p = whaleui_dom_query_selector_all(doc, "div p, input");
        assert(all_p != nullptr && whaleui_dom_list_length(all_p) == 3);
        whaleui_dom_list_destroy(all_p);

        /* lists */
        whaleui_dom_list_t* ps = whaleui_dom_element_query_selector_all(a, "p");
        assert(ps != nullptr && whaleui_dom_list_length(ps) == 2);
        whaleui_dom_element_t* p1 = whaleui_dom_list_item(ps, 0);
        whaleui_dom_element_t* p2 = whaleui_dom_list_item(ps, 1);
        assert(p1 && p2);
        whaleui_dom_list_destroy(ps);
        assert(whaleui_dom_child_element_count(a) == 2);
        whaleui_dom_list_t* kids = whaleui_dom_children(a);
        assert(kids && whaleui_dom_list_length(kids) == 2);
        whaleui_dom_list_destroy(kids);
        assert(whaleui_dom_first_element_child(a) == p1);
        assert(whaleui_dom_last_element_child(a) == p2);
        assert(whaleui_dom_previous_element_sibling(p2) == p1);
        assert(whaleui_dom_next_element_sibling(p1) == p2);
        assert(whaleui_dom_parent_element(p1) == a);
        assert(whaleui_dom_contains(a, p2) == 1);
        assert(whaleui_dom_has_child_nodes(a) == 1);
        assert(whaleui_dom_is_connected(a) == 1);
        /* document-level collections */
        whaleui_dom_list_t* all = whaleui_dom_get_elements_by_tag_name(doc, "p");
        assert(all && whaleui_dom_list_length(all) == 2);
        whaleui_dom_list_destroy(all);
        whaleui_dom_list_t* xy = whaleui_dom_get_elements_by_class_name(doc, "x");
        assert(xy && whaleui_dom_list_length(xy) == 1);
        whaleui_dom_list_destroy(xy);

        /* classList */
        assert(whaleui_dom_class_contains(a, "x"));
        assert(whaleui_dom_class_add(a, "z") == 0);
        assert(whaleui_dom_class_contains(a, "z"));
        assert(whaleui_dom_class_toggle(a, "z") == 0); /* removed */
        assert(!whaleui_dom_class_contains(a, "z"));
        assert(whaleui_dom_class_remove(a, "x") == 0);
        assert(!whaleui_dom_class_contains(a, "x"));
        assert(whaleui_dom_class_contains(a, "y")); /* other tokens intact */

        /* classList extras: length/item/replace/toggle(force) */
        assert(whaleui_dom_class_add(a, "x") == 0); /* "x y" restored-ish */
        assert(whaleui_dom_class_length(a) >= 1);
        char tok[32];
        assert(whaleui_dom_class_item(a, 0, tok, sizeof(tok)) == 0);
        assert(tok[0] != '\0');
        assert(whaleui_dom_class_item(a, 999, tok, sizeof(tok)) != 0);
        assert(whaleui_dom_class_replace(a, "y", "z") == 1); /* was present */
        assert(whaleui_dom_class_contains(a, "z"));
        assert(!whaleui_dom_class_contains(a, "y"));
        assert(whaleui_dom_class_replace(a, "nope", "w") == 0); /* absent */
        assert(whaleui_dom_class_toggle_force(a, "q", 1) == 1); /* force on */
        assert(whaleui_dom_class_contains(a, "q"));
        assert(whaleui_dom_class_toggle_force(a, "q", 1) == 1); /* already on, no flip */
        assert(whaleui_dom_class_toggle_force(a, "q", 0) == 0); /* force off */
        assert(!whaleui_dom_class_contains(a, "q"));
        assert(whaleui_dom_class_toggle_force(a, "q", -1) == 1); /* flip on */

        /* dataset: camelCase <-> data-kebab, set/get/has/remove */
        assert(whaleui_dom_dataset_set(a, "fooBar", "42") == 0);
        assert(whaleui_dom_dataset_get(a, "fooBar") != nullptr);
        assert(std::strcmp(whaleui_dom_dataset_get(a, "fooBar"), "42") == 0);
        assert(std::strcmp(whaleui_dom_get_attribute(a, "data-foo-bar"), "42") == 0);
        assert(whaleui_dom_dataset_has(a, "fooBar"));
        assert(!whaleui_dom_dataset_has(a, "missing"));
        assert(whaleui_dom_dataset_remove(a, "fooBar") == 0);
        assert(!whaleui_dom_dataset_has(a, "fooBar"));
        assert(whaleui_dom_dataset_get(a, nullptr) == nullptr);

        /* insertAdjacentHTML: the four positions keep fragment order */
        {
            whaleui_dom_document_t* d2 = whaleui_dom_parse_html(
                "<html><body><div id=\"t\"><b>mid</b></div></body></html>", 0);
            assert(d2 != nullptr);
            whaleui_dom_element_t* t = whaleui_dom_get_element_by_id(d2, "t");
            /* beforeend appends after existing child */
            assert(whaleui_dom_insert_adjacent_html(t, "beforeend",
                       "<i>1</i><i>2</i>") == 0);
            assert(std::strstr(whaleui_dom_get_inner_html(t),
                               "<b>mid</b><i>1</i><i>2</i>") != nullptr);
            /* afterbegin prepends before existing first child, in order */
            assert(whaleui_dom_insert_adjacent_html(t, "afterbegin",
                       "<u>A</u><u>B</u>") == 0);
            assert(std::strstr(whaleui_dom_get_inner_html(t),
                               "<u>A</u><u>B</u><b>mid</b>") != nullptr);
            /* beforebegin inserts a sibling before t */
            whaleui_dom_element_t* body = whaleui_dom_body(d2);
            assert(whaleui_dom_insert_adjacent_html(t, "beforebegin",
                       "<span>pb</span>") == 0);
            assert(std::strstr(whaleui_dom_get_inner_html(body),
                               "<span>pb</span><div") != nullptr);
            /* afterend inserts a sibling after t */
            assert(whaleui_dom_insert_adjacent_html(t, "afterend",
                       "<span>ae</span>") == 0);
            assert(std::strstr(whaleui_dom_get_inner_html(body),
                               "</div><span>ae</span>") != nullptr);
            /* invalid position rejected */
            assert(whaleui_dom_insert_adjacent_html(t, "nowhere", "x") != 0);
            whaleui_dom_document_destroy(d2);
        }

        /* append/prepend/before/after (node forms) */
        {
            whaleui_dom_document_t* d3 = whaleui_dom_parse_html(
                "<html><body><div id=\"p\"><a>1</a><b>2</b></div></body></html>", 0);
            assert(d3 != nullptr);
            whaleui_dom_element_t* p = whaleui_dom_get_element_by_id(d3, "p");
            whaleui_dom_element_t* first = whaleui_dom_first_element_child(p);
            whaleui_dom_element_t* last = whaleui_dom_last_element_child(p);
            whaleui_dom_element_t* x = whaleui_dom_create_element(d3, "span");
            whaleui_dom_set_text(x, "x");
            assert(whaleui_dom_append(p, x) == 0);
            assert(whaleui_dom_last_element_child(p) == x);
            whaleui_dom_element_t* y = whaleui_dom_create_element(d3, "span");
            assert(whaleui_dom_prepend(p, y) == 0);
            assert(whaleui_dom_first_element_child(p) == y);
            /* before/after relative to an existing child */
            whaleui_dom_element_t* bx = whaleui_dom_create_element(d3, "i");
            assert(whaleui_dom_before(first, bx) == 0);
            assert(whaleui_dom_previous_element_sibling(first) == bx);
            whaleui_dom_element_t* ax = whaleui_dom_create_element(d3, "em");
            assert(whaleui_dom_after(first, ax) == 0);
            assert(whaleui_dom_next_element_sibling(first) == ax);
            whaleui_dom_document_destroy(d3);
        }

        /* DocumentFragment: append moves its children, not the fragment */
        {
            whaleui_dom_document_t* d4 = whaleui_dom_parse_html(
                "<html><body><div id=\"f\"></div></body></html>", 0);
            assert(d4 != nullptr);
            whaleui_dom_element_t* f = whaleui_dom_get_element_by_id(d4, "f");
            whaleui_dom_element_t* frag = whaleui_dom_create_document_fragment(d4);
            assert(frag != nullptr);
            whaleui_dom_element_t* c1 = whaleui_dom_create_element(d4, "b");
            whaleui_dom_element_t* c2 = whaleui_dom_create_element(d4, "i");
            assert(whaleui_dom_append_child(frag, c1) == 0);
            assert(whaleui_dom_append_child(frag, c2) == 0);
            /* appending the fragment moves b,i into f; the fragment empties */
            assert(whaleui_dom_append_child(f, frag) == 0);
            assert(whaleui_dom_child_element_count(f) == 2);
            assert(whaleui_dom_first_element_child(f) == c1);
            assert(whaleui_dom_last_element_child(f) == c2);
            assert(whaleui_dom_child_element_count(frag) == 0);
            /* insertAdjacentText: text nodes at each position */
            assert(whaleui_dom_insert_adjacent_text(c1, "beforeend", "hi") == 0);
            assert(std::strcmp(whaleui_dom_get_text(c1), "hi") == 0);
            assert(whaleui_dom_insert_adjacent_text(f, "afterbegin", "lead") == 0);
            assert(std::strcmp(whaleui_dom_get_text(f), "leadhi") == 0);
            assert(whaleui_dom_insert_adjacent_text(c1, "nowhere", "x") != 0);
            whaleui_dom_document_destroy(d4);
        }

        /* innerHTML round-trip */
        const char* ih = whaleui_dom_get_inner_html(a);
        assert(ih != nullptr && std::strstr(ih, "<p>one</p>") != nullptr);
        assert(whaleui_dom_set_inner_html(a, "<em>e</em><p>n</p>") == 0);
        assert(whaleui_dom_get_element_by_id(doc, "a") != nullptr);
        const char* ih2 = whaleui_dom_get_inner_html(a);
        assert(ih2 && std::strstr(ih2, "<em>e</em>") != nullptr);
        assert(std::strstr(ih2, "<p>n</p>") != nullptr);
        /* outerHTML serializes the element itself */
        const char* oh = whaleui_dom_get_outer_html(a);
        assert(oh && std::strstr(oh, "<div") != nullptr);

        /* form values */
        assert(std::strcmp(whaleui_dom_get_value(
                   whaleui_dom_get_element_by_id(doc, "in")), "v1") == 0);
        assert(whaleui_dom_set_value(
                   whaleui_dom_get_element_by_id(doc, "in"), "v2") == 0);
        assert(std::strcmp(whaleui_dom_get_value(
                   whaleui_dom_get_element_by_id(doc, "in")), "v2") == 0);
        assert(std::strcmp(whaleui_dom_get_value(
                   whaleui_dom_get_element_by_id(doc, "sel")), "A") == 0);
        assert(whaleui_dom_set_value(
                   whaleui_dom_get_element_by_id(doc, "sel"), "B") == 0);
        assert(std::strcmp(whaleui_dom_get_value(
                   whaleui_dom_get_element_by_id(doc, "sel")), "B") == 0);
        assert(std::strcmp(whaleui_dom_get_value(
                   whaleui_dom_get_element_by_id(doc, "ta")), "ta-text") == 0);
        assert(whaleui_dom_get_value(a) == nullptr); /* not a control */

        /* focus/blur are no-ops at the DOM layer */
        assert(whaleui_dom_focus(a) == 0);
        assert(whaleui_dom_blur(a) == 0);

        /* events: add / dispatch / remove */
        static int clicks = 0;
        static const char* got_type = nullptr;
        static void* got_ud = nullptr;
        static int ev_prevented = 0;
        struct Local {
            static void on_click(whaleui_dom_event_t* ev, void* ud) {
                ++clicks;
                got_type = whaleui_dom_event_type(ev);
                got_ud = ud;
                assert(whaleui_dom_event_target(ev) != nullptr);
                whaleui_dom_event_prevent_default(ev);
                ev_prevented = whaleui_dom_event_default_prevented(ev);
            }
        };
        void* ud = &clicks;
        assert(whaleui_dom_add_event_listener(a, "click", Local::on_click, ud) == 0);
        assert(whaleui_dom_add_event_listener(a, "click", Local::on_click, ud) == 0); /* dup no-op */
        assert(whaleui_dom_dispatch_event(a, "click") == 0);
        assert(clicks == 1);
        assert(got_type && std::strcmp(got_type, "click") == 0);
        assert(got_ud == ud);
        assert(ev_prevented == 1);
        /* dispatch on an element without listeners still succeeds */
        assert(whaleui_dom_dispatch_event(whaleui_dom_body(doc), "click") == 0);
        assert(whaleui_dom_remove_event_listener(a, "click", Local::on_click, ud) == 0);
        assert(whaleui_dom_dispatch_event(a, "click") == 0);
        assert(clicks == 1); /* removed */

        /* three-phase dispatch: capture (root -> parent), target, bubble
         * (parent -> root). A grandchild click must visit body-capture,
         * div-capture, target-capture, target-bubble, div-bubble,
         * body-bubble in that order, and stopPropagation halts the rest. */
        {
            static std::string order;
            static bool do_stop = false;
            /* ud encodes "tag+cap": e.g. "divC" = div's capture listener.
             * The record prints the registration owner + the phase number
             * (1 = capture, 2 = target, 3 = bubble). */
            struct Ph {
                static void rec(whaleui_dom_event_t* ev, void* ud) {
                    const char* tag = static_cast<const char*>(ud);
                    order += tag;
                    order += ('0' + whaleui_dom_event_phase(ev));
                    order += ";";
                    assert(whaleui_dom_event_current_target(ev) != nullptr);
                    if (do_stop && tag[0] == 'd' && tag[1] == 'i' &&
                        whaleui_dom_event_phase(ev) == 3) {
                        whaleui_dom_event_stop_propagation(ev);
                    }
                }
            };
            whaleui_dom_document_t* doc2 = whaleui_dom_parse_html(
                "<html><body><div id=\"d\">"
                "<span id=\"s\">x</span></div>"
                "</body></html>",
                0);
            assert(doc2 != nullptr);
            whaleui_dom_element_t* body2 = whaleui_dom_body(doc2);
            whaleui_dom_element_t* d = whaleui_dom_get_element_by_id(doc2, "d");
            whaleui_dom_element_t* s = whaleui_dom_get_element_by_id(doc2, "s");
            assert(body2 && d && s);
            assert(whaleui_dom_add_event_listener_ex(body2, "click", Ph::rec,
                                                     (void*)"bodyC", 1) == 0);
            assert(whaleui_dom_add_event_listener(body2, "click", Ph::rec,
                                                  (void*)"bodyB") == 0);
            assert(whaleui_dom_add_event_listener_ex(d, "click", Ph::rec,
                                                     (void*)"divC", 1) == 0);
            assert(whaleui_dom_add_event_listener(d, "click", Ph::rec,
                                                  (void*)"divB") == 0);
            assert(whaleui_dom_add_event_listener_ex(s, "click", Ph::rec,
                                                     (void*)"spanC", 1) == 0);
            assert(whaleui_dom_add_event_listener(s, "click", Ph::rec,
                                                  (void*)"spanB") == 0);
            order.clear();
            do_stop = false;
            assert(whaleui_dom_dispatch_event(s, "click") == 0);
            std::printf("[events] order=%s\n", order.c_str());
            std::fflush(stdout);
            assert(order == "bodyC1;divC1;spanC2;spanB2;divB3;bodyB3;");
            /* stopPropagation at div-bubble: body-bubble never runs */
            order.clear();
            do_stop = true;
            assert(whaleui_dom_dispatch_event(s, "click") == 0);
            assert(order == "bodyC1;divC1;spanC2;spanB2;divB3;");
            whaleui_dom_document_destroy(doc2);
        }

        /* non-bubbling dispatch (mouseenter/mouseleave/focus/blur):
         * capture listeners on ancestors still fire, bubble listeners do
         * not. Same chain as above, separate doc (listeners map is global). */
        {
            static std::string order;
            struct Nb {
                static void rec(whaleui_dom_event_t* ev, void* ud) {
                    const char* tag = static_cast<const char*>(ud);
                    order += tag;
                    order += ('0' + whaleui_dom_event_phase(ev));
                    order += ";";
                }
            };
            whaleui_dom_document_t* doc3 = whaleui_dom_parse_html(
                "<html><body><div id=\"d\"><span id=\"s\">x</span></div>"
                "</body></html>",
                0);
            assert(doc3 != nullptr);
            whaleui_dom_element_t* body3 = whaleui_dom_body(doc3);
            whaleui_dom_element_t* d3 = whaleui_dom_get_element_by_id(doc3, "d");
            whaleui_dom_element_t* s3 = whaleui_dom_get_element_by_id(doc3, "s");
            assert(body3 && d3 && s3);
            assert(whaleui_dom_add_event_listener(body3, "mouseenter", Nb::rec,
                                                   (void*)"bub") == 0);
            assert(whaleui_dom_add_event_listener_ex(d3, "mouseenter", Nb::rec,
                                                     (void*)"divC", 1) == 0);
            assert(whaleui_dom_add_event_listener(s3, "mouseenter", Nb::rec,
                                                  (void*)"tgt") == 0);
            order.clear();
            int prevented = 0;
            assert(whaleui_dom_dispatch_nonbubble(
                       s3, "mouseenter", 0, 3, 4, 0, 0.0f, 0.0f,
                       &prevented) == 0);
            assert(prevented == 0);
            /* capture on the ancestor + target only; the parent's bubble
             * listener never runs (non-bubbling contract) */
            assert(order == "divC1;tgt2;");
            /* bubbling variants of the same types still reach ancestors */
            order.clear();
            assert(whaleui_dom_add_event_listener(d3, "load", Nb::rec,
                                                  (void*)"divL") == 0);
            assert(whaleui_dom_add_event_listener(s3, "load", Nb::rec,
                                                  (void*)"tgtL") == 0);
            assert(whaleui_dom_dispatch_event(s3, "load") == 0);
            assert(order == "tgtL2;divL3;");
            whaleui_dom_document_destroy(doc3);
        }

        /* newly wired engine event types dispatch through the same
         * pipeline (app.cpp routes real input to these names) */
        {
            static int fired = 0;
            struct T {
                static void on(whaleui_dom_event_t* ev, void* ud) {
                    (void)ev; (void)ud; ++fired;
                }
            };
            for (const char* t : {"dblclick", "contextmenu", "mousemove",
                                  "wheel", "input", "change", "scroll",
                                  "resize"}) {
                assert(whaleui_dom_add_event_listener(a, t, T::on, nullptr) == 0);
            }
            for (const char* t : {"dblclick", "contextmenu", "mousemove",
                                  "wheel", "input", "change", "scroll",
                                  "resize"}) {
                assert(whaleui_dom_dispatch_event(a, t) == 0);
            }
            assert(fired == 8);
        }

        /* clone: deep copy is independent */
        whaleui_dom_element_t* clone = whaleui_dom_clone(a, 1);
        assert(clone != nullptr);
        assert(whaleui_dom_get_inner_html(clone) != nullptr);
        whaleui_dom_element_destroy(clone);

        /* remove / insert_before with fresh nodes (the earlier p1/p2 were
         * destroyed by set_inner_html) */
        whaleui_dom_element_t* body = whaleui_dom_body(doc);
        whaleui_dom_element_t* nx = whaleui_dom_create_element(doc, "div");
        assert(nx != nullptr);
        assert(whaleui_dom_insert_before(
                   body, nx, whaleui_dom_first_element_child(body)) == 0);
        assert(whaleui_dom_parent(nx) == body);
        assert(whaleui_dom_is_connected(nx) == 1);
        assert(whaleui_dom_remove(nx) == 0);
        assert(whaleui_dom_parent(nx) == nullptr);
        assert(whaleui_dom_is_connected(nx) == 0);
        whaleui_dom_element_destroy(nx);
        assert(body != nullptr);

        whaleui_dom_document_destroy(doc);
    }

    test_dom_utf16_and_bom_input();

    return 0;
}
