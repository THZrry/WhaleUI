/* DOM: public C API implementation.
 * Step 2: contract implementation - minimal hand-rolled tree so the API is
 * exercised. Step 3 replaces internals with lexbor (parse + full tree ops). */

#include "dom/dom.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace {

char* dup_str(const char* s)
{
    size_t n = std::strlen(s) + 1;
    char* d = static_cast<char*>(std::malloc(n));
    if (d) {
        std::memcpy(d, s, n);
    }
    return d;
}

/* find "name=value" in a flat array, return the value or NULL */
const char* kv_get(char* const* arr, size_t count, const char* name)
{
    size_t n = std::strlen(name);
    for (size_t i = 0; i < count; ++i) {
        if (std::strncmp(arr[i], name, n) == 0 && arr[i][n] == '=') {
            return arr[i] + n + 1;
        }
    }
    return nullptr;
}

int kv_set(char*** arr, size_t* count, const char* name, const char* value)
{
    size_t n = std::strlen(name);
    for (size_t i = 0; i < *count; ++i) {
        if (std::strncmp((*arr)[i], name, n) == 0 && (*arr)[i][n] == '=') {
            char* rep = static_cast<char*>(std::malloc(n + std::strlen(value) + 2));
            if (!rep) {
                return -1;
            }
            std::sprintf(rep, "%s=%s", name, value);
            std::free((*arr)[i]);
            (*arr)[i] = rep;
            return 0;
        }
    }
    char* kv = static_cast<char*>(std::malloc(n + std::strlen(value) + 2));
    if (!kv) {
        return -1;
    }
    std::sprintf(kv, "%s=%s", name, value);
    char** na = static_cast<char**>(std::realloc(*arr, (*count + 1) * sizeof(char*)));
    if (!na) {
        std::free(kv);
        return -1;
    }
    *arr = na;
    (*arr)[(*count)++] = kv;
    return 0;
}

void free_kv(char** arr, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        std::free(arr[i]);
    }
    std::free(arr);
}

void element_free(whaleui_dom_element_t* el)
{
    if (!el) {
        return;
    }
    std::free(el->tag);
    std::free(el->id);
    std::free(el->text);
    free_kv(el->attrs, el->attr_count);
    free_kv(el->styles, el->style_count);
    delete el;
}

} // namespace

extern "C" whaleui_dom_document_t* whaleui_dom_parse_html(const char* html, size_t len)
{
    (void)html;
    (void)len;
    /* Step 2: no parsing yet - return an empty document with an html root. */
    whaleui_dom_document_t* doc = new whaleui_dom_document_t;
    whaleui_dom_element_t* root = new whaleui_dom_element_t;
    std::memset(root, 0, sizeof(*root));
    root->tag = dup_str("html");
    doc->root = root;
    return doc;
}

extern "C" void whaleui_dom_document_destroy(whaleui_dom_document_t* doc)
{
    if (!doc) {
        return;
    }
    /* free children recursively (simple) */
    whaleui_dom_element_t* el = doc->root;
    while (el) {
        whaleui_dom_element_t* next = el->next_sibling;
        element_free(el);
        el = next;
    }
    delete doc;
}

extern "C" whaleui_dom_element_t* whaleui_dom_document_element(whaleui_dom_document_t* doc)
{
    return doc ? doc->root : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_get_element_by_id(whaleui_dom_document_t* doc,
                                                                const char* id)
{
    if (!doc || !id) {
        return nullptr;
    }
    /* Step 2: scan root and its direct children (real traversal in step 3). */
    whaleui_dom_element_t* el = doc->root;
    while (el) {
        if (el->id && std::strcmp(el->id, id) == 0) {
            return el;
        }
        el = el->first_child;
        while (el) {
            if (el->id && std::strcmp(el->id, id) == 0) {
                return el;
            }
            el = el->next_sibling;
        }
    }
    return nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_query_selector(whaleui_dom_document_t* doc,
                                                             const char* selector)
{
    /* Step 2: accept "#id" form against root only. */
    if (!doc || !selector) {
        return nullptr;
    }
    if (selector[0] == '#') {
        return whaleui_dom_get_element_by_id(doc, selector + 1);
    }
    return nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_parent(whaleui_dom_element_t* el)
{
    return el ? el->parent : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_first_child(whaleui_dom_element_t* el)
{
    return el ? el->first_child : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_next_sibling(whaleui_dom_element_t* el)
{
    return el ? el->next_sibling : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_create_element(whaleui_dom_document_t* doc,
                                                             const char* tag)
{
    if (!doc || !tag) {
        return nullptr;
    }
    whaleui_dom_element_t* el = new whaleui_dom_element_t;
    std::memset(el, 0, sizeof(*el));
    el->tag = dup_str(tag);
    return el;
}

extern "C" int whaleui_dom_append_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child)
{
    if (!parent || !child) {
        return -1;
    }
    child->parent = parent;
    child->next_sibling = nullptr;
    if (!parent->first_child) {
        parent->first_child = child;
    } else {
        whaleui_dom_element_t* last = parent->first_child;
        while (last->next_sibling) {
            last = last->next_sibling;
        }
        last->next_sibling = child;
    }
    return 0;
}

extern "C" int whaleui_dom_remove_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child)
{
    if (!parent || !child) {
        return -1;
    }
    whaleui_dom_element_t** link = &parent->first_child;
    while (*link) {
        if (*link == child) {
            *link = child->next_sibling;
            child->parent = nullptr;
            child->next_sibling = nullptr;
            return 0;
        }
        link = &(*link)->next_sibling;
    }
    return -2;
}

extern "C" int whaleui_dom_element_destroy(whaleui_dom_element_t* el)
{
    if (!el) {
        return -1;
    }
    element_free(el);
    return 0;
}

extern "C" int whaleui_dom_set_attribute(whaleui_dom_element_t* el, const char* name, const char* value)
{
    if (!el || !name || !value) {
        return -1;
    }
    if (std::strcmp(name, "id") == 0) {
        std::free(el->id);
        el->id = dup_str(value);
        return 0;
    }
    return kv_set(&el->attrs, &el->attr_count, name, value);
}

extern "C" const char* whaleui_dom_get_attribute(whaleui_dom_element_t* el, const char* name)
{
    if (!el || !name) {
        return nullptr;
    }
    if (std::strcmp(name, "id") == 0) {
        return el->id;
    }
    return kv_get(el->attrs, el->attr_count, name);
}

extern "C" int whaleui_dom_set_text(whaleui_dom_element_t* el, const char* text)
{
    if (!el || !text) {
        return -1;
    }
    std::free(el->text);
    el->text = dup_str(text);
    return 0;
}

extern "C" const char* whaleui_dom_get_text(whaleui_dom_element_t* el)
{
    return el ? el->text : nullptr;
}

extern "C" int whaleui_dom_set_style(whaleui_dom_element_t* el, const char* property, const char* value)
{
    if (!el || !property || !value) {
        return -1;
    }
    return kv_set(&el->styles, &el->style_count, property, value);
}

extern "C" const char* whaleui_dom_get_style(whaleui_dom_element_t* el, const char* property)
{
    if (!el || !property) {
        return nullptr;
    }
    return kv_get(el->styles, el->style_count, property);
}

extern "C" const char* whaleui_dom_tag_name(whaleui_dom_element_t* el)
{
    return el ? el->tag : nullptr;
}
