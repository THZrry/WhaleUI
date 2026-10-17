/* DOM: public C API implementation (step 3 - lexbor backed).
 *
 * Handles are the lexbor objects themselves (see dom.h): the document handle
 * is an lxb_html_document*, an element handle is an lxb_dom_element*. All
 * parsing/query/mutation is delegated to lexbor. */

#include "dom/dom.h"
#include "core/utf.h"
#include "dom/events.h"
#include "dom/observers.h"
#include "style/style.h"
#include "canvas/canvas.h"

/* whaleui_render_last_tree (get_bounding_client_rect) */
#include "render/render.h"

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <lexbor/html/parser.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

/* pending DOM mutations, per document. The renderer drains the set for its
 * own document each frame (whaleui_dom_take_dirty) and incrementally
 * relayouts the affected subtrees. */
struct DirtyEntry
{
    lxb_dom_element* el;
    lxb_dom_document* doc;
    /* structural (children/text added, removed, moved, replaced) vs style-only
     * (hover/focus/class/attribute): only a structural edit can change a run's
     * TEXT, so only those should invalidate the text raster caches. See the
     * renderer's per-element version map. */
    bool structural;
};
std::vector<DirtyEntry>& g_dirty()
{
    static std::vector<DirtyEntry> d;
    return d;
}

/* style-attribute edits can change --var definitions; the vars collection
 * (whole-document walk) is then stale and must re-run once. Set only by the
 * style-mutating APIs below, consumed by the renderer per dom_dirty frame. */
bool& g_vars_dirty()
{
    static bool d = false;
    return d;
}

/* a structural DOM edit happened this frame (children/text added, removed,
 * moved or replaced): the renderer must full-relayout (not the style-only
 * fast path) and repaint widely. See whaleui_dom_mark_dirty_struct. */
bool& g_struct_dirty()
{
    static bool d = false;
    return d;
}

/* a stylesheet SOURCE node was added, removed or edited: the rule set the
 * render context holds is stale and must be re-collected. The engine collects
 * CSS from the document only at load / resize / theme change (see
 * window_reload_css), so without this flag a <style> injected through the DOM
 * API - or one whose text/attribute was rewritten - never took effect at all.
 * That is the "CSS mounted from C++ does nothing" report: the markup path and
 * the API path disagreed because only the markup path ran the collector.
 * Per document (like the dirty set above) so one window's style edit does not
 * re-parse every other window's sheet. Consumed by the render worker
 * (whaleui_dom_take_css_dirty). */
std::set<lxb_dom_document*>& g_css_dirty()
{
    static std::set<lxb_dom_document*> s;
    return s;
}

/* document.activeElement mirror: per-document focused element (written by
 * whaleui_dom_set_active - the public focus/blur API and the render click/
 * Tab focus bookkeeping). Cleared with the document on destroy. */
std::map<lxb_dom_document*, lxb_dom_element*>& g_active()
{
    static std::map<lxb_dom_document*, lxb_dom_element*> m;
    return m;
}

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
            /* the value ends at the next token start (both . and #), not
             * just the next '.': "#a.b" is id a + class b */
            while (p < end && *p != '.' && *p != '#') {
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
            /* compound classes ("div.a.b") must split at the next '.' too -
             * consuming everything up to '#' made ".a.b" a single class
             * token "a.b" that never matches */
            while (p < end && *p != '#' && *p != '.') {
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

/* query matching via the style engine (same selector language as the
 * cascade: tag/#id/.class/attr/combinators/pseudo-classes). The style
 * matcher needs NUL-terminated selectors; simple_match stays as the fast
 * path for single-compound selectors. */
bool query_match(lxb_dom_element* el, const char* sel, size_t len)
{
    if (!el || !sel) return false;
    /* fast path: single compound without combinators/attrs/pseudos */
    bool complex = false;
    for (size_t i = 0; i < len; ++i) {
        char c = sel[i];
        if (c == ' ' || c == '\t' || c == '>' || c == '+' || c == '~' ||
            c == '[' || c == ':' || c == ',') { complex = true; break; }
    }
    if (!complex) return simple_match(el, sel, len);
    std::string s(sel, len);
    /* comma lists: match if any branch matches */
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i < s.size() ? s[i] : ',';
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        if (c == ',' && depth == 0) {
            std::string branch = s.substr(start, i - start);
            size_t a = 0;
            while (a < branch.size() && (branch[a] == ' ' || branch[a] == '\t')) ++a;
            size_t b = branch.size();
            while (b > a && (branch[b-1] == ' ' || branch[b-1] == '\t')) --b;
            if (a < b) {
                std::string t = branch.substr(a, b - a);
                whaleui_style_state st;
                if (whaleui_style_match(t.c_str(), el, &st)) return true;
            }
            start = i + 1;
        }
    }
    return false;
}

/* depth-first search for the first element matching `sel` under root.
 * Returns 0 when found (out = element), 1 when not found. */
int find_sel(lxb_dom_element* root, const char* sel, size_t len, lxb_dom_element** out)
{
    if (query_match(root, sel, len)) {
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

/* depth-first search excluding the root itself (element.querySelector(All)
 * semantics: descendants only). */
int find_sel_desc(lxb_dom_element* root, const char* sel, size_t len, lxb_dom_element** out)
{
    lxb_dom_node* child = root->node.first_child;
    while (child) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element* ce = lxb_dom_interface_element(child);
            if (query_match(ce, sel, len)) { *out = ce; return 0; }
            if (find_sel_desc(ce, sel, len, out) == 0) return 0;
        }
        child = child->next;
    }
    return 1;
}

void collect_sel_desc(lxb_dom_element* root, const char* sel, size_t len,
                      std::vector<lxb_dom_element*>& out)
{
    for (lxb_dom_node* c = root->node.first_child; c; c = c->next) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element* ce = lxb_dom_interface_element(c);
            if (query_match(ce, sel, len)) out.push_back(ce);
            collect_sel_desc(ce, sel, len, out);
        }
    }
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

/* parse an HTML fragment and hand back its detached top-level nodes (the
 * siblings the fragment contributes), without inserting them. Mirrors
 * parse_into's parsing but leaves placement to the caller (insertAdjacentHTML). */
std::vector<lxb_dom_node*> parse_detach(lxb_dom_element* el, const char* html,
                                        size_t len)
{
    std::vector<lxb_dom_node*> out;
    lxb_dom_document* doc = el->node.owner_document;
    /* an lxb_dom_document is the first member of lxb_html_document */
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    if (!hd || !html) {
        return out;
    }
    lxb_html_parser_t* parser = lxb_html_parser_create();
    if (!parser) {
        return out;
    }
    lxb_html_parser_init(parser);
    lxb_dom_node_t* frag = lxb_html_parse_fragment_by_tag_id(
        parser, hd, LXB_TAG_DIV, LXB_NS_HTML,
        reinterpret_cast<const lxb_char_t*>(html), len);
    if (!frag) {
        lxb_html_parser_destroy(parser);
        return out;
    }
    while (frag->first_child) {
        lxb_dom_node* c = frag->first_child;
        lxb_dom_node_remove(c);
        out.push_back(c);
    }
    lxb_html_parser_destroy(parser);
    return out;
}

} // namespace

/* Resolve the four insertAdjacent positions to a fixed (parent, anchor,
 * append-mode) triple, validating the position string and that a sibling
 * position has a parent. Returns 0 on success; -2 invalid position,
 * -3 no parent for beforebegin/afterend. The anchor is captured BEFORE any
 * insertion so a multi-node fragment keeps its order (insert each node before
 * the same anchor). */
struct AdjacentTarget
{
    bool beforebegin;
    bool afterbegin;
    lxb_dom_node* parent_node;
    lxb_dom_node* anchor;
    bool append_mode;
};
static int resolve_adjacent(lxb_dom_element* e, const char* position,
                            AdjacentTarget& t)
{
    t.beforebegin = std::strcmp(position, "beforebegin") == 0;
    t.afterbegin = std::strcmp(position, "afterbegin") == 0;
    bool beforeend = std::strcmp(position, "beforeend") == 0;
    bool afterend = std::strcmp(position, "afterend") == 0;
    if (!t.beforebegin && !t.afterbegin && !beforeend && !afterend) {
        return -2;
    }
    if ((t.beforebegin || afterend) && !e->node.parent) {
        return -3;
    }
    t.parent_node = nullptr;
    t.anchor = nullptr;
    t.append_mode = false;
    if (t.beforebegin) {
        t.parent_node = e->node.parent; t.anchor = &e->node;
    } else if (t.afterbegin) {
        t.parent_node = &e->node; t.anchor = e->node.first_child; t.append_mode = !t.anchor;
    } else if (beforeend) {
        t.parent_node = &e->node; t.append_mode = true;
    } else { /* afterend */
        t.parent_node = e->node.parent; t.anchor = e->node.next; t.append_mode = !t.anchor;
    }
    return 0;
}

extern "C" int whaleui_dom_insert_adjacent_html(whaleui_dom_element_t* elt,
                                                const char* position,
                                                const char* html)
{
    lxb_dom_element* e = reinterpret_cast<lxb_dom_element*>(elt);
    if (!e || !position || !html) {
        return -1;
    }
    AdjacentTarget t;
    int rc = resolve_adjacent(e, position, t);
    if (rc != 0) {
        return rc;
    }
    std::vector<lxb_dom_node*> nodes = parse_detach(e, html, std::strlen(html));
    if (nodes.empty()) {
        return 0; /* empty/failed fragment: nothing to insert */
    }
    lxb_dom_element* owner = e->node.parent
        ? lxb_dom_interface_element(e->node.parent) : nullptr;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (t.append_mode) {
            lxb_dom_node_insert_child(t.parent_node, nodes[i]);
        } else {
            lxb_dom_node_insert(t.parent_node, nodes[i], t.anchor, false);
        }
    }
    whaleui_dom_mark_dirty_struct(e);
    if (t.beforebegin || (owner && t.parent_node == e->node.parent && t.anchor != &e->node)) {
        /* afterend touched the parent; owner dirty */
        whaleui_dom_mark_dirty_struct(owner);
    }
    whaleui_dom_mark_vars_dirty();
    return 0;
}

