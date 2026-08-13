/* DOM: public C API implementation (step 3 - lexbor backed).
 *
 * Handles are the lexbor objects themselves (see dom.h): the document handle
 * is an lxb_html_document*, an element handle is an lxb_dom_element*. All
 * parsing/query/mutation is delegated to lexbor. */

#include "dom/dom.h"

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace {

lxb_dom_element* as_el(whaleui_dom_element_t* el)
{
    return reinterpret_cast<lxb_dom_element*>(el);
}

lxb_html_document* as_doc(whaleui_dom_document_t* doc)
{
    return reinterpret_cast<lxb_html_document*>(doc);
}

lxb_dom_element* root_element(whaleui_dom_document_t* doc)
{
    lxb_html_document* d = as_doc(doc);
    return d ? lxb_dom_document_element(&d->dom_document) : nullptr;
}

lxb_dom_document* dom_doc(whaleui_dom_document_t* doc)
{
    lxb_html_document* d = as_doc(doc);
    return d ? &d->dom_document : nullptr;
}

whaleui_dom_element_t* out(lxb_dom_element* el)
{
    return reinterpret_cast<whaleui_dom_element_t*>(el);
}

/* iterate child/sibling chain, skipping non-element nodes */
lxb_dom_element* next_element(lxb_dom_node* n)
{
    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return lxb_dom_interface_element(n);
        }
        n = n->next;
    }
    return nullptr;
}

/* match a simple selector: [tag][#id][.class...] */
bool simple_match(lxb_dom_element* el, const char* sel, size_t len)
{
    const char* p = sel;
    const char* end = sel + len;
    /* optional tag */
    if (p < end && *p != '#' && *p != '.') {
        const char* t = p;
        while (p < end && *p != '#' && *p != '.') {
            ++p;
        }
        size_t tlen = static_cast<size_t>(p - t);
        size_t nlen = 0;
        const lxb_char_t* name = lxb_dom_element_local_name(el, &nlen);
        if (!name || nlen != tlen || std::memcmp(name, t, tlen) != 0) {
            return false;
        }
    }
    while (p < end) {
        if (*p == '#') {
            ++p;
            const char* s = p;
            while (p < end && *p != '.') {
                ++p;
            }
            size_t vlen = static_cast<size_t>(p - s);
            size_t alen = 0;
            const lxb_char_t* id = lxb_dom_element_get_attribute(el, (const lxb_char_t*)"id", 2, &alen);
            if (!id || alen != vlen || std::memcmp(id, s, vlen) != 0) {
                return false;
            }
        } else if (*p == '.') {
            ++p;
            const char* s = p;
            while (p < end && *p != '#') {
                ++p;
            }
            size_t vlen = static_cast<size_t>(p - s);
            size_t alen = 0;
            const lxb_char_t* cls = lxb_dom_element_get_attribute(el, (const lxb_char_t*)"class", 5, &alen);
            if (!cls) {
                return false;
            }
            /* class list: any token equal to s..p */
            bool hit = false;
            const lxb_char_t* c = cls;
            const lxb_char_t* ce = cls + alen;
            while (c < ce) {
                while (c < ce && (*c == ' ' || *c == '\t')) {
                    ++c;
                }
                const lxb_char_t* tok = c;
                while (c < ce && *c != ' ' && *c != '\t') {
                    ++c;
                }
                if (static_cast<size_t>(c - tok) == vlen && std::memcmp(tok, s, vlen) == 0) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                return false;
            }
        }
    }
    return true;
}

/* depth-first search for the first element matching `sel` under root.
 * Returns 0 when found (out = element), 1 when not found. */
int find_sel(lxb_dom_element* root, const char* sel, size_t len, lxb_dom_element** out)
{
    if (simple_match(root, sel, len)) {
        *out = root;
        return 0;
    }
    lxb_dom_node* child = root->node.first_child;
    while (child) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (find_sel(lxb_dom_interface_element(child), sel, len, out) == 0) {
                return 0;
            }
        }
        child = child->next;
    }
    return 1;
}

/* full text content of an element (concatenated text-node data), malloc'd */
char* text_content(lxb_dom_element* e, size_t* outlen)
{
    size_t cap = 64, len = 0;
    char* buf = static_cast<char*>(std::malloc(cap));
    if (!buf) {
        return nullptr;
    }
    lxb_dom_node* n = e->node.first_child;
    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
            const lexbor_str_t* s = &lxb_dom_interface_text(n)->char_data.data;
            size_t dlen = s->length;
            while (len + dlen + 1 > cap) {
                cap *= 2;
                char* nb = static_cast<char*>(std::realloc(buf, cap));
                if (!nb) {
                    std::free(buf);
                    return nullptr;
                }
                buf = nb;
            }
            if (s->data) {
                std::memcpy(buf + len, s->data, dlen);
            }
            len += dlen;
        }
        n = n->next;
    }
    buf[len] = '\0';
    if (outlen) {
        *outlen = len;
    }
    return buf;
}

