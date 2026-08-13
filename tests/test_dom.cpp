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

        /* attach to root */
        whaleui_dom_element_t* root = whaleui_dom_document_element(doc);
        assert(whaleui_dom_append_child(root, box) == 0);
        assert(whaleui_dom_parent(box) == root);
        assert(whaleui_dom_first_child(root) == box);

        /* id query */
        assert(whaleui_dom_set_attribute(box, "id", "box") == 0);
        assert(whaleui_dom_get_element_by_id(doc, "box") == box);
        assert(whaleui_dom_query_selector(doc, "#box") == box);

        /* remove */
        assert(whaleui_dom_remove_child(root, box) == 0);
        assert(whaleui_dom_parent(box) == nullptr);
        assert(whaleui_dom_first_child(root) == nullptr);
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