extern "C" int whaleui_dom_insert_adjacent_text(whaleui_dom_element_t* elt,
                                                const char* position,
                                                const char* text)
{
    lxb_dom_element* e = reinterpret_cast<lxb_dom_element*>(elt);
    if (!e || !position || !text) {
        return -1;
    }
    AdjacentTarget t;
    int rc = resolve_adjacent(e, position, t);
    if (rc != 0) {
        return rc;
    }
    lxb_dom_text* tn = lxb_dom_document_create_text_node(
        e->node.owner_document, reinterpret_cast<const lxb_char_t*>(text),
        std::strlen(text));
    if (!tn) {
        return -4;
    }
    lxb_dom_node* n = lxb_dom_interface_node(tn);
    if (t.append_mode) {
        lxb_dom_node_insert_child(t.parent_node, n);
    } else {
        lxb_dom_node_insert(t.parent_node, n, t.anchor, false);
    }
    whaleui_dom_mark_dirty_struct(e);
    return 0;
}

extern "C" whaleui_dom_element_t* whaleui_dom_create_document_fragment(whaleui_dom_document_t* doc)
{
    lxb_dom_document* d = dom_doc(doc);
    if (!d) {
        return nullptr;
    }
    lxb_dom_document_fragment_t* frag = lxb_dom_document_create_document_fragment(d);
    return frag ? reinterpret_cast<whaleui_dom_element_t*>(&frag->node) : nullptr;
}

/* --- DOM mutation tracking (incremental relayout) --- */

/* 1 when `el` IS a stylesheet source node (<style> or <link>): its text,
 * attributes or inline style are read by the collector, so any edit to it
 * changes the document's CSS. */
static bool is_css_source_node(const lxb_dom_element* el)
{
    if (!el) {
        return false;
    }
    size_t len = 0;
    const lxb_char_t* nm = lxb_dom_element_local_name(
        const_cast<lxb_dom_element*>(el), &len);
    if (!nm || !len) {
        return false;
    }
    return (len == 5 && std::memcmp(nm, "style", 5) == 0) ||
           (len == 4 && std::memcmp(nm, "link", 4) == 0);
}

/* 1 when `el` is a section whose DIRECT CHILDREN the collector reads. Only a
 * STRUCTURAL edit here changes the sheet (a child added, removed or moved);
 * a text/attribute edit on <body> itself does not, which is what keeps an
 * ordinary edit inside the page from re-collecting the stylesheet. */
static bool is_css_section(const lxb_dom_element* el)
{
    if (!el) {
        return false;
    }
    size_t len = 0;
    const lxb_char_t* nm = lxb_dom_element_local_name(
        const_cast<lxb_dom_element*>(el), &len);
    if (!nm || len != 4) {
        return false;
    }
    return std::memcmp(nm, "head", 4) == 0 || std::memcmp(nm, "body", 4) == 0;
}

/* shared body: records (element, document, structural) with the upgrade rule.
 *
 * `attr` names the attribute an attribute edit touched, so the MutationObserver
 * record carries it. It is a parameter rather than a separate notify call
 * because two calls produced TWO records for one edit (a named one plus the
 * generic one below) - an observer saw every setAttribute twice. */
static void mark_dirty_ex(lxb_dom_element* el, bool structural,
                          const char* attr = nullptr)
{
    if (!el) {
        return;
    }
    lxb_dom_document* doc = el->node.owner_document;
    /* A stylesheet source node edited directly (text / attribute / style), or a
     * STRUCTURAL change to a <head>/<body> - the sections whose child list the
     * collector reads. Every insertion/removal/move API marks the PARENT it
     * touched (append_child, remove_child, insert_before, set_inner_html,
     * insert_adjacent*), so the section clause alone covers them all; a text or
     * attribute edit on an ordinary child of <body> marks that child, which is
     * correctly NOT a stylesheet change (flagging it would re-collect the whole
     * sheet on every keystroke). */
    if (is_css_source_node(el) ||
        (structural && is_css_section(el))) {
        g_css_dirty().insert(doc);
    }
    /* MutationObserver: a structural edit is a childList mutation, anything
     * else an attribute/characterData edit (with the attribute name when the
     * caller knows it - see the attribute APIs). */
    whaleui_dom_notify_mutation(el, structural ? 2 : 0, structural ? nullptr : attr,
                                structural ? 1 : 0, 0);
    for (auto& d : g_dirty()) {
        if (d.el == el && d.doc == doc) {
            /* already pending: a later structural edit must still upgrade the
             * entry, or the text caches would keep serving the old raster */
            d.structural = d.structural || structural;
            return;
        }
    }
    g_dirty().push_back({el, doc, structural});
}

/* mark_dirty for an edit that set/removed a known attribute: the observer
 * record carries the name. */
static void mark_dirty_attr(lxb_dom_element* el, const char* name)
{
    mark_dirty_ex(el, false, name);
}

/* Same drain as whaleui_dom_take_dirty, but keeps the "structural" flag per
 * element (parallel to `out`). Only a structural edit can change a run's TEXT,
 * and DOM edits are not rare (animations, editors, dynamic lists), so the
 * renderer bumps just those elements' text-cache versions instead of
 * invalidating every text cache on any mutation. */
extern "C" void whaleui_dom_take_dirty_typed(
    whaleui_dom_document_t* doc, std::vector<lxb_dom_element*>& out,
    std::vector<int>& out_structural)
{
    out.clear();
    out_structural.clear();
    lxb_dom_document* want =
        as_doc(doc) ? &as_doc(doc)->dom_document : nullptr;
    std::vector<DirtyEntry>& d = g_dirty();
    std::vector<DirtyEntry> keep;
    keep.reserve(d.size());
    for (auto& e : d) {
        if (e.doc == want) {
            out.push_back(e.el);
            out_structural.push_back(e.structural ? 1 : 0);
        } else {
            keep.push_back(e);
        }
    }
    d.swap(keep);
}

extern "C" void whaleui_dom_mark_dirty(lxb_dom_element* el)
{
    mark_dirty_ex(el, false);
}

extern "C" void whaleui_dom_take_dirty(whaleui_dom_document_t* doc,
                                       std::vector<lxb_dom_element*>& out)
{
    std::vector<int> structural;
    whaleui_dom_take_dirty_typed(doc, out, structural);
}

extern "C" void whaleui_dom_mark_vars_dirty(void)
{
    g_vars_dirty() = true;
}

extern "C" int whaleui_dom_take_vars_dirty(void)
{
    bool v = g_vars_dirty();
    g_vars_dirty() = false;
    return v ? 1 : 0;
}

extern "C" void whaleui_dom_mark_dirty_struct(lxb_dom_element* el)
{
    g_struct_dirty() = true;
    mark_dirty_ex(el, true);
}

extern "C" int whaleui_dom_take_struct_dirty(void)
{
    bool v = g_struct_dirty();
    g_struct_dirty() = false;
    return v ? 1 : 0;
}

extern "C" int whaleui_dom_take_css_dirty(whaleui_dom_document_t* doc)
{
    lxb_dom_document* want = as_doc(doc) ? &as_doc(doc)->dom_document : nullptr;
    return g_css_dirty().erase(want) > 0 ? 1 : 0;
}

extern "C" int whaleui_dom_has_css_dirty(whaleui_dom_document_t* doc)
{
    lxb_dom_document* want = as_doc(doc) ? &as_doc(doc)->dom_document : nullptr;
    return g_css_dirty().find(want) != g_css_dirty().end() ? 1 : 0;
}

extern "C" void whaleui_dom_mark_css_dirty(whaleui_dom_document_t* doc)
{
    lxb_dom_document* want = as_doc(doc) ? &as_doc(doc)->dom_document : nullptr;
    if (want) {
        g_css_dirty().insert(want);
    }
}

extern "C" whaleui_dom_document_t* whaleui_dom_parse_html(const char* html, size_t len)
{
    lxb_html_document* doc = lxb_html_document_create();
    if (!doc) {
        return nullptr;
    }
    if (html) {
        /* contract: len == 0 means NUL-terminated */
        if (len == 0) {
            len = std::strlen(html);
        }
        /* Encoding at the edge: a UTF-8 BOM reaching the parser becomes a
         * leading TEXT node before <html> (the page then renders that text
         * AND its stylesheets stop applying), and a UTF-16 file decoded as
         * UTF-8 collapses to its first ASCII character. See core/utf.h. */
        std::string scratch;
        size_t view_len = 0;
        const char* view = wui_utf8_view(html, len, scratch, &view_len);
        lxb_html_document_parse(doc, reinterpret_cast<const lxb_char_t*>(view),
                                view_len);
    }
    return reinterpret_cast<whaleui_dom_document_t*>(doc);
}

extern "C" void whaleui_dom_document_destroy(whaleui_dom_document_t* doc)
{
    if (doc) {
        /* element listeners die with their document (all elements become
         * unreachable; dropping the whole map avoids stale pointers) */
        whaleui_events_clear_all();
        /* canvas contexts too: keyed on element pointers that dangle with
         * the document (a later allocation reusing an address would hit a
         * stale context with a freed backing) */
        whaleui_canvas_drop_all();
        /* the active-element mirror too: its key (the lxb_dom_document) is
         * about to be freed, and a later allocation could reuse the address */
        g_active().erase(dom_doc(doc));
        /* same address-reuse hazard for the stylesheet-invalidation set and
         * the document's programmatic sheet (whaleui_css_apply) */
        g_css_dirty().erase(&as_doc(doc)->dom_document);
        whaleui_style_doc_rules_clear(doc);
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
    if (!doc || !selector || !*selector) {
        return nullptr;
    }
    lxb_dom_element* root = root_element(doc);
    if (!root) {
        return nullptr;
    }
    lxb_dom_element* found = nullptr;
    if (find_sel(root, selector, std::strlen(selector), &found) != 0) {
        return nullptr;
    }
    return out(found);
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
    if (!p || !child) {
        return -1;
    }
    /* child may be an element, text node or document fragment (all share
     * lxb_dom_node as first member); a fragment moves its children in. */
    lxb_dom_node* pc = &p->node;
    lxb_dom_node* cc = reinterpret_cast<lxb_dom_node*>(child);
    if (cc->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
        std::vector<lxb_dom_node*> kids;
        for (lxb_dom_node* c = cc->first_child; c;) {
            lxb_dom_node* nx = c->next;
            lxb_dom_node_remove(c);
            kids.push_back(c);
            c = nx;
        }
        for (size_t i = 0; i < kids.size(); ++i) {
            lxb_dom_node_insert_child(pc, kids[i]);
        }
    } else {
        if (cc->parent) {
            /* moving: the old parent loses this subtree too */
            whaleui_dom_mark_dirty_struct(
                lxb_dom_interface_element(cc->parent));
            lxb_dom_node_remove(cc);
        }
        lxb_dom_node_insert_child(pc, cc);
    }
    whaleui_dom_mark_dirty_struct(p);
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
    lxb_dom_element* old_parent =
        lxb_dom_interface_element(c->node.parent);
    lxb_dom_node_remove(&c->node);
    /* the old parent's subtree loses this branch on its next rebuild */
    whaleui_dom_mark_dirty_struct(old_parent);
    return 0;
}

extern "C" int whaleui_dom_element_destroy(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return -1;
    }
    whaleui_events_clear_element(e);
    if (!e->node.parent) {
        /* detached: owned by nobody, free the node */
        lxb_dom_node_destroy(&e->node);
    } else {
        /* attached: the parent's subtree changes when this node goes away */
        whaleui_dom_mark_dirty_struct(lxb_dom_interface_element(e->node.parent));
    }
    return 0;
}