/* --- inline style (style attribute) helpers: flat "prop: value;" list --- */

const char* style_find(const char* style, const char* prop, size_t plen)
{
    if (!style) {
        return nullptr;
    }
    const char* p = style;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') {
            ++p;
        }
        if (std::strncmp(p, prop, plen) == 0 && p[plen] == ':') {
            return p + plen + 1;
        }
        while (*p && *p != ';') {
            ++p;
        }
    }
    return nullptr;
}

int style_set(lxb_dom_element* e, const char* prop, const char* value)
{
    if (!e) {
        return -1;
    }
    size_t alen = 0;
    const lxb_char_t* cur = lxb_dom_element_get_attribute(e, (const lxb_char_t*)"style", 5, &alen);
    std::string out;
    if (cur && alen) {
        out.assign(reinterpret_cast<const char*>(cur), alen);
    }
    size_t plen = std::strlen(prop);
    bool replaced = false;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t s = pos;
        while (s < out.size() && (out[s] == ' ' || out[s] == '\t' || out[s] == ';')) {
            ++s;
        }
        if (out.compare(s, plen, prop) == 0 && s + plen <= out.size() && out[s + plen] == ':') {
            size_t v = s + plen + 1;
            size_t e2 = out.find(';', v);
            if (e2 == std::string::npos) {
                e2 = out.size();
            }
            out.replace(s, e2 - s, std::string(prop) + ": " + value);
            replaced = true;
            break;
        }
        size_t semi = out.find(';', s);
        if (semi == std::string::npos) {
            break;
        }
        pos = semi + 1;
    }
    if (!replaced) {
        if (!out.empty() && out[out.size() - 1] != ';') {
            out += "; ";
        }
        out += std::string(prop) + ": " + value;
    }
    return lxb_dom_element_set_attribute(e, (const lxb_char_t*)"style", 5,
                                         reinterpret_cast<const lxb_char_t*>(out.c_str()),
                                         out.size()) ? 0 : -2;
}

} // namespace

extern "C" whaleui_dom_document_t* whaleui_dom_parse_html(const char* html, size_t len)
{
    lxb_html_document* doc = lxb_html_document_create();
    if (!doc) {
        return nullptr;
    }
    if (html) {
        lxb_html_document_parse(doc, reinterpret_cast<const lxb_char_t*>(html), len);
    }
    return reinterpret_cast<whaleui_dom_document_t*>(doc);
}

extern "C" void whaleui_dom_document_destroy(whaleui_dom_document_t* doc)
{
    if (doc) {
        lxb_html_document_destroy(as_doc(doc));
    }
}

extern "C" whaleui_dom_element_t* whaleui_dom_document_element(whaleui_dom_document_t* doc)
{
    return out(root_element(doc));
}

extern "C" whaleui_dom_element_t* whaleui_dom_get_element_by_id(whaleui_dom_document_t* doc,
                                                                const char* id)
{
    if (!doc || !id) {
        return nullptr;
    }
    lxb_dom_element* root = root_element(doc);
    if (!root) {
        return nullptr;
    }
    return out(lxb_dom_element_by_id(root, reinterpret_cast<const lxb_char_t*>(id),
                                     std::strlen(id)));
}

extern "C" whaleui_dom_element_t* whaleui_dom_query_selector(whaleui_dom_document_t* doc,
                                                             const char* selector)
{
    if (!doc || !selector) {
        return nullptr;
    }
    lxb_dom_element* root = root_element(doc);
    if (!root) {
        return nullptr;
    }
    /* split on whitespace: descendant chain, first match wins */
    const char* s = selector;
    lxb_dom_element* scope = root;
    while (*s) {
        while (*s == ' ' || *s == '\t') {
            ++s;
        }
        if (!*s) {
            break;
        }
        const char* part = s;
        while (*s && *s != ' ' && *s != '\t') {
            ++s;
        }
        size_t plen = static_cast<size_t>(s - part);
        lxb_dom_element* found = nullptr;
        if (find_sel(scope, part, plen, &found) != 0) {
            return nullptr;
        }
        scope = found;
    }
    return out(scope);
}

extern "C" whaleui_dom_element_t* whaleui_dom_parent(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !e->node.parent) {
        return nullptr;
    }
    return out(lxb_dom_interface_element(e->node.parent));
}

