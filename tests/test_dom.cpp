// test_dom: DOM API contract tests.
#include "whaleui.h"

#include <cassert>
#include <cstring>

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
        assert(whaleui_dom_query_selector(doc, "div #card") == card);
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

        /* element-scoped query */
        whaleui_dom_element_t* a = whaleui_dom_get_element_by_id(doc, "a");
        assert(a != nullptr);
        assert(whaleui_dom_element_query_selector(a, "p") != nullptr);
        assert(whaleui_dom_closest(whaleui_dom_query_selector(doc, ".y"), "div") == a);
        assert(whaleui_dom_matches(a, "div.x"));
        assert(!whaleui_dom_matches(a, "p"));

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

    return 0;
}