extern "C" int whaleui_dom_set_attribute(whaleui_dom_element_t* el, const char* name, const char* value)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !name || !value) {
        return -1;
    }
    int rc = lxb_dom_element_set_attribute(
                 e, reinterpret_cast<const lxb_char_t*>(name),
                 std::strlen(name),
                 reinterpret_cast<const lxb_char_t*>(value),
                 std::strlen(value))
                 ? 0
                 : -2;
    if (rc == 0) {
        mark_dirty_attr(e, name);
        if (std::strcmp(name, "style") == 0) {
            whaleui_dom_mark_vars_dirty();
        }
    }
    return rc;
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

extern "C" int whaleui_dom_has_attribute(whaleui_dom_element_t* el, const char* name)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !name) {
        return 0;
    }
    /* presence, not value: boolean attributes (<details open>,
     * <input checked>) carry no value yet must match */
    return lxb_dom_element_has_attribute(e, reinterpret_cast<const lxb_char_t*>(name),
                                         std::strlen(name)) ? 1 : 0;
}

extern "C" int whaleui_dom_remove_attribute(whaleui_dom_element_t* el, const char* name)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !name) {
        return -1;
    }
    /* lexbor returns a status: 0 (LXB_STATUS_OK) means removed */
    int rc = lxb_dom_element_remove_attribute(
                 e, reinterpret_cast<const lxb_char_t*>(name), std::strlen(name)) == 0
                 ? 0
                 : -2;
    if (rc == 0) {
        mark_dirty_attr(e, name);
        if (std::strcmp(name, "style") == 0) {
            whaleui_dom_mark_vars_dirty();
        }
    }
    return rc;
}

extern "C" int whaleui_dom_toggle_attribute(whaleui_dom_element_t* el, const char* name,
                                            int force)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !name) {
        return -1;
    }
    bool has = lxb_dom_element_has_attribute(
        e, reinterpret_cast<const lxb_char_t*>(name), std::strlen(name)) != 0;
    bool want = force >= 0 ? (force != 0) : !has;
    if (want == has) {
        return has ? 1 : 0;
    }
    if (want) {
        lxb_dom_element_set_attribute(e, reinterpret_cast<const lxb_char_t*>(name),
                                      std::strlen(name),
                                      (const lxb_char_t*)"", 0);
    } else {
        lxb_dom_element_remove_attribute(e, reinterpret_cast<const lxb_char_t*>(name),
                                         std::strlen(name));
    }
    mark_dirty_attr(e, name);
    if (std::strcmp(name, "style") == 0) {
        whaleui_dom_mark_vars_dirty();
    }
    return want ? 1 : 0;
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
    whaleui_dom_mark_dirty_struct(e);
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
    int rc = style_set(as_el(el), property, value);
    if (rc == 0) {
        /* an inline-style edit IS an attribute edit on "style": report the
         * name so a MutationObserver record carries it */
        mark_dirty_attr(as_el(el), "style");
        /* a --var definition or any style change can affect var()
         * resolution elsewhere; re-collect vars next frame */
        whaleui_dom_mark_vars_dirty();
    }
    return rc;
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

/* parse the inline style attribute into ordered (name, value) pairs */
static void style_parse_pairs(const char* style, size_t len,
                              std::vector<std::pair<std::string,std::string>>& out)
{
    out.clear();
    if (!style) return;
    std::string s(style, len);
    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == ';' || s[pos] == '\r' || s[pos] == '\n')) ++pos;
        if (pos >= s.size()) break;
        size_t colon = s.find(':', pos);
        if (colon == std::string::npos) break;
        std::string name = s.substr(pos, colon - pos);
        size_t ns = 0;
        while (ns < name.size() && (name[ns] == ' ' || name[ns] == '\t')) ++ns;
        size_t ne = name.size();
        while (ne > ns && (name[ne-1] == ' ' || name[ne-1] == '\t')) --ne;
        name = name.substr(ns, ne - ns);
        size_t semi = s.find(';', colon + 1);
        std::string val = s.substr(colon + 1, semi == std::string::npos ? std::string::npos : semi - colon - 1);
        size_t vs = 0;
        while (vs < val.size() && (val[vs] == ' ' || val[vs] == '\t' || val[vs] == '\r' || val[vs] == '\n')) ++vs;
        size_t ve = val.size();
        while (ve > vs && (val[ve-1] == ' ' || val[ve-1] == '\t' || val[ve-1] == '\r' || val[ve-1] == '\n')) --ve;
        val = val.substr(vs, ve - vs);
        if (!name.empty()) out.push_back(std::make_pair(name, val));
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
}

static int style_remove(lxb_dom_element* e, const char* prop)
{
    if (!e) return -1;
    size_t alen = 0;
    const lxb_char_t* cur = lxb_dom_element_get_attribute(e, (const lxb_char_t*)"style", 5, &alen);
    if (!cur) return -2;
    std::vector<std::pair<std::string,std::string>> pairs;
    style_parse_pairs(reinterpret_cast<const char*>(cur), alen, pairs);
    std::string out;
    bool found = false;
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (pairs[i].first == prop) { found = true; continue; }
        if (!out.empty()) out += "; ";
        out += pairs[i].first + ": " + pairs[i].second;
    }
    if (!found) return -2;
    return lxb_dom_element_set_attribute(e, (const lxb_char_t*)"style", 5,
        reinterpret_cast<const lxb_char_t*>(out.c_str()), out.size()) ? 0 : -3;
}

extern "C" int whaleui_dom_style_set_property(whaleui_dom_element_t* el, const char* property, const char* value)
{
    return whaleui_dom_set_style(el, property, value);
}

extern "C" int whaleui_dom_style_get_property(whaleui_dom_element_t* el, const char* property, char* out, size_t cap)
{
    if (!el || !property || !out || cap == 0) return -1;
    const char* v = whaleui_dom_get_style(el, property);
    if (!v) return -2;
    size_t n = std::strlen(v);
    if (n + 1 > cap) return -3;
    std::memcpy(out, v, n + 1);
    return 0;
}

extern "C" int whaleui_dom_style_remove_property(whaleui_dom_element_t* el, const char* property)
{
    if (!el || !property) return -1;
    int rc = style_remove(as_el(el), property);
    if (rc == 0) {
        mark_dirty_attr(as_el(el), "style");
        whaleui_dom_mark_vars_dirty();
    }
    return rc;
}

extern "C" int whaleui_dom_style_get_css_text(whaleui_dom_element_t* el, char* out, size_t cap)
{
    if (!el || !out || cap == 0) return -1;
    size_t alen = 0;
    const lxb_char_t* s = lxb_dom_element_get_attribute(as_el(el), (const lxb_char_t*)"style", 5, &alen);
    std::string t;
    if (s && alen) t.assign(reinterpret_cast<const char*>(s), alen);
    if (t.size() + 1 > cap) return -2;
    std::memcpy(out, t.c_str(), t.size() + 1);
    return 0;
}

extern "C" int whaleui_dom_style_set_css_text(whaleui_dom_element_t* el, const char* css_text)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !css_text) return -1;
    int rc = lxb_dom_element_set_attribute(e, (const lxb_char_t*)"style", 5,
        reinterpret_cast<const lxb_char_t*>(css_text), std::strlen(css_text)) ? 0 : -2;
    if (rc == 0) {
        mark_dirty_attr(e, "style");
        whaleui_dom_mark_vars_dirty();
    }
    return rc;
}

extern "C" size_t whaleui_dom_style_length(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!e) return 0;
    size_t alen = 0;
    const lxb_char_t* s = lxb_dom_element_get_attribute(e, (const lxb_char_t*)"style", 5, &alen);
    std::vector<std::pair<std::string,std::string>> pairs;
    style_parse_pairs(s ? reinterpret_cast<const char*>(s) : nullptr, alen, pairs);
    return pairs.size();
}

extern "C" int whaleui_dom_style_item(whaleui_dom_element_t* el, size_t index, char* out, size_t cap)
{
    if (!el || !out || cap == 0) return -1;
    lxb_dom_element* e = as_el(el);
    size_t alen = 0;
    const lxb_char_t* s = lxb_dom_element_get_attribute(e, (const lxb_char_t*)"style", 5, &alen);
    std::vector<std::pair<std::string,std::string>> pairs;
    style_parse_pairs(s ? reinterpret_cast<const char*>(s) : nullptr, alen, pairs);
    if (index >= pairs.size()) return -2;
    const std::string& n = pairs[index].first;
    if (n.size() + 1 > cap) return -3;
    std::memcpy(out, n.c_str(), n.size() + 1);
    return 0;
}

extern "C" whaleui_style_declaration_t* whaleui_dom_get_computed_style(whaleui_dom_element_t* el, const char* pseudo)
{
    (void)pseudo;
    lxb_dom_element* e = as_el(el);
    if (!e) return nullptr;
    /* computed style = most recent layout node's style when available,
     * else inline style pairs. Layout nodes own the cascade result. */
    whaleui_layout_tree_t* tree = whaleui_render_last_tree();
    const WhaleUIComputedStyle* src = nullptr;
    if (tree) {
        for (size_t i = 0; i < tree->arena.size(); ++i) {
            if (tree->arena[i].el == e && !tree->arena[i].is_text) { src = &tree->arena[i].style; break; }
        }
    }
    whaleui_style_declaration_t* d = new whaleui_style_declaration_t;
    if (src) {
        for (std::map<std::string,std::string>::const_iterator it = src->begin(); it != src->end(); ++it) {
            d->names.push_back(it->first);
            d->values.push_back(it->second);
        }
    } else {
        size_t alen = 0;
        const lxb_char_t* s = lxb_dom_element_get_attribute(e, (const lxb_char_t*)"style", 5, &alen);
        std::vector<std::pair<std::string,std::string>> pairs;
        style_parse_pairs(s ? reinterpret_cast<const char*>(s) : nullptr, alen, pairs);
        for (size_t i = 0; i < pairs.size(); ++i) {
            d->names.push_back(pairs[i].first);
            d->values.push_back(pairs[i].second);
        }
    }
    return d;
}

