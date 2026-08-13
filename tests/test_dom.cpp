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

    return 0;
}