extern "C" whaleui_dom_element_t* whaleui_dom_first_child(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    return e ? out(next_element(e->node.first_child)) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_next_sibling(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    return e ? out(next_element(e->node.next)) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_create_element(whaleui_dom_document_t* doc,
                                                             const char* tag)
{
    lxb_dom_document* d = dom_doc(doc);
    if (!d || !tag) {
        return nullptr;
    }
    /* lexbor keeps local names verbatim; normalize to lowercase */
    char lower[64];
    size_t i = 0;
    for (; tag[i] && i < sizeof(lower) - 1; ++i) {
        char c = tag[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    lower[i] = '\0';
    lxb_dom_element* el = lxb_dom_document_create_element(d,
        reinterpret_cast<const lxb_char_t*>(lower), i, nullptr);
    return out(el);
}

extern "C" int whaleui_dom_append_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child)
{
    lxb_dom_element* p = as_el(parent);
    lxb_dom_element* c = as_el(child);
    if (!p || !c) {
        return -1;
    }
    if (c->node.parent) {
        lxb_dom_node_remove(&c->node);
    }
    lxb_dom_node_insert_child(&p->node, &c->node);
    return 0;
}

extern "C" int whaleui_dom_remove_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child)
{
    (void)parent;
    lxb_dom_element* c = as_el(child);
    if (!c) {
        return -1;
    }
    if (!c->node.parent) {
        return -2;
    }
    lxb_dom_node_remove(&c->node);
    return 0;
}

extern "C" int whaleui_dom_element_destroy(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return -1;
    }
    if (!e->node.parent) {
        /* detached: owned by nobody, free the node */
        lxb_dom_node_destroy(&e->node);
    }
    return 0;
}

extern "C" int whaleui_dom_set_attribute(whaleui_dom_element_t* el, const char* name, const char* value)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !name || !value) {
        return -1;
    }
    return lxb_dom_element_set_attribute(e, reinterpret_cast<const lxb_char_t*>(name),
                                         std::strlen(name),
                                         reinterpret_cast<const lxb_char_t*>(value),
                                         std::strlen(value)) ? 0 : -2;
}

extern "C" const char* whaleui_dom_get_attribute(whaleui_dom_element_t* el, const char* name)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !name) {
        return nullptr;
    }
    size_t len = 0;
    return reinterpret_cast<const char*>(
        lxb_dom_element_get_attribute(e, reinterpret_cast<const lxb_char_t*>(name),
                                      std::strlen(name), &len));
}

extern "C" int whaleui_dom_set_text(whaleui_dom_element_t* el, const char* text)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !text) {
        return -1;
    }
    /* clear children then append one text node */
    lxb_dom_node* n = e->node.first_child;
    while (n) {
        lxb_dom_node* nx = n->next;
        lxb_dom_node_remove(n);
        lxb_dom_node_destroy(n);
        n = nx;
    }
    lxb_dom_text* tn = lxb_dom_document_create_text_node(
        e->node.owner_document,
        reinterpret_cast<const lxb_char_t*>(text), std::strlen(text));
    if (!tn) {
        return -2;
    }
    lxb_dom_node_insert_child(&e->node, lxb_dom_interface_node(tn));
    return 0;
}

extern "C" const char* whaleui_dom_get_text(whaleui_dom_element_t* el)
{
    static char empty[1] = {0};
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return nullptr;
    }
    if (!e->node.first_child) {
        return empty;
    }
    /* per-document cache so the pointer stays valid until the next call */
    static std::string cache;
    cache.clear();
    /* recursive text collection (textContent semantics) */
    std::function<void(lxb_dom_node*)> walk = [&walk](lxb_dom_node* n) {
        while (n) {
            if (n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
                const lexbor_str_t* s = &lxb_dom_interface_text(n)->char_data.data;
                if (s->data) {
                    cache.append(reinterpret_cast<const char*>(s->data), s->length);
                }
            } else if (n->first_child) {
                walk(n->first_child);
            }
            n = n->next;
        }
    };
    walk(e->node.first_child);
    return cache.c_str();
}

extern "C" int whaleui_dom_set_style(whaleui_dom_element_t* el, const char* property, const char* value)
{
    if (!el || !property || !value) {
        return -1;
    }
    return style_set(as_el(el), property, value);
}

extern "C" const char* whaleui_dom_get_style(whaleui_dom_element_t* el, const char* property)
{
    if (!el || !property) {
        return nullptr;
    }
    size_t alen = 0;
    const lxb_char_t* style = lxb_dom_element_get_attribute(as_el(el), (const lxb_char_t*)"style", 5, &alen);
    if (!style) {
        return nullptr;
    }
    std::string s(reinterpret_cast<const char*>(style), alen);
    const char* v = style_find(s.c_str(), property, std::strlen(property));
    if (!v) {
        return nullptr;
    }
    while (*v == ' ' || *v == '\t') {
        ++v;
    }
    /* trim trailing whitespace / ';' */
    static char cache[256];
    size_t i = 0;
    while (v[i] && v[i] != ';' && v[i] != '\r' && v[i] != '\n' && i < sizeof(cache) - 1) {
        cache[i] = v[i];
        ++i;
    }
    while (i > 0 && (cache[i - 1] == ' ' || cache[i - 1] == '\t')) {
        --i;
    }
    cache[i] = '\0';
    return cache;
}

extern "C" const char* whaleui_dom_tag_name(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return nullptr;
    }
    size_t len = 0;
    return reinterpret_cast<const char*>(lxb_dom_element_local_name(e, &len));
}