extern "C" void whaleui_style_declaration_destroy(whaleui_style_declaration_t* decl)
{
    delete decl;
}

extern "C" size_t whaleui_style_declaration_length(const whaleui_style_declaration_t* decl)
{
    return decl ? decl->names.size() : 0;
}

extern "C" int whaleui_style_declaration_item(const whaleui_style_declaration_t* decl, size_t index, char* out, size_t cap)
{
    if (!decl || !out || cap == 0) return -1;
    if (index >= decl->names.size()) return -2;
    const std::string& n = decl->names[index];
    if (n.size() + 1 > cap) return -3;
    std::memcpy(out, n.c_str(), n.size() + 1);
    return 0;
}

extern "C" int whaleui_style_declaration_get(const whaleui_style_declaration_t* decl, const char* property, char* out, size_t cap)
{
    if (!decl || !property || !out || cap == 0) return -1;
    for (size_t i = 0; i < decl->names.size(); ++i) {
        if (decl->names[i] == property) {
            const std::string& v = decl->values[i];
            if (v.size() + 1 > cap) return -3;
            std::memcpy(out, v.c_str(), v.size() + 1);
            return 0;
        }
    }
    return -2;
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

/* --- element collections (snapshot lists) --- */

struct whaleui_dom_list
{
    std::vector<lxb_dom_element*> items;
};

extern "C" size_t whaleui_dom_list_length(const whaleui_dom_list_t* list)
{
    return list ? list->items.size() : 0;
}

extern "C" whaleui_dom_element_t* whaleui_dom_list_item(const whaleui_dom_list_t* list,
                                                        size_t index)
{
    return (list && index < list->items.size()) ? out(list->items[index])
                                                : nullptr;
}

extern "C" void whaleui_dom_list_destroy(whaleui_dom_list_t* list)
{
    delete list;
}

whaleui_dom_list_t* make_list(std::vector<lxb_dom_element*>& v)
{
    whaleui_dom_list_t* list = new whaleui_dom_list_t;
    list->items.swap(v);
    return list;
}

/* depth-first collectors (element order = document order) */
void collect_all(lxb_dom_element* root, std::vector<lxb_dom_element*>& out)
{
    out.push_back(root);
    for (lxb_dom_node* c = root->node.first_child; c; c = c->next) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            collect_all(lxb_dom_interface_element(c), out);
        }
    }
}

void collect_sel(lxb_dom_element* root, const char* sel, size_t len,
                 std::vector<lxb_dom_element*>& out)
{
    if (query_match(root, sel, len)) {
        out.push_back(root);
    }
    for (lxb_dom_node* c = root->node.first_child; c; c = c->next) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            collect_sel(lxb_dom_interface_element(c), sel, len, out);
        }
    }
}

void collect_tag(lxb_dom_element* root, const char* tag, size_t len,
                 std::vector<lxb_dom_element*>& out)
{
    size_t nlen = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(root, &nlen);
    if (name && nlen == len && std::memcmp(name, tag, len) == 0) {
        out.push_back(root);
    }
    for (lxb_dom_node* c = root->node.first_child; c; c = c->next) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            collect_tag(lxb_dom_interface_element(c), tag, len, out);
        }
    }
}

void collect_class(lxb_dom_element* root, const char* cls, size_t len,
                   std::vector<lxb_dom_element*>& out)
{
    size_t alen = 0;
    const lxb_char_t* a = lxb_dom_element_get_attribute(
        root, (const lxb_char_t*)"class", 5, &alen);
    bool hit = false;
    if (a) {
        const lxb_char_t* c = a;
        const lxb_char_t* e = a + alen;
        while (c < e && !hit) {
            while (c < e && (*c == ' ' || *c == '\t')) {
                ++c;
            }
            const lxb_char_t* tok = c;
            while (c < e && *c != ' ' && *c != '\t') {
                ++c;
            }
            if (static_cast<size_t>(c - tok) == len &&
                std::memcmp(tok, cls, len) == 0) {
                hit = true;
            }
        }
    }
    if (hit) {
        out.push_back(root);
    }
    for (lxb_dom_node* c = root->node.first_child; c; c = c->next) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            collect_class(lxb_dom_interface_element(c), cls, len, out);
        }
    }
}

/* last element child (reverse walk) */
lxb_dom_element* last_element_child(lxb_dom_element* el)
{
    for (lxb_dom_node* c = el->node.last_child; c; c = c->prev) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return lxb_dom_interface_element(c);
        }
    }
    return nullptr;
}

lxb_dom_element* prev_element(lxb_dom_element* el)
{
    for (lxb_dom_node* s = el->node.prev; s; s = s->prev) {
        if (s->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return lxb_dom_interface_element(s);
        }
    }
    return nullptr;
}

/* --- document-level queries --- */

extern "C" whaleui_dom_element_t* whaleui_dom_body(whaleui_dom_document_t* doc)
{
    lxb_html_document* d = as_doc(doc);
    return d && d->body ? out(lxb_dom_interface_element(&d->body->element.element.node))
                        : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_head(whaleui_dom_document_t* doc)
{
    lxb_html_document* d = as_doc(doc);
    return d && d->head ? out(lxb_dom_interface_element(&d->head->element.element.node))
                        : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_active_element(whaleui_dom_document_t* doc)
{
    /* the DOM-layer active element (public focus()/blur() + the engine's
     * click/Tab focus both record it via whaleui_dom_set_active); the body
     * is the default when nothing was focused, like the browser */
    lxb_dom_document* d = dom_doc(doc);
    if (d) {
        std::map<lxb_dom_document*, lxb_dom_element*>::iterator it =
            g_active().find(d);
        if (it != g_active().end() && it->second) {
            return out(it->second);
        }
    }
    return whaleui_dom_body(doc);
}

/* The single writer of the active-element map: the public focus/blur API
 * and the render click/Tab focus bookkeeping both call this. */
extern "C" void whaleui_dom_set_active(struct lxb_dom_element* el)
{
    if (!el || !el->node.owner_document) {
        return;
    }
    g_active()[el->node.owner_document] = el;
}

/* Drop the mirror for a document whose engine focus cleared to nothing
 * (click outside the tree). Safe on an absent key. */
extern "C" void whaleui_dom_clear_active(struct lxb_dom_document* doc)
{
    if (doc) {
        g_active().erase(doc);
    }
}

extern "C" int whaleui_dom_set_title(whaleui_dom_document_t* doc, const char* title)
{
    if (!doc || !title) {
        return -1;
    }
    lxb_html_document* d = as_doc(doc);
    lxb_dom_element* title_el = nullptr;
    if (d->head) {
        for (lxb_dom_node* c = d->head->element.element.node.first_child; c;
             c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element* e = lxb_dom_interface_element(c);
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(e, &len);
                if (name && len == 5 && std::memcmp(name, "title", 5) == 0) {
                    title_el = e;
                    break;
                }
            }
        }
    }
    if (!title_el) {
        title_el = lxb_dom_document_create_element(
            dom_doc(doc), (const lxb_char_t*)"title", 5, nullptr);
        if (!title_el) {
            return -2;
        }
        lxb_dom_node_insert_child(&d->head->element.element.node, &title_el->node);
    }
    lxb_dom_node* n = title_el->node.first_child;
    while (n) {
        lxb_dom_node* nx = n->next;
        lxb_dom_node_remove(n);
        lxb_dom_node_destroy(n);
        n = nx;
    }
    lxb_dom_text* tn = lxb_dom_document_create_text_node(
        dom_doc(doc), reinterpret_cast<const lxb_char_t*>(title),
        std::strlen(title));
    if (!tn) {
        return -2;
    }
    lxb_dom_node_insert_child(&title_el->node, lxb_dom_interface_node(tn));
    return 0;
}

extern "C" const char* whaleui_dom_get_title(whaleui_dom_document_t* doc)
{
    static std::string cache;
    cache.clear();
    lxb_html_document* d = as_doc(doc);
    if (!d || !d->head) {
        return nullptr;
    }
    for (lxb_dom_node* c = d->head->element.element.node.first_child; c; c = c->next) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element* e = lxb_dom_interface_element(c);
            size_t len = 0;
            const lxb_char_t* name = lxb_dom_element_local_name(e, &len);
            if (name && len == 5 && std::memcmp(name, "title", 5) == 0) {
                for (lxb_dom_node* t = e->node.first_child; t; t = t->next) {
                    if (t->type == LXB_DOM_NODE_TYPE_TEXT) {
                        const lexbor_str_t* s =
                            &lxb_dom_interface_text(t)->char_data.data;
                        if (s->data) {
                            cache.append(
                                reinterpret_cast<const char*>(s->data),
                                s->length);
                        }
                    }
                }
                return cache.c_str();
            }
        }
    }
    return nullptr;
}

extern "C" whaleui_dom_list_t* whaleui_dom_query_selector_all(whaleui_dom_document_t* doc,
                                                               const char* selector)
{
    if (!doc || !selector || !*selector) {
        return nullptr;
    }
    lxb_dom_element* root = root_element(doc);
    std::vector<lxb_dom_element*> v;
    if (!root) {
        return make_list(v);
    }
    collect_sel(root, selector, std::strlen(selector), v);
    return make_list(v);
}

extern "C" whaleui_dom_list_t* whaleui_dom_get_elements_by_class_name(whaleui_dom_document_t* doc,
                                                                       const char* cls)
{
    if (!doc || !cls) {
        return nullptr;
    }
    lxb_dom_element* root = root_element(doc);
    std::vector<lxb_dom_element*> v;
    if (root) {
        collect_class(root, cls, std::strlen(cls), v);
    }
    return make_list(v);
}

extern "C" whaleui_dom_list_t* whaleui_dom_get_elements_by_tag_name(whaleui_dom_document_t* doc,
                                                                    const char* tag)
{
    if (!doc || !tag) {
        return nullptr;
    }
    lxb_dom_element* root = root_element(doc);
    std::vector<lxb_dom_element*> v;
    if (root) {
        collect_tag(root, tag, std::strlen(tag), v);
    }
    return make_list(v);
}

/* --- element-scoped queries --- */

extern "C" whaleui_dom_element_t* whaleui_dom_element_query_selector(whaleui_dom_element_t* el,
                                                                     const char* selector)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !selector || !*selector) {
        return nullptr;
    }
    lxb_dom_element* found = nullptr;
    /* browser semantics: descendants only, root itself excluded */
    if (find_sel_desc(e, selector, std::strlen(selector), &found) != 0) {
        return nullptr;
    }
    return out(found);
}

extern "C" whaleui_dom_list_t* whaleui_dom_element_query_selector_all(whaleui_dom_element_t* el,
                                                                      const char* selector)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !selector || !*selector) {
        return nullptr;
    }
    std::vector<lxb_dom_element*> v;
    collect_sel_desc(e, selector, std::strlen(selector), v);
    return make_list(v);
}

extern "C" whaleui_dom_list_t* whaleui_dom_element_get_elements_by_class_name(whaleui_dom_element_t* el,
                                                                              const char* cls)
{
    lxb_dom_element* e = as_el(el);
    std::vector<lxb_dom_element*> v;
    if (e && cls) {
        collect_class(e, cls, std::strlen(cls), v);
    }
    return make_list(v);
}

extern "C" whaleui_dom_list_t* whaleui_dom_element_get_elements_by_tag_name(whaleui_dom_element_t* el,
                                                                            const char* tag)
{
    lxb_dom_element* e = as_el(el);
    std::vector<lxb_dom_element*> v;
    if (e && tag) {
        collect_tag(e, tag, std::strlen(tag), v);
    }
    return make_list(v);
}

extern "C" whaleui_dom_element_t* whaleui_dom_closest(whaleui_dom_element_t* el,
                                                      const char* selector)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !selector || !*selector) {
        return nullptr;
    }
    size_t len = std::strlen(selector);
    for (lxb_dom_node* n = &e->node; n; n = n->parent) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            query_match(lxb_dom_interface_element(n), selector, len)) {
            return out(lxb_dom_interface_element(n));
        }
    }
    return nullptr;
}

extern "C" int whaleui_dom_matches(whaleui_dom_element_t* el, const char* selector)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !selector || !*selector) {
        return 0;
    }
    return query_match(e, selector, std::strlen(selector)) ? 1 : 0;
}

/* --- navigation --- */

extern "C" whaleui_dom_element_t* whaleui_dom_parent_element(whaleui_dom_element_t* el)
{
    return whaleui_dom_parent(el);
}

extern "C" whaleui_dom_element_t* whaleui_dom_last_child(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    return e ? out(last_element_child(e)) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_previous_sibling(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    return e ? out(prev_element(e)) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_first_element_child(whaleui_dom_element_t* el)
{
    return whaleui_dom_first_child(el);
}

extern "C" whaleui_dom_element_t* whaleui_dom_last_element_child(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    return e ? out(last_element_child(e)) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_next_element_sibling(whaleui_dom_element_t* el)
{
    return whaleui_dom_next_sibling(el);
}

extern "C" whaleui_dom_element_t* whaleui_dom_previous_element_sibling(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    return e ? out(prev_element(e)) : nullptr;
}

extern "C" whaleui_dom_list_t* whaleui_dom_children(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    std::vector<lxb_dom_element*> v;
    if (e) {
        for (lxb_dom_node* c = e->node.first_child; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                v.push_back(lxb_dom_interface_element(c));
            }
        }
    }
    return make_list(v);
}

extern "C" size_t whaleui_dom_child_element_count(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    size_t n = 0;
    if (e) {
        for (lxb_dom_node* c = e->node.first_child; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                ++n;
            }
        }
    }
    return n;
}

/* --- structure --- */

extern "C" whaleui_dom_element_t* whaleui_dom_create_text_node(whaleui_dom_document_t* doc,
                                                               const char* text)
{
    lxb_dom_document* d = dom_doc(doc);
    if (!d || !text) {
        return nullptr;
    }
    lxb_dom_text* tn = lxb_dom_document_create_text_node(
        d, reinterpret_cast<const lxb_char_t*>(text), std::strlen(text));
    /* a text node is not an element; the handle is the node pointer itself
     * (append_child inserts it as a node; tag_name on it is undefined) */
    return reinterpret_cast<whaleui_dom_element_t*>(tn);
}

extern "C" int whaleui_dom_insert_before(whaleui_dom_element_t* parent,
                                         whaleui_dom_element_t* new_child,
                                         whaleui_dom_element_t* ref_child)
{
    lxb_dom_element* p = as_el(parent);
    lxb_dom_node* nc = new_child ? reinterpret_cast<lxb_dom_node*>(new_child) : nullptr;
    lxb_dom_node* rc = ref_child ? &as_el(ref_child)->node : nullptr;
    if (!p || !nc) {
        return -1;
    }
    if (nc->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
        std::vector<lxb_dom_node*> kids;
        for (lxb_dom_node* c = nc->first_child; c;) {
            lxb_dom_node* nx = c->next;
            lxb_dom_node_remove(c);
            kids.push_back(c);
            c = nx;
        }
        for (size_t i = 0; i < kids.size(); ++i) {
            if (rc) {
                lxb_dom_node_insert(&p->node, kids[i], rc, false);
            } else {
                lxb_dom_node_insert_child(&p->node, kids[i]);
            }
        }
        whaleui_dom_mark_dirty_struct(p);
        return 0;
    }
    if (nc->parent) {
        /* moving: the old parent loses this subtree too */
        whaleui_dom_mark_dirty_struct(lxb_dom_interface_element(nc->parent));
        lxb_dom_node_remove(nc);
    }
    if (!rc) {
        lxb_dom_node_insert_child(&p->node, nc);
    } else {
        lxb_dom_node_insert(&p->node, nc, rc, false);
    }
    whaleui_dom_mark_dirty_struct(p);
    return 0;
}

extern "C" int whaleui_dom_replace_child(whaleui_dom_element_t* parent,
                                         whaleui_dom_element_t* new_child,
                                         whaleui_dom_element_t* old_child)
{
    lxb_dom_element* p = as_el(parent);
    lxb_dom_node* nc = new_child ? &as_el(new_child)->node : nullptr;
    lxb_dom_node* oc = old_child ? &as_el(old_child)->node : nullptr;
    if (!p || !nc || !oc) {
        return -1;
    }
    if (nc->parent) {
        /* moving: the old parent loses this subtree too */
        whaleui_dom_mark_dirty_struct(lxb_dom_interface_element(nc->parent));
        lxb_dom_node_remove(nc);
    }
    /* insert before old, then drop old (lexbor has no node_replace) */
    lxb_dom_node_insert(&p->node, nc, oc, false);
    lxb_dom_node_remove(oc);
    whaleui_dom_mark_dirty_struct(p);
    return 0;
}

extern "C" int whaleui_dom_remove(whaleui_dom_element_t* el)
{
    return whaleui_dom_remove_child(nullptr, el);
}

extern "C" whaleui_dom_element_t* whaleui_dom_clone(whaleui_dom_element_t* el, int deep)
{
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return nullptr;
    }
    lxb_dom_node* clone = lxb_dom_node_clone(&e->node, deep ? true : false);
    return clone ? out(lxb_dom_interface_element(clone)) : nullptr;
}

/* --- append/prepend/before/after (single-node forms) ---
 *
 * The node handle may be an element, a text node, or a document fragment:
 * all share lxb_dom_node as their first member (see dom.h / create_text_node),
 * so a plain reinterpret to the node pointer covers all three. */
static int node_handle(whaleui_dom_element_t* n, lxb_dom_node** out)
{
    if (!n) {
        *out = nullptr;
        return -1;
    }
    *out = reinterpret_cast<lxb_dom_node*>(n);
    return 0;
}

/* Insert `nc` under parent->node before ref (ref null = append). A document
 * fragment is not itself inserted: its children are moved before ref in
 * fragment order (browser appendChild(fragment) semantics), leaving the
 * fragment empty. The fragment itself is unlinked first if it was attached. */
static void splice_insert(lxb_dom_node* parent, lxb_dom_node* nc,
                          lxb_dom_node* ref)
{
    if (nc->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
        /* detach the fragment from any parent, then relocate its children */
        std::vector<lxb_dom_node*> kids;
        for (lxb_dom_node* c = nc->first_child; c;) {
            lxb_dom_node* nx = c->next;
            lxb_dom_node_remove(c);
            kids.push_back(c);
            c = nx;
        }
        /* insert before the same ref each time keeps fragment order (ref is
         * a sibling of parent, not a kid of the fragment, so it stays valid) */
        for (size_t i = 0; i < kids.size(); ++i) {
            if (ref) {
                lxb_dom_node_insert(parent, kids[i], ref, false);
            } else {
                lxb_dom_node_insert_child(parent, kids[i]);
            }
        }
        return;
    }
    if (nc->parent) {
        whaleui_dom_mark_dirty_struct(lxb_dom_interface_element(nc->parent));
        lxb_dom_node_remove(nc);
    }
    if (ref) {
        lxb_dom_node_insert(parent, nc, ref, false);
    } else {
        lxb_dom_node_insert_child(parent, nc);
    }
}

extern "C" int whaleui_dom_append(whaleui_dom_element_t* el,
                                  whaleui_dom_element_t* node)
{
    lxb_dom_element* e = as_el(el);
    lxb_dom_node* nc = nullptr;
    if (node_handle(node, &nc) != 0 || !e) {
        return -1;
    }
    splice_insert(&e->node, nc, nullptr);
    whaleui_dom_mark_dirty_struct(e);
    return 0;
}

extern "C" int whaleui_dom_prepend(whaleui_dom_element_t* el,
                                   whaleui_dom_element_t* node)
{
    lxb_dom_element* e = as_el(el);
    lxb_dom_node* nc = nullptr;
    if (node_handle(node, &nc) != 0 || !e) {
        return -1;
    }
    splice_insert(&e->node, nc, e->node.first_child);
    whaleui_dom_mark_dirty_struct(e);
    return 0;
}

extern "C" int whaleui_dom_before(whaleui_dom_element_t* el,
                                  whaleui_dom_element_t* node)
{
    lxb_dom_element* e = as_el(el);
    lxb_dom_node* nc = nullptr;
    if (node_handle(node, &nc) != 0 || !e || !e->node.parent) {
        return -1;
    }
    splice_insert(e->node.parent, nc, &e->node);
    whaleui_dom_mark_dirty_struct(lxb_dom_interface_element(e->node.parent));
    return 0;
}

extern "C" int whaleui_dom_after(whaleui_dom_element_t* el,
                                 whaleui_dom_element_t* node)
{
    lxb_dom_element* e = as_el(el);
    lxb_dom_node* nc = nullptr;
    if (node_handle(node, &nc) != 0 || !e || !e->node.parent) {
        return -1;
    }
    splice_insert(e->node.parent, nc, e->node.next);
    whaleui_dom_mark_dirty_struct(lxb_dom_interface_element(e->node.parent));
    return 0;
}

extern "C" int whaleui_dom_contains(whaleui_dom_element_t* parent,
                                    whaleui_dom_element_t* child)
{
    lxb_dom_element* p = as_el(parent);
    lxb_dom_element* c = as_el(child);
    if (!p || !c) {
        return 0;
    }
    for (lxb_dom_node* n = &c->node; n; n = n->parent) {
        if (n == &p->node) {
            return 1;
        }
    }
    return 0;
}

extern "C" int whaleui_dom_has_child_nodes(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    return e && e->node.first_child ? 1 : 0;
}

extern "C" int whaleui_dom_is_connected(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return 0;
    }
    for (lxb_dom_node* n = &e->node; n; n = n->parent) {
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
            return 1;
        }
    }
    return 0;
}

/* --- classList --- */

int class_has(lxb_dom_element* e, const char* cls)
{
    size_t clen = std::strlen(cls);
    size_t alen = 0;
    const lxb_char_t* a = lxb_dom_element_get_attribute(
        e, (const lxb_char_t*)"class", 5, &alen);
    if (!a) {
        return 0;
    }
    const lxb_char_t* c = a;
    const lxb_char_t* end = a + alen;
    while (c < end) {
        while (c < end && (*c == ' ' || *c == '\t')) {
            ++c;
        }
        const lxb_char_t* tok = c;
        while (c < end && *c != ' ' && *c != '\t') {
            ++c;
        }
        if (static_cast<size_t>(c - tok) == clen &&
            std::memcmp(tok, cls, clen) == 0) {
            return 1;
        }
    }
    return 0;
}

int class_set(lxb_dom_element* e, const char* cls, int add)
{
    size_t alen = 0;
    const lxb_char_t* a = lxb_dom_element_get_attribute(
        e, (const lxb_char_t*)"class", 5, &alen);
    std::string out;
    if (a && alen) {
        out.assign(reinterpret_cast<const char*>(a), alen);
    }
    if (add) {
        if (class_has(e, cls)) {
            return 0;
        }
        if (!out.empty() && out.back() != ' ') {
            out += ' ';
        }
        out += cls;
    } else {
        /* rebuild without the token */
        std::string nv;
        size_t clen = std::strlen(cls);
        size_t pos = 0;
        while (pos < out.size()) {
            size_t s = pos;
            while (s < out.size() && (out[s] == ' ' || out[s] == '\t')) {
                ++s;
            }
            size_t t = s;
            while (t < out.size() && out[t] != ' ' && out[t] != '\t') {
                ++t;
            }
            if (t > s && !(t - s == clen && out.compare(s, clen, cls) == 0)) {
                if (!nv.empty()) {
                    nv += ' ';
                }
                nv.append(out, s, t - s);
            }
            pos = t;
        }
        out.swap(nv);
    }
    return lxb_dom_element_set_attribute(e, (const lxb_char_t*)"class", 5,
                                         reinterpret_cast<const lxb_char_t*>(out.c_str()),
                                         out.size()) ? 0 : -2;
}

extern "C" int whaleui_dom_class_add(whaleui_dom_element_t* el, const char* cls)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !cls) {
        return -1;
    }
    int rc = class_set(e, cls, 1);
    if (rc == 0) {
        mark_dirty_attr(e, "class");
    }
    return rc;
}

extern "C" int whaleui_dom_class_remove(whaleui_dom_element_t* el, const char* cls)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !cls) {
        return -1;
    }
    int rc = class_set(e, cls, 0);
    if (rc == 0) {
        mark_dirty_attr(e, "class");
    }
    return rc;
}

extern "C" int whaleui_dom_class_toggle(whaleui_dom_element_t* el, const char* cls)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !cls) {
        return -1;
    }
    int has = class_has(e, cls);
    int rc = class_set(e, cls, !has);
    if (rc == 0) {
        mark_dirty_attr(e, "class");
    }
    return has ? 0 : 1;
}

extern "C" int whaleui_dom_class_contains(whaleui_dom_element_t* el, const char* cls)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !cls) {
        return 0;
    }
    return class_has(e, cls);
}

/* split the class attribute into whitespace-separated tokens */
std::vector<std::string> class_tokens(lxb_dom_element* e)
{
    std::vector<std::string> out;
    size_t alen = 0;
    const lxb_char_t* a = lxb_dom_element_get_attribute(
        e, (const lxb_char_t*)"class", 5, &alen);
    if (!a) {
        return out;
    }
    std::string s(reinterpret_cast<const char*>(a), alen);
    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
            ++pos;
        }
        size_t t = pos;
        while (t < s.size() && s[t] != ' ' && s[t] != '\t') {
            ++t;
        }
        if (t > pos) {
            out.push_back(s.substr(pos, t - pos));
        }
        pos = t;
    }
    return out;
}

extern "C" size_t whaleui_dom_class_length(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    return e ? class_tokens(e).size() : 0;
}

extern "C" int whaleui_dom_class_item(whaleui_dom_element_t* el, size_t index,
                                      char* out, size_t cap)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !out || cap == 0) {
        return -1;
    }
    std::vector<std::string> v = class_tokens(e);
    if (index >= v.size()) {
        return -2;
    }
    if (v[index].size() + 1 > cap) {
        return -3;
    }
    std::memcpy(out, v[index].c_str(), v[index].size() + 1);
    return 0;
}

extern "C" int whaleui_dom_class_replace(whaleui_dom_element_t* el,
                                         const char* old_cls,
                                         const char* new_cls)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !old_cls || !new_cls || !*old_cls || !*new_cls) {
        return 0;
    }
    if (std::strcmp(old_cls, new_cls) == 0) {
        return class_has(e, old_cls); /* no-op; report presence */
    }
    int found = class_has(e, old_cls);
    if (!found) {
        return 0;
    }
    class_set(e, old_cls, 0);
    class_set(e, new_cls, 1);
    mark_dirty_attr(e, "class");
    return 1;
}

extern "C" int whaleui_dom_class_toggle_force(whaleui_dom_element_t* el,
                                              const char* cls, int force)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !cls) {
        return -1;
    }
    int has = class_has(e, cls);
    int want = force >= 0 ? (force != 0) : !has;
    if (want == has) {
        return has;
    }
    int rc = class_set(e, cls, want);
    if (rc != 0) {
        return -1;
    }
    mark_dirty_attr(e, "class");
    return want;
}

/* --- dataset (element.dataset.*) ---
 * camelCase key <-> data-kebab-case attribute. A key already holding a '-'
 * (or non-alphanumeric, e.g. a digit run) is not split further by the
 * browser's algorithm; only [a-z0-9]+ boundaries between a lowercase/number
 * and an uppercase letter insert "-lower". We match that: every uppercase at
 * index>0 that follows a lowercase-or-digit gets a hyphen. */
void dataset_attr_name(const char* key, std::string& out)
{
    out = "data-";
    if (!key) {
        return;
    }
    for (const char* p = key; *p; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z' && p != key) {
            out += '-';
            c = static_cast<char>(c - 'A' + 'a');
        }
        out += c;
    }
}

extern "C" int whaleui_dom_dataset_set(whaleui_dom_element_t* el,
                                       const char* key, const char* value)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !key || !*key || !value) {
        return -1;
    }
    std::string name;
    dataset_attr_name(key, name);
    int rc = lxb_dom_element_set_attribute(
                 e, reinterpret_cast<const lxb_char_t*>(name.c_str()),
                 name.size(), reinterpret_cast<const lxb_char_t*>(value),
                 std::strlen(value))
                 ? 0
                 : -2;
    if (rc == 0) {
        mark_dirty_attr(e, name.c_str());
    }
    return rc;
}

extern "C" const char* whaleui_dom_dataset_get(whaleui_dom_element_t* el,
                                               const char* key)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !key || !*key) {
        return nullptr;
    }
    std::string name;
    dataset_attr_name(key, name);
    size_t len = 0;
    return reinterpret_cast<const char*>(lxb_dom_element_get_attribute(
        e, reinterpret_cast<const lxb_char_t*>(name.c_str()), name.size(),
        &len));
}

extern "C" int whaleui_dom_dataset_has(whaleui_dom_element_t* el, const char* key)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !key || !*key) {
        return 0;
    }
    std::string name;
    dataset_attr_name(key, name);
    return lxb_dom_element_has_attribute(
               e, reinterpret_cast<const lxb_char_t*>(name.c_str()), name.size())
               ? 1
               : 0;
}

extern "C" int whaleui_dom_dataset_remove(whaleui_dom_element_t* el, const char* key)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !key || !*key) {
        return -1;
    }
    std::string name;
    dataset_attr_name(key, name);
    int rc = lxb_dom_element_remove_attribute(
                 e, reinterpret_cast<const lxb_char_t*>(name.c_str()),
                 name.size()) == 0
                 ? 0
                 : -2;
    if (rc == 0) {
        mark_dirty_attr(e, name.c_str());
    }
    return rc;
}

/* --- HTML serialization (innerHTML / outerHTML) --- */

bool is_void_tag(lxb_dom_element* e)
{
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(e, &len);
    static const char* kVoid[] = {"br", "img", "hr", "input", "meta", "link",
                                  "base", "source", "track", "wbr", "col",
                                  "param", "area", "embed"};
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kVoid) / sizeof(kVoid[0]); ++i) {
        if (len == std::strlen(kVoid[i]) &&
            std::memcmp(name, kVoid[i], len) == 0) {
            return true;
        }
    }
    return false;
}

void escape_text(const std::string& s, std::string& out)
{
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '>') {
            out += "&gt;";
        } else {
            out += c;
        }
    }
}

void serialize_open(lxb_dom_element* e, std::string& out)
{
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(e, &len);
    out += '<';
    if (name) {
        out.append(reinterpret_cast<const char*>(name), len);
    }
    lxb_dom_attr_t* a = lxb_dom_element_first_attribute(e);
    while (a) {
        size_t nlen = 0;
        const lxb_char_t* nm = lxb_dom_attr_qualified_name(a, &nlen);
        size_t vlen = 0;
        const lxb_char_t* v = a->value ? lxb_dom_attr_value(a, &vlen) : nullptr;
        if (nm) {
            out += ' ';
            out.append(reinterpret_cast<const char*>(nm), nlen);
            if (v) {
                out += "=\"";
                out.append(reinterpret_cast<const char*>(v), vlen);
                out += '"';
            }
        }
        a = lxb_dom_element_next_attribute(a);
    }
    out += '>';
}

void serialize_nodes(lxb_dom_node* n, std::string& out)
{
    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element* e = lxb_dom_interface_element(n);
            serialize_open(e, out);
            if (!is_void_tag(e)) {
                serialize_nodes(e->node.first_child, out);
                size_t len = 0;
                const lxb_char_t* name = lxb_dom_element_local_name(e, &len);
                out += "</";
                if (name) {
                    out.append(reinterpret_cast<const char*>(name), len);
                }
                out += '>';
            }
        } else if (n->type == LXB_DOM_NODE_TYPE_TEXT ||
                   n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
            const lexbor_str_t* s = &lxb_dom_interface_text(n)->char_data.data;
            if (s->data) {
                escape_text(std::string(reinterpret_cast<const char*>(s->data),
                                        s->length),
                            out);
            }
        }
        n = n->next;
    }
}

/* parse an HTML fragment into `el`'s children (owned by the same document).
 * Returns 0 on success. */
int parse_into(lxb_dom_element* el, const char* html, size_t len)
{
    lxb_dom_document* doc = el->node.owner_document;
    /* an lxb_dom_document is the first member of lxb_html_document */
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    if (!hd) {
        return -2;
    }
    lxb_html_parser_t* parser = lxb_html_parser_create();
    if (!parser) {
        return -3;
    }
    lxb_html_parser_init(parser);
    lxb_dom_node_t* frag = lxb_html_parse_fragment_by_tag_id(
        parser, hd, LXB_TAG_DIV, LXB_NS_HTML,
        reinterpret_cast<const lxb_char_t*>(html), len);
    if (!frag) {
        lxb_html_parser_destroy(parser);
        return -4;
    }
    /* move the parsed children under el */
    while (frag->first_child) {
        lxb_dom_node* c = frag->first_child;
        lxb_dom_node_remove(c);
        lxb_dom_node_insert_child(&el->node, c);
    }
    lxb_html_parser_destroy(parser);
    return 0;
}

extern "C" int whaleui_dom_set_inner_html(whaleui_dom_element_t* el,
                                          const char* html)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !html) {
        return -1;
    }
    /* clear children */
    lxb_dom_node* n = e->node.first_child;
    while (n) {
        lxb_dom_node* nx = n->next;
        lxb_dom_node_remove(n);
        lxb_dom_node_destroy(n);
        n = nx;
    }
    whaleui_dom_mark_dirty_struct(e);
    if (!*html) {
        return 0;
    }
    /* fresh subtree may carry style attributes with --var definitions */
    whaleui_dom_mark_vars_dirty();
    int rc = parse_into(e, html, std::strlen(html));
    if (rc == 0) {
        whaleui_dom_mark_dirty_struct(e);
    }
    return rc;
}

extern "C" const char* whaleui_dom_get_inner_html(whaleui_dom_element_t* el)
{
    static std::string cache;
    cache.clear();
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return nullptr;
    }
    serialize_nodes(e->node.first_child, cache);
    return cache.c_str();
}

extern "C" int whaleui_dom_set_outer_html(whaleui_dom_element_t* el,
                                          const char* html)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !html) {
        return -1;
    }
    lxb_dom_node* parent = e->node.parent;
    if (!parent) {
        return -2;
    }
    lxb_dom_node* prev = e->node.prev;
    if (parse_into(e, html, std::strlen(html)) != 0) {
        return -3;
    }
    /* move the parsed children out to replace the element itself */
    while (e->node.first_child) {
        lxb_dom_node* c = e->node.first_child;
        lxb_dom_node_remove(c);
        if (prev) {
            lxb_dom_node_insert_after(prev, c);
        } else {
            lxb_dom_node_insert(parent, c, parent->first_child, false);
        }
        prev = c;
    }
    lxb_dom_node_remove(&e->node);
    lxb_dom_node_destroy(&e->node);
    /* the element is gone: the parent's subtree must rebuild */
    whaleui_dom_mark_dirty_struct(lxb_dom_interface_element(parent));
    /* the fresh siblings may carry style attributes with --var definitions */
    whaleui_dom_mark_vars_dirty();
    return 0;
}

extern "C" const char* whaleui_dom_get_outer_html(whaleui_dom_element_t* el)
{
    static std::string cache;
    cache.clear();
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return nullptr;
    }
    serialize_nodes(&e->node, cache);
    return cache.c_str();
}

/* --- form values (el.value) --- */

extern "C" int whaleui_dom_set_value(whaleui_dom_element_t* el, const char* value)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !value) {
        return -1;
    }
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(e, &len);
    if (!name) {
        return -1;
    }
    if (len == 5 && std::memcmp(name, "input", 5) == 0) {
        return whaleui_dom_set_attribute(el, "value", value);
    }
    if (len == 8 && std::memcmp(name, "textarea", 8) == 0) {
        return whaleui_dom_set_text(el, value);
    }
    if (len == 6 && std::memcmp(name, "select", 6) == 0) {
        /* select the option whose value matches */
        for (lxb_dom_node* c = e->node.first_child; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element* opt = lxb_dom_interface_element(c);
                size_t olen = 0;
                const lxb_char_t* nm = lxb_dom_element_local_name(opt, &olen);
                if (nm && olen == 6 && std::memcmp(nm, "option", 6) == 0) {
                    size_t vlen = 0;
                    const lxb_char_t* v = lxb_dom_element_get_attribute(
                        opt, (const lxb_char_t*)"value", 5, &vlen);
                    if ((v && vlen == std::strlen(value) &&
                         std::memcmp(v, value, vlen) == 0) ||
                        (!v && value[0] == '\0')) {
                        lxb_dom_element_set_attribute(
                            opt, (const lxb_char_t*)"selected", 8,
                            (const lxb_char_t*)"", 0);
                    } else {
                        lxb_dom_element_remove_attribute(
                            opt, (const lxb_char_t*)"selected", 8);
                    }
                }
            }
        }
        return 0;
    }
    return -2; /* not a form control */
}

extern "C" const char* whaleui_dom_get_value(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return nullptr;
    }
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(e, &len);
    if (!name) {
        return nullptr;
    }
    if (len == 5 && std::memcmp(name, "input", 5) == 0) {
        size_t vlen = 0;
        const lxb_char_t* v = lxb_dom_element_get_attribute(
            e, (const lxb_char_t*)"value", 5, &vlen);
        return v ? reinterpret_cast<const char*>(v) : "";
    }
    if (len == 6 && std::memcmp(name, "select", 6) == 0) {
        for (lxb_dom_node* c = e->node.first_child; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element* opt = lxb_dom_interface_element(c);
                size_t olen = 0;
                const lxb_char_t* nm = lxb_dom_element_local_name(opt, &olen);
                if (nm && olen == 6 && std::memcmp(nm, "option", 6) == 0) {
                    if (lxb_dom_element_has_attribute(
                            opt, (const lxb_char_t*)"selected", 8)) {
                        size_t vlen = 0;
                        const lxb_char_t* v = lxb_dom_element_get_attribute(
                            opt, (const lxb_char_t*)"value", 5, &vlen);
                        static std::string sel;
                        sel.assign(reinterpret_cast<const char*>(
                                       v ? v : (const lxb_char_t*)""),
                                   v ? vlen : 0);
                        return sel.c_str();
                    }
                }
            }
        }
        return "";
    }
    if (len == 8 && std::memcmp(name, "textarea", 8) == 0) {
        return whaleui_dom_get_text(el);
    }
    return nullptr; /* not a form control */
}

/* --- focus / blur (document.activeElement mirror + non-bubbling events) ---
 *
 * The engine's real focus lives in the render context (click / Tab move it
 * through whaleui_dom_set_active). The public API records the DOM-layer
 * state + fires focus/blur (non-bubbling, like the browser); :focus STYLES
 * sync on the next frame when the window is visible (the render focus is
 * only moved by real input). */

extern "C" int whaleui_dom_focus(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return -1;
    }
    lxb_dom_document* d = e->node.owner_document;
    lxb_dom_element* old = nullptr;
    std::map<lxb_dom_document*, lxb_dom_element*>::iterator it = g_active().find(d);
    if (it != g_active().end()) {
        old = it->second;
    }
    if (old == e) {
        return 0; /* already the active element */
    }
    if (old) {
        WUI_DOM_FIRE_NB(out(old), "blur", 0, 0, 0, 0, 0, 0);
    }
    whaleui_dom_set_active(e);
    WUI_DOM_FIRE_NB(el, "focus", 0, 0, 0, 0, 0, 0);
    return 0;
}

extern "C" int whaleui_dom_blur(whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!e) {
        return -1;
    }
    lxb_dom_document* d = e->node.owner_document;
    std::map<lxb_dom_document*, lxb_dom_element*>::iterator it = g_active().find(d);
    if (it == g_active().end() || it->second != e) {
        return 0; /* only the active element blur-fires */
    }
    it->second = nullptr;
    WUI_DOM_FIRE_NB(el, "blur", 0, 0, 0, 0, 0, 0);
    return 0;
}

/* --- geometry: from the last layout tree (render module) --- */

extern "C" int whaleui_dom_get_bounding_client_rect(whaleui_dom_element_t* el,
                                                    whaleui_dom_rect_t* out)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !out) {
        return -1;
    }
    whaleui_layout_tree_t* tree = whaleui_render_last_tree();
    if (!tree) {
        return -2; /* never rendered */
    }
    for (auto& n : tree->arena) {
        if (n.el == e && !n.is_text) {
            out->x = static_cast<float>(n.border.x);
            out->y = static_cast<float>(n.border.y);
            out->width = static_cast<float>(n.border.w);
            out->height = static_cast<float>(n.border.h);
            return 0;
        }
    }
    return -3; /* not in the last layout (display:none or detached) */
}

/* --- Range (document.createRange() subset) ---
 *
 * The endpoint model matches the engine's selection: a container ELEMENT
 * plus a UTF-8 byte offset into its text. A text-node container resolves to
 * its parent element (the engine addresses text by element + byte offset),
 * with the offset left as given - the caller's node/offset pair is the
 * browser's addressing, so the mapping is intentionally 1:1. */

/* text of a range endpoint element: input -> its value attribute, otherwise
 * the concatenated text children (textContent), matching the render side's
 * edit_value. */
static std::string edit_value_of(lxb_dom_element* el)
{
    if (!el) {
        return std::string();
    }
    size_t nlen = 0;
    const lxb_char_t* nm = lxb_dom_element_local_name(el, &nlen);
    if (nm && nlen == 5 && std::memcmp(nm, "input", 5) == 0) {
        size_t alen = 0;
        const lxb_char_t* v = lxb_dom_element_get_attribute(
            el, (const lxb_char_t*)"value", 5, &alen);
        return v ? std::string(reinterpret_cast<const char*>(v), alen)
                 : std::string();
    }
    std::string out;
    for (lxb_dom_node* n = el->node.first_child; n; n = n->next) {
        if (n->type == LXB_DOM_NODE_TYPE_TEXT ||
            n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
            const lexbor_str_t* s = &lxb_dom_interface_text(n)->char_data.data;
            if (s->data) {
                out.append(reinterpret_cast<const char*>(s->data), s->length);
            }
        }
    }
    return out;
}

/* write text back: input -> value attribute, otherwise replace children with
 * one text node (mirrors the render side's edit_set_value). */
static void edit_set_value_of(lxb_dom_element* el, const std::string& s)
{
    if (!el) {
        return;
    }
    size_t nlen = 0;
    const lxb_char_t* nm = lxb_dom_element_local_name(el, &nlen);
    if (nm && nlen == 5 && std::memcmp(nm, "input", 5) == 0) {
        lxb_dom_element_set_attribute(
            el, (const lxb_char_t*)"value", 5,
            reinterpret_cast<const lxb_char_t*>(s.c_str()), s.size());
        return;
    }
    lxb_dom_node* n = el->node.first_child;
    while (n) {
        lxb_dom_node* nx = n->next;
        lxb_dom_node_remove(n);
        lxb_dom_node_destroy(n);
        n = nx;
    }
    lxb_dom_text* tn = lxb_dom_document_create_text_node(
        el->node.owner_document, reinterpret_cast<const lxb_char_t*>(s.c_str()),
        s.size());
    if (tn) {
        lxb_dom_node_insert_child(&el->node, lxb_dom_interface_node(tn));
    }
}

struct whaleui_dom_range
{
    lxb_dom_element* start_el;
    lxb_dom_element* end_el;
    int start_off;
    int end_off;
    /* cached to_string result (the returned pointer must stay valid) */
    std::string text_cache;
};

/* resolve a container handle (element OR text node) to the element that owns
 * the text the offset indexes */
static lxb_dom_element* range_owner(whaleui_dom_element_t* container)
{
    if (!container) {
        return nullptr;
    }
    lxb_dom_node* n = reinterpret_cast<lxb_dom_node*>(container);
    if (n->type == LXB_DOM_NODE_TYPE_TEXT ||
        n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
        lxb_dom_node* p = n->parent;
        if (!p || p->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            return nullptr;
        }
        return lxb_dom_interface_element(p);
    }
    return lxb_dom_interface_element(n);
}

extern "C" whaleui_dom_range_t* whaleui_dom_create_range(whaleui_dom_document_t* doc)
{
    if (!doc) {
        return nullptr;
    }
    whaleui_dom_range_t* r = new whaleui_dom_range_t;
    r->start_el = r->end_el = nullptr;
    r->start_off = r->end_off = 0;
    return r;
}

extern "C" void whaleui_dom_range_destroy(whaleui_dom_range_t* range)
{
    delete range;
}

extern "C" int whaleui_dom_range_set_start(whaleui_dom_range_t* range,
                                           whaleui_dom_element_t* container,
                                           int offset)
{
    if (!range) {
        return -1;
    }
    lxb_dom_element* e = range_owner(container);
    if (!e) {
        return -2;
    }
    range->start_el = e;
    range->start_off = offset < 0 ? 0 : offset;
    return 0;
}

extern "C" int whaleui_dom_range_set_end(whaleui_dom_range_t* range,
                                         whaleui_dom_element_t* container,
                                         int offset)
{
    if (!range) {
        return -1;
    }
    lxb_dom_element* e = range_owner(container);
    if (!e) {
        return -2;
    }
    range->end_el = e;
    range->end_off = offset < 0 ? 0 : offset;
    return 0;
}

extern "C" whaleui_dom_element_t* whaleui_dom_range_start_container(const whaleui_dom_range_t* range)
{
    return range ? out(range->start_el) : nullptr;
}

extern "C" whaleui_dom_element_t* whaleui_dom_range_end_container(const whaleui_dom_range_t* range)
{
    return range ? out(range->end_el) : nullptr;
}

extern "C" int whaleui_dom_range_start_offset(const whaleui_dom_range_t* range)
{
    return range ? range->start_off : 0;
}

extern "C" int whaleui_dom_range_end_offset(const whaleui_dom_range_t* range)
{
    return range ? range->end_off : 0;
}

extern "C" int whaleui_dom_range_collapsed(const whaleui_dom_range_t* range)
{
    if (!range) {
        return 1;
    }
    return (range->start_el == range->end_el &&
            range->start_off == range->end_off) ? 1 : 0;
}

/* collect the text of every element between two endpoints in DOM preorder,
 * mirroring the selection's coverage rules (partial ends, full middles) */
static std::string range_text_between(lxb_dom_element* sa, int so,
                                      lxb_dom_element* se, int eo)
{
    std::string out;
    if (!sa || !se) {
        return out;
    }
    if (sa == se) {
        std::string v = edit_value_of(sa);
        size_t a = static_cast<size_t>(so);
        size_t b = static_cast<size_t>(eo);
        if (a > b) {
            std::swap(a, b);
        }
        if (a >= v.size()) {
            return out;
        }
        if (b > v.size()) {
            b = v.size();
        }
        return v.substr(a, b - a);
    }
    bool saw_start = false;
    bool saw_end = false;
    std::function<void(lxb_dom_node*)> walk = [&](lxb_dom_node* n) {
        while (n && !saw_end) {
            if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element* e = lxb_dom_interface_element(n);
                if (e == sa && !saw_start) {
                    saw_start = true;
                    std::string v = edit_value_of(sa);
                    size_t a = static_cast<size_t>(so);
                    if (a < v.size()) {
                        out += v.substr(a);
                    }
                } else if (e == se && saw_start) {
                    saw_end = true;
                    std::string v = edit_value_of(se);
                    size_t b = static_cast<size_t>(eo);
                    if (b > v.size()) {
                        b = v.size();
                    }
                    out += v.substr(0, b);
                    return;
                } else if (saw_start) {
                    out += edit_value_of(e);
                }
                if (n->first_child) {
                    walk(n->first_child);
                }
            }
            n = n->next;
        }
    };
    lxb_dom_node* root = sa->node.owner_document
        ? reinterpret_cast<lxb_dom_node*>(
              lxb_dom_document_element(sa->node.owner_document))
        : nullptr;
    if (root) {
        walk(root);
    }
    return out;
}

extern "C" const char* whaleui_dom_range_to_string(const whaleui_dom_range_t* range)
{
    if (!range) {
        return "";
    }
    /* the cache lives on the range (mutable through the const handle: the
     * returned pointer is the documented contract, not const-correctness) */
    whaleui_dom_range_t* m = const_cast<whaleui_dom_range_t*>(range);
    m->text_cache = range_text_between(m->start_el, m->start_off, m->end_el,
                                       m->end_off);
    return m->text_cache.c_str();
}

extern "C" int whaleui_dom_range_select_node_contents(whaleui_dom_range_t* range,
                                                      whaleui_dom_element_t* el)
{
    lxb_dom_element* e = as_el(el);
    if (!range || !e) {
        return -1;
    }
    range->start_el = e;
    range->end_el = e;
    range->start_off = 0;
    range->end_off = static_cast<int>(edit_value_of(e).size());
    return 0;
}

extern "C" int whaleui_dom_range_delete_contents(whaleui_dom_range_t* range)
{
    if (!range || !range->start_el || !range->end_el) {
        return -1;
    }
    if (whaleui_dom_range_collapsed(range)) {
        return 0; /* nothing covered */
    }
    if (range->start_el == range->end_el) {
        std::string v = edit_value_of(range->start_el);
        size_t a = static_cast<size_t>(range->start_off);
        size_t b = static_cast<size_t>(range->end_off);
        if (a > b) {
            std::swap(a, b);
        }
        if (a > v.size()) {
            a = v.size();
        }
        if (b > v.size()) {
            b = v.size();
        }
        std::string nv = v.substr(0, a) + v.substr(b);
        edit_set_value_of(range->start_el, nv);
        range->end_off = range->start_off = static_cast<int>(a);
        whaleui_dom_mark_dirty_struct(range->start_el);
        return 0;
    }
    /* cross-element: clear the covered middles entirely and trim the ends.
     * A full structural delete needs a text-node model the engine does not
     * have; trimming each editable endpoint is the documented behavior. */
    std::string sv = edit_value_of(range->start_el);
    size_t sa = static_cast<size_t>(range->start_off);
    if (sa > sv.size()) {
        sa = sv.size();
    }
    edit_set_value_of(range->start_el, sv.substr(0, sa));
    std::string ev = edit_value_of(range->end_el);
    size_t eb = static_cast<size_t>(range->end_off);
    if (eb > ev.size()) {
        eb = ev.size();
    }
    edit_set_value_of(range->end_el, ev.substr(eb));
    range->start_el = range->end_el;
    range->start_off = range->end_off = 0;
    whaleui_dom_mark_dirty_struct(range->start_el);
    return 0;
}
