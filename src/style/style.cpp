/* Style engine: selector matching, cascade, var() resolution.
 *
 * Computed styles are plain property->value maps. Cascade order:
 * !important > specificity (id, class, type) > source order; inline style
 * overrides everything except !important. */

#include "style/style.h"

#include "dom/dom.h"       /* whaleui_dom_mark_css_dirty */
#include "render/render.h" /* whaleui_render_parse_color */

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <functional>
#include <vector>

namespace {

/* --- selector parsing --- */

struct SelPart
{
    std::string tag;    /* "" = any, "*" = any */
    std::string id;     /* "" = any */
    std::string cls;    /* first class only ("" = any) */
    bool hover;         /* ":hover" pseudo-class present */
    bool active;        /* ":active" present */
    bool focus;         /* ":focus" / ":focus-visible" present */
    bool disabled;      /* ":disabled" present */
    bool checked;       /* ":checked" present (checked / selected attr) */
    bool indeterminate; /* ":indeterminate" present */
    bool last_child;    /* ":last-child" present */
    bool first_child;   /* ":first-child" present */
    bool nth;           /* ":nth-child(An+B)" present */
    int nth_a, nth_b;   /* position p matches when p = A*n + B, n >= 0 */
    bool pseudo_el;     /* "::before"/"::after" suffix: never matches el */
    bool has_attr;      /* "[name...]" present */
    std::string attr;   /* attribute name */
    std::string attr_val; /* expected value ("" = presence only) */
    int attr_op;        /* 0==presence, 1 '=' exact, 2 '^=' prefix,
                         * 3 '$=' suffix, 4 '*=' substring, 5 '~=' word,
                         * 6 '|=' hyphen-prefix */
    bool enabled;       /* ":enabled" present */
    bool required;      /* ":required" present */
    bool optional;      /* ":optional" present */
    bool empty;         /* ":empty" present */
    bool link;          /* ":link"/":visited" present (any <a href>) */
    bool focus_within;  /* ":focus-within" present */
    std::string is_sel; /* ":is(...)" inner selector list (comma-separated) */
    bool has_not;       /* ":not(<simple>)" present: matches when the inner
                         * simple selector does NOT match (single level,
                         * no combinators inside, per CSS) */
    std::string not_sel; /* the inner selector text */
    std::vector<std::string> has_sel; /* ":has(<relative>)" inners (each must
                         * match; multiple :has() concatenate) */
    bool not_hover;     /* ":not(:hover)" - state negation, checked with
                         * the same bubble rule as :hover in match() */
    bool not_active;    /* ":not(:active)" */
    bool not_focus;     /* ":not(:focus)" / ":not(:focus-visible)" */
};

/* parse ":nth-child(odd|even|N|n|n+B|An+B)" into a/b. Returns false on
 * malformed input. */
bool parse_nth(const char* s, size_t len, int& a, int& b)
{
    a = 0;
    b = 0;
    std::string v(s, len);
    size_t trim_b = v.find_first_not_of(" \t");
    size_t trim_e = v.find_last_not_of(" \t");
    if (trim_b == std::string::npos) {
        return false;
    }
    v = v.substr(trim_b, trim_e - trim_b + 1);
    if (v == "odd") {
        a = 2;
        b = 1;
        return true;
    }
    if (v == "even") {
        a = 2;
        b = 0;
        return true;
    }
    /* pure number */
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (end && *end == '\0' && v.size()) {
        a = 0;
        b = static_cast<int>(n);
        return true;
    }
    /* An+B / n+B / n-B */
    size_t npos = v.find('n');
    if (npos == std::string::npos) {
        return false;
    }
    std::string coef = v.substr(0, npos);
    if (coef.empty() || coef == "+") {
        a = 1;
    } else if (coef == "-") {
        a = -1;
    } else {
        a = std::atoi(coef.c_str());
    }
    std::string rest = v.substr(npos + 1);
    if (rest.empty()) {
        b = 0;
    } else {
        b = std::atoi(rest.c_str());
    }
    return true;
}

/* parse "tag#id.cls:hover" into SelPart (other pseudo-classes ignored) */
bool parse_simple(const char* sel, size_t len, SelPart& out)
{
    out = SelPart();
    if (len == 1 && sel[0] == '*') {
        return true; /* universal selector matches any element */
    }
    const char* p = sel;
    const char* end = sel + len;
    if (p < end && *p != '#' && *p != '.') {
        const char* t = p;
        while (p < end && *p != '#' && *p != '.' && *p != ':' &&
               *p != '[') {
            ++p;
        }
        out.tag.assign(t, static_cast<size_t>(p - t));
        if (out.tag == "*") {
            out.tag.clear(); /* universal selector */
        }
    }
    while (p < end) {
        if (*p == '[') {
            /* attribute selector: [name] presence, [name="value"] equality,
             * [name^="v"] prefix / [name$="v"] suffix / [name*="v"]
             * substring / [name~="v"] word / [name|="v"] hyphen-prefix.
             * Quotes inside the value are handled so "=" within them is not
             * mistaken for the operator. */
            ++p;
            const char* as = p;
            while (p < end && *p != ']' && *p != '=' && *p != '^' &&
                   *p != '$' && *p != '*' && *p != '~' && *p != '|') {
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    while (p < end && *p != q) {
                        ++p;
                    }
                    if (p < end) {
                        ++p;
                    }
                    continue;
                }
                ++p;
            }
            std::string aname(as, static_cast<size_t>(p - as));
            size_t ab = aname.find_first_not_of(" \t");
            size_t ae = aname.find_last_not_of(" \t");
            if (ab != std::string::npos) {
                out.attr = aname.substr(ab, ae - ab + 1);
                out.has_attr = true;
                out.attr_op = 0;
            }
            if (p < end && (*p == '^' || *p == '$' || *p == '*' ||
                            *p == '~' || *p == '|')) {
                char oc = *p++;
                if (p < end && *p == '=') {
                    ++p;
                    out.attr_op = oc == '^' ? 2
                                  : oc == '$' ? 3
                                  : oc == '*' ? 4
                                  : oc == '~' ? 5
                                              : 6;
                } else {
                    /* stray operator char: skip to ']' (malformed) */
                    while (p < end && *p != ']') {
                        ++p;
                    }
                }
                const char* vs = p;
                if (p < end && (*p == '"' || *p == '\'')) {
                    char q = *p++;
                    const char* v0 = p;
                    while (p < end && *p != q) {
                        ++p;
                    }
                    out.attr_val.assign(v0, static_cast<size_t>(p - v0));
                    if (p < end) {
                        ++p;
                    }
                } else {
                    while (p < end && *p != ']') {
                        ++p;
                    }
                    out.attr_val.assign(vs, static_cast<size_t>(p - vs));
                    size_t vb = out.attr_val.find_first_not_of(" \t");
                    size_t ve = out.attr_val.find_last_not_of(" \t");
                    if (vb != std::string::npos) {
                        out.attr_val = out.attr_val.substr(vb, ve - vb + 1);
                    }
                }
            } else if (p < end && *p == '=') {
                out.attr_op = 1;
                ++p;
                const char* vs = p;
                if (p < end && (*p == '"' || *p == '\'')) {
                    char q = *p++;
                    const char* v0 = p;
                    while (p < end && *p != q) {
                        ++p;
                    }
                    out.attr_val.assign(v0, static_cast<size_t>(p - v0));
                    if (p < end) {
                        ++p;
                    }
                } else {
                    while (p < end && *p != ']') {
                        ++p;
                    }
                    out.attr_val.assign(vs, static_cast<size_t>(p - vs));
                    size_t vb = out.attr_val.find_first_not_of(" \t");
                    size_t ve = out.attr_val.find_last_not_of(" \t");
                    if (vb != std::string::npos) {
                        out.attr_val = out.attr_val.substr(vb, ve - vb + 1);
                    }
                }
            }
            while (p < end && *p != ']') {
                ++p;
            }
            if (p < end) {
                ++p; /* skip ']' */
            }
            continue;
        }
        if (*p == ':') {
            ++p;
            /* "::after"/"::before" pseudo-element: never matches the
             * element itself (only ::-rules carry paint; element rules
             * must not leak position/height/etc. into the element). */
            if (p < end && *p == ':') {
                out.pseudo_el = true;
                ++p;
                while (p < end && *p != '#' && *p != '.' && *p != ':' &&
                       *p != '[') {
                    ++p;
                }
                continue;
            }
            const char* s = p;
            /* ":not(...)" spans the class/id/attr/state markers inside its
             * own parentheses (":not(.a)", ":not(:hover)"), so the
             * delimiters only terminate at depth 0: a bare ':' starts a
             * NEW pseudo-class there, '#' / '.' / '[' start a new
             * id/class/attr segment */
            int pdepth = 0;
            while (p < end && (pdepth > 0 || (*p != '#' && *p != '.' &&
                                             *p != '[' && *p != ':'))) {
                if (*p == '(') {
                    ++pdepth;
                } else if (*p == ')') {
                    if (pdepth > 0) {
                        --pdepth;
                    }
                }
                ++p;
            }
            std::string v(s, static_cast<size_t>(p - s));
            if (v == "hover") {                out.hover = true;
            } else if (v == "active") {
                out.active = true;
            } else if (v == "focus" || v == "focus-visible") {
                out.focus = true;
            } else if (v == "disabled") {
                out.disabled = true;
            } else if (v == "enabled") {
                out.enabled = true;
            } else if (v == "required") {
                out.required = true;
            } else if (v == "optional") {
                out.optional = true;
            } else if (v == "empty") {
                out.empty = true;
            } else if (v == "link" || v == "visited") {
                out.link = true;
            } else if (v == "focus-within") {
                out.focus_within = true;
            } else if ((v.compare(0, 3, "is(") == 0 ||
                        v.compare(0, 6, "where(") == 0) &&
                       !v.empty() && v.back() == ')') {
                /* ":is(a, b)" / ":where(a, b)" - match if ANY inner
                 * selector matches (single-level, like :not; :where has
                 * zero specificity but we do not discount it - the
                 * cascade rarely depends on that here). */
                size_t open = v.find('(');
                std::string inner = v.substr(open + 1, v.size() - open - 2);
                size_t b = inner.find_first_not_of(" \t");
                size_t e = inner.find_last_not_of(" \t");
                if (b != std::string::npos) {
                    out.is_sel = inner.substr(b, e - b + 1);
                }
            } else if (v == "checked") {
                out.checked = true;
            } else if (v == "indeterminate") {
                out.indeterminate = true;
            } else if (v == "last-child") {
                out.last_child = true;
            } else if (v == "first-child") {
                out.first_child = true;
            } else if (v.compare(0, 10, "nth-child(") == 0 &&
                       !v.empty() && v.back() == ')') {
                /* "nth-child(" is 10 chars; inner expr = v[10..size-2] */
                if (parse_nth(v.c_str() + 10, v.size() - 11, out.nth_a,
                              out.nth_b)) {
                    out.nth = true;
                }
            } else if (v.compare(0, 4, "not(") == 0 &&
                       !v.empty() && v.back() == ')') {
                /* ":not(<simple>)" - single-level negation (no combinators
                 * inside per CSS). The inner text is stored raw and matched
                 * against the same element in part_match. Malformed or
                 * combinator-carrying arguments are ignored (the :not is
                 * dropped, like other unknown pseudo-classes). */
                std::string inner = v.substr(4, v.size() - 5);
                size_t b = inner.find_first_not_of(" \t");
                size_t e = inner.find_last_not_of(" \t");
                if (b != std::string::npos) {
                    inner = inner.substr(b, e - b + 1);
                    bool complex = false;
                    int depth = 0;
                    for (size_t k = 0; k < inner.size(); ++k) {
                        char ch = inner[k];
                        if (ch == '(') {
                            ++depth;
                        } else if (ch == ')') {
                            if (depth > 0) {
                                --depth;
                            }
                        } else if (depth == 0 &&
                                   (ch == ' ' || ch == '\t' || ch == '>' ||
                                    ch == '+' || ch == '~')) {
                            complex = true;
                            break;
                        }
                    }
                    if (!complex && !inner.empty()) {
                        /* a lone state pseudo-class negates with the same
                         * bubble rule as the positive form (checked in
                         * match(), where the state lives); anything else
                         * negates statically in part_match */
                        if (inner == ":hover") {
                            out.not_hover = true;
                        } else if (inner == ":active") {
                            out.not_active = true;
                        } else if (inner == ":focus" ||
                                   inner == ":focus-visible") {
                            out.not_focus = true;
                        } else {
                            out.has_not = true;
                            out.not_sel = inner;
                        }
                    }
                }
            } else if (v.compare(0, 4, "has(") == 0 &&
                       !v.empty() && v.back() == ')') {
                /* ":has(<relative>)" - parent/sibling conditional. The inner
                 * text is stored raw (leading > + ~ supported, combinators
                 * inside allowed) and evaluated against el's surroundings in
                 * part_match. Multiple :has() concatenate (each must match).
                 * Malformed/empty arguments are dropped like other unknown
                 * pseudo-classes. */
                std::string inner = v.substr(4, v.size() - 5);
                size_t b = inner.find_first_not_of(" \t");
                size_t e = inner.find_last_not_of(" \t");
                if (b != std::string::npos) {
                    inner = inner.substr(b, e - b + 1);
                    if (!inner.empty()) {
                        out.has_sel.push_back(inner);
                    }
                }
            }
            continue;
        }
        char kind = *p++;
        const char* s = p;
        while (p < end && *p != '#' && *p != '.' && *p != ':' &&
               *p != '[') {
            ++p;
        }
        std::string v(s, static_cast<size_t>(p - s));
        if (kind == '#') {
            out.id = v;
        } else if (kind == '.') {
            /* multiple classes ("a.b") all have to match; keep them
             * space-separated so el_has_class checks every one */
            if (out.cls.empty()) {
                out.cls = v;
            } else {
                out.cls += ' ';
                out.cls += v;
            }
        }
    }
    return !out.pseudo_el &&
           (!out.tag.empty() || !out.id.empty() || !out.cls.empty() ||
            out.hover || out.active || out.focus || out.disabled ||
            out.checked || out.indeterminate || out.enabled ||
            out.required || out.optional || out.empty || out.link ||
            out.focus_within || !out.is_sel.empty() ||
            out.last_child || out.first_child || out.nth || out.has_attr ||
            out.has_not || out.not_hover || out.not_active ||
            out.not_focus || !out.has_sel.empty());
}

/* fwd: full-selector match used by :has() inner evaluation */
extern "C" int whaleui_style_match(const char* selector, lxb_dom_element* el,
                                   const whaleui_style_state* st);

/* ":has(<relative>)" inner evaluation against el's surroundings.
 * Leading combinator picks the candidate set; the remainder is a full
 * selector (combinators inside allowed) matched with whaleui_style_match:
 *   "> X"  any child matches X          "+ X"  next sibling matches X
 *   "~ X"  any later sibling matches X  "X"    any descendant matches X
 * State inside X is approximated as unset (zero state: no hover/focus),
 * same conservatism as :is()'s state stripping. Nested :has inside the
 * remainder works via recursion (depth-bounded by selector nesting). */
static bool has_match_inner(lxb_dom_element* el, const std::string& inner)
{
    size_t b = inner.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return false;
    }
    std::string s = inner.substr(b);
    size_t e = s.find_last_not_of(" \t");
    s = s.substr(0, e + 1);
    if (s.empty()) {
        return false;
    }
    int scope = 0; /* 0 = descendant, 1 = child, 2 = adjacent, 3 = sibling */
    std::string rest = s;
    if (s[0] == '>') {
        scope = 1;
        rest = s.substr(1);
    } else if (s[0] == '+') {
        scope = 2;
        rest = s.substr(1);
    } else if (s[0] == '~') {
        scope = 3;
        rest = s.substr(1);
    }
    b = rest.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return false;
    }
    e = rest.find_last_not_of(" \t");
    rest = rest.substr(b, e - b + 1);
    if (rest.empty()) {
        return false;
    }
    whaleui_style_state zero;
    auto matches = [&](lxb_dom_element* cand) {
        return cand &&
               whaleui_style_match(rest.c_str(), cand, &zero) != 0;
    };
    if (scope == 1) {
        for (lxb_dom_node* c = el->node.first_child; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                matches(lxb_dom_interface_element(c))) {
                return true;
            }
        }
        return false;
    }
    if (scope == 2) {
        for (lxb_dom_node* c = el->node.next; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                return matches(lxb_dom_interface_element(c));
            }
        }
        return false;
    }
    if (scope == 3) {
        for (lxb_dom_node* c = el->node.next; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                matches(lxb_dom_interface_element(c))) {
                return true;
            }
        }
        return false;
    }
    /* descendant: iterative DFS (no recursion depth risk on deep pages) */
    std::vector<lxb_dom_node*> stack;
    for (lxb_dom_node* c = el->node.first_child; c; c = c->next) {
        stack.push_back(c);
    }
    while (!stack.empty()) {
        lxb_dom_node* c = stack.back();
        stack.pop_back();
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (matches(lxb_dom_interface_element(c))) {
                return true;
            }
        }
        for (lxb_dom_node* k = c->first_child; k; k = k->next) {
            stack.push_back(k);
        }
    }
    return false;
}

bool el_has_class(lxb_dom_element* el, const std::string& cls)
{
    size_t alen = 0;
    const lxb_char_t* a = lxb_dom_element_get_attribute(el, (const lxb_char_t*)"class", 5, &alen);
    if (!a) {
        return false;
    }
    /* cls may hold several classes ("panel collapsed"): EVERY one must be
     * present on the element */
    const char* cp = cls.c_str();
    while (*cp) {
        while (*cp == ' ' || *cp == '\t') {
            ++cp;
        }
        if (!*cp) {
            break;
        }
        const char* cs = cp;
        while (*cp && *cp != ' ' && *cp != '\t') {
            ++cp;
        }
        std::string want(cs, static_cast<size_t>(cp - cs));
        bool found = false;
        const lxb_char_t* c = a;
        const lxb_char_t* e = a + alen;
        while (c < e) {
            while (c < e && (*c == ' ' || *c == '\t')) {
                ++c;
            }
            const lxb_char_t* tok = c;
            while (c < e && *c != ' ' && *c != '\t') {
                ++c;
            }
            if (static_cast<size_t>(c - tok) == want.size() &&
                std::memcmp(tok, want.data(), want.size()) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool part_match(const SelPart& p, lxb_dom_element* el)
{
    if (!p.id.empty()) {
        size_t alen = 0;
        const lxb_char_t* id = lxb_dom_element_get_attribute(el, (const lxb_char_t*)"id", 2, &alen);
        if (!id || alen != p.id.size() || std::memcmp(id, p.id.data(), p.id.size()) != 0) {
            return false;
        }
    }
    if (!p.cls.empty() && !el_has_class(el, p.cls)) {
        return false;
    }
    if (!p.tag.empty()) {
        size_t nlen = 0;
        const lxb_char_t* name = lxb_dom_element_local_name(el, &nlen);
        if (!name || nlen != p.tag.size() || std::memcmp(name, p.tag.data(), p.tag.size()) != 0) {
            return false;
        }
    }
    if (p.disabled) {
        /* has_attribute, not get_attribute: a value-less boolean attribute
         * (disabled/checked/selected) stores no value in lexbor and
         * get_attribute returns NULL for it, so every [attr]/:disabled/
         * :checked test silently failed on exactly the attributes HTML
         * writes without a value. */
        if (!lxb_dom_element_has_attribute(el, (const lxb_char_t*)"disabled",
                                           8)) {
            return false;
        }
    }
    if (p.checked) {
        /* the checked/selected *attribute* (the DOM property a page sets
         * with JS; the paint path keeps its own state for clicks) */
        if (!lxb_dom_element_has_attribute(el, (const lxb_char_t*)"checked", 7) &&
            !lxb_dom_element_has_attribute(el, (const lxb_char_t*)"selected",
                                           8)) {
            return false;
        }
    }
    if (p.indeterminate) {
        /* :indeterminate is a DOM property with no attribute equivalent;
         * accept an "indeterminate" attribute so a page can express it */
        if (!lxb_dom_element_has_attribute(
                el, (const lxb_char_t*)"indeterminate", 13)) {
            return false;
        }
    }
    if (p.has_attr) {
        /* presence must go through has_attribute (a value-less attribute
         * has no value to return); only the valued forms need the value
         * itself */
        if (!lxb_dom_element_has_attribute(
                el, reinterpret_cast<const lxb_char_t*>(p.attr.c_str()),
                p.attr.size())) {
            return false; /* [name]: the attribute must exist */
        }
        if (p.attr_op != 0 && !p.attr_val.empty()) {
            size_t alen = 0;
            const lxb_char_t* a = lxb_dom_element_get_attribute(
                el, reinterpret_cast<const lxb_char_t*>(p.attr.c_str()),
                p.attr.size(), &alen);
            std::string av;
            if (a) {
                av.assign(reinterpret_cast<const char*>(a), alen);
            }
            const std::string& w = p.attr_val;
            bool ok = false;
            switch (p.attr_op) {
            case 1: /* [name="value"]: exact match */
                ok = av == w;
                break;
            case 2: /* [name^="v"]: prefix */
                ok = av.size() >= w.size() &&
                     av.compare(0, w.size(), w) == 0;
                break;
            case 3: /* [name$="v"]: suffix */
                ok = av.size() >= w.size() &&
                     av.compare(av.size() - w.size(), w.size(), w) == 0;
                break;
            case 4: /* [name*="v"]: substring */
                ok = av.find(w) != std::string::npos;
                break;
            case 5: { /* [name~="v"]: whitespace-separated word */
                size_t pos = 0;
                while (pos < av.size()) {
                    while (pos < av.size() &&
                           (av[pos] == ' ' || av[pos] == '\t')) {
                        ++pos;
                    }
                    size_t s = pos;
                    while (pos < av.size() && av[pos] != ' ' &&
                           av[pos] != '\t') {
                        ++pos;
                    }
                    if (pos > s && av.compare(s, pos - s, w) == 0) {
                        ok = true;
                        break;
                    }
                }
                break;
            }
            case 6: /* [name|="v"]: exact or hyphen-prefixed */
                ok = av == w ||
                     (av.size() > w.size() && av.compare(0, w.size(), w) == 0 &&
                      av[w.size()] == '-');
                break;
            default:
                break;
            }
            if (!ok) {
                return false;
            }
        }
    }
    if (p.enabled) {
        /* form controls that are NOT disabled (:disabled's complement) */
        if (lxb_dom_element_has_attribute(el, (const lxb_char_t*)"disabled",
                                           8)) {
            return false;
        }
    }
    if (p.required) {
        if (!lxb_dom_element_has_attribute(el, (const lxb_char_t*)"required",
                                           8)) {
            return false;
        }
    }
    if (p.optional) {
        if (lxb_dom_element_has_attribute(el, (const lxb_char_t*)"required",
                                          8)) {
            return false;
        }
    }
    if (p.empty) {
        /* no element children and no non-blank text */
        for (lxb_dom_node* c = el->node.first_child; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                return false;
            }
            if (c->type == LXB_DOM_NODE_TYPE_TEXT ||
                c->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
                const lexbor_str_t* s =
                    &lxb_dom_interface_text(c)->char_data.data;
                if (s->data) {
                    for (size_t k = 0; k < s->length; ++k) {
                        unsigned char ch = s->data[k];
                        if (ch != ' ' && ch != '\t' && ch != '\n' &&
                            ch != '\r') {
                            return false;
                        }
                    }
                }
            }
        }
    }
    if (p.link) {
        /* :link/:visited - any <a href> (no history tracking, so :visited
         * matches the same set; splitting them would only matter for
         * privacy-separated visited styling, which we do not do) */
        size_t nlen = 0;
        const lxb_char_t* name = lxb_dom_element_local_name(el, &nlen);
        if (!name || nlen != 1 || name[0] != 'a') {
            return false;
        }
        size_t hlen = 0;
        const lxb_char_t* href = lxb_dom_element_get_attribute(
            el, (const lxb_char_t*)"href", 4, &hlen);
        if (!href || hlen == 0) {
            return false;
        }
    }
    if (!p.is_sel.empty()) {
        /* ":is(a, b)" / ":where(a, b)" - any inner simple selector
         * matching the same element wins (single-level, like :not) */
        bool any = false;
        size_t start = 0;
        int depth = 0;
        for (size_t k = 0; k <= p.is_sel.size(); ++k) {
            char ch = k < p.is_sel.size() ? p.is_sel[k] : ',';
            if (ch == '(') {
                ++depth;
            } else if (ch == ')') {
                if (depth > 0) {
                    --depth;
                }
            }
            if ((ch == ',' && depth == 0) || k == p.is_sel.size()) {
                std::string one = p.is_sel.substr(start, k - start);
                size_t b = one.find_first_not_of(" \t");
                size_t e = one.find_last_not_of(" \t");
                if (b != std::string::npos) {
                    one = one.substr(b, e - b + 1);
                    SelPart inner;
                    if (parse_simple(one.c_str(), one.size(), inner)) {
                        inner.hover = inner.active = inner.focus = false;
                        inner.not_hover = inner.not_active =
                            inner.not_focus = false;
                        if (part_match(inner, el)) {
                            any = true;
                            break;
                        }
                    }
                }
                start = k + 1;
            }
        }
        if (!any) {
            return false;
        }
    }
    if (p.last_child) {
        /* no following ELEMENT siblings */
        for (lxb_dom_node* s = el->node.next; s; s = s->next) {
            if (s->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                return false;
            }
        }
    }
    if (p.first_child || p.nth) {
        /* 1-based position among ELEMENT siblings (nth-child counts only
         * elements, unlike :nth-of-type which filters by tag) */
        int pos = 0;
        for (lxb_dom_node* s = el->node.prev; s; s = s->prev) {
            if (s->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                ++pos;
            }
        }
        ++pos;
        if (p.first_child && pos != 1) {
            return false;
        }
        if (p.nth) {
            if (p.nth_a == 0) {
                if (pos != p.nth_b) {
                    return false;
                }
            } else {
                int d = pos - p.nth_b;
                if (d < 0 || d % p.nth_a != 0) {
                    return false;
                }
            }
        }
    }
    if (p.has_not) {
        /* ":not(<simple>)" - the inner selector matches the SAME element
         * (no state: hover/active/focus negations live in match(), where
         * the interaction state is visible). A nested :not inside :not is
         * dropped at parse time (not_sel never carries has_not). */
        SelPart inner;
        if (parse_simple(p.not_sel.c_str(), p.not_sel.size(), inner)) {
            /* strip any state flags the inner may carry (a lone state
             * inner is routed to not_hover/active/focus at parse, but a
             * compound like ".a:hover" still parses hover=true - the
             * static half decides here, the state half is approximated
             * as "inner matches" so :not() stays conservative) */
            inner.hover = inner.active = inner.focus = false;
            inner.not_hover = inner.not_active = inner.not_focus = false;
            if (part_match(inner, el)) {
                return false;
            }
        }
    }
    if (!p.has_sel.empty()) {
        /* ":has(<relative>)" - each inner must match against el's
         * surroundings (children/descendants/siblings). Unlike :not this
         * looks AWAY from the element (parent-conditional styling). */
        for (size_t i = 0; i < p.has_sel.size(); ++i) {
            if (!has_match_inner(el, p.has_sel[i])) {
                return false;
            }
        }
    }
    return true;
}

/* --- specificity --- */

struct Specificity { int a, b, c; };

inline bool better(const Specificity& a, const Specificity& b)
{
    if (a.a != b.a) {
        return a.a > b.a;
    }
    if (a.b != b.b) {
        return a.b > b.b;
    }
    return a.c > b.c;
}

Specificity sel_specificity(const char* sel)
{
    Specificity sp = {0, 0, 0};
    for (const char* p = sel; *p; ++p) {
        if (*p == '#') {
            ++sp.a;
        } else if (*p == '.') {
            ++sp.b;
        } else if (*p == ':') {
            /* :where() contributes nothing (not even the colon); :is(),
             * :not() and :has() contribute their most-specific argument
             * (ditto). All matched case-insensitively like the rest of
             * the parser. */
            bool is_where = (p[1] == 'w' || p[1] == 'W') &&
                            (p[2] == 'h' || p[2] == 'H') &&
                            (p[3] == 'e' || p[3] == 'E') &&
                            (p[4] == 'r' || p[4] == 'R') &&
                            (p[5] == 'e' || p[5] == 'E') && p[6] == '(';
            bool is_is = !is_where && (p[1] == 'i' || p[1] == 'I') &&
                         (p[2] == 's' || p[2] == 'S') && p[3] == '(';
            bool is_not = !is_where && !is_is &&
                          (p[1] == 'n' || p[1] == 'N') &&
                          (p[2] == 'o' || p[2] == 'O') &&
                          (p[3] == 't' || p[3] == 'T') && p[4] == '(';
            bool is_has = !is_where && !is_is && !is_not &&
                          (p[1] == 'h' || p[1] == 'H') &&
                          (p[2] == 'a' || p[2] == 'A') &&
                          (p[3] == 's' || p[3] == 'S') && p[4] == '(';
            if (is_where || is_is || is_not || is_has) {
                const char* open = std::strchr(p, '(');
                if (!open) {
                    ++sp.b;
                    continue;
                }
                int depth = 1;
                const char* q = open + 1;
                while (*q && depth > 0) {
                    if (*q == '(') {
                        ++depth;
                    } else if (*q == ')') {
                        --depth;
                    }
                    ++q;
                }
                /* [open+1, q-1) is the argument list */
                if (is_is || is_not || is_has) {
                    /* max over the comma-separated arguments (a leading
                     * > + ~ combinator in :has() carries no weight; only
                     * the inner #/. counts leak through, which is exactly
                     * what the raw scan below would see) */
                    /* max over the comma-separated arguments */
                    Specificity best = {0, 0, 0};
                    const char* a0 = open + 1;
                    const char* end = q - 1;
                    int d2 = 0;
                    for (const char* k = a0;; ++k) {
                        char c = k >= end ? ',' : *k;
                        if (c == '(') {
                            ++d2;
                        } else if (c == ')') {
                            if (d2 > 0) {
                                --d2;
                            }
                        }
                        if ((c == ',' && d2 == 0) || k >= end) {
                            std::string arg(a0, static_cast<size_t>(
                                                    (k >= end ? end : k) -
                                                    a0));
                            size_t b = arg.find_first_not_of(" \t");
                            if (b != std::string::npos) {
                                size_t e = arg.find_last_not_of(" \t");
                                arg = arg.substr(b, e - b + 1);
                                Specificity sa =
                                    sel_specificity(arg.c_str());
                                if (better(sa, best)) {
                                    best = sa;
                                }
                            }
                            a0 = k + 1;
                            if (k >= end) {
                                break;
                            }
                        }
                        if (k >= end) {
                            break;
                        }
                    }
                    sp.a += best.a;
                    sp.b += best.b;
                    sp.c += best.c;
                }
                /* :where: nothing. Either way the whole (...) is skipped
                 * (inner #/. chars must not leak into the raw count). */
                p = q - 1;
                continue;
            }
            ++sp.b; /* pseudo-class counts as class */
        }
    }
    return sp;
}

/* --- value helpers --- */

void resolve_var(std::string& val, const std::map<std::string, std::string>& vars)
{
    for (;;) {
        size_t start = val.find("var(");
        if (start == std::string::npos) {
            return;
        }
        size_t end = val.find(')', start);
        if (end == std::string::npos) {
            return;
        }
        std::string inner = val.substr(start + 4, end - start - 4);
        /* trim */
        size_t b = inner.find_first_not_of(" \t");
        if (b == std::string::npos) {
            return;
        }
        size_t e = inner.find_last_not_of(" \t");
        inner = inner.substr(b, e - b + 1);
        /* var(--x, fallback) */
        size_t comma = inner.find(',');
        std::string name = comma == std::string::npos ? inner : inner.substr(0, comma);
        size_t nb = name.find_first_not_of(" \t");
        size_t ne = name.find_last_not_of(" \t");
        name = name.substr(nb, ne - nb + 1);
        std::string repl;
        auto it = vars.find(name);
        if (it != vars.end()) {
            repl = it->second;
        } else if (comma != std::string::npos) {
            repl = inner.substr(comma + 1);
            size_t rb = repl.find_first_not_of(" \t");
            size_t re = repl.find_last_not_of(" \t");
            repl = repl.substr(rb, re - rb + 1);
        }
        val.replace(start, end - start + 1, repl);
        if (repl.empty()) {
            return; /* avoid infinite loop on empty var */
        }
    }
}

/* --- shorthand expansion (font, border-top/bottom/right/left, border) --- */

std::vector<std::string> split_space(const std::string& s)
{
    std::vector<std::string> out;
    const char* p = s.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char* q = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
        out.push_back(std::string(q, static_cast<size_t>(p - q)));
    }
    return out;
}

/* find a color token inside a value ("1px solid #fff"): returns the token */
std::string value_color(const std::string& v)
{
    unsigned int c = 0;
    if (whaleui_render_parse_color(v.c_str(), &c) == 0) {
        std::string t = v;
        size_t b = t.find_first_not_of(" \t");
        size_t e = t.find_last_not_of(" \t");
        return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
    }
    std::vector<std::string> toks = split_space(v);
    for (size_t i = 0; i < toks.size(); ++i) {
        if (whaleui_render_parse_color(toks[i].c_str(), &c) == 0) {
            return toks[i];
        }
    }
    return std::string();
}

/* "font: <weight|style> <size>[/<line-height>] <family...>" shorthand */
void expand_font(WhaleUIComputedStyle& s)
{
    WhaleUIComputedStyle::const_iterator it = s.find("font");
    if (it == s.end() || it->second.empty()) {
        return;
    }
    /* No "longhand wins" early-out: a `font` shorthand with an explicit
     * size must apply it even when a LOWER-specificity rule set font-size
     * (theme kbd{font-size:.9em} vs .btn kbd{font:600 10px mono} - the
     * shorthand's specificity is higher, so its size must win). */
    std::vector<std::string> toks = split_space(it->second);
    std::string weight, style, size, lh, family;
    size_t size_idx = std::string::npos;
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& t = toks[i];
        if (t == "italic" || t == "oblique") {
            style = t;
            continue;
        }
        if (t == "bold") {
            weight = "bold";
            continue;
        }
        if (t == "normal" || t == "bolder" || t == "lighter") {
            if (weight.empty()) {
                weight = t;
            }
            continue;
        }
        /* size: has a unit/function or a slash (line-height) */
        bool has_unit = t.find("px") != std::string::npos ||
                        t.find("em") != std::string::npos ||
                        t.find("%") != std::string::npos ||
                        t.find("clamp(") == 0 ||
                        t.find("min(") == 0 || t.find("max(") == 0 ||
                        t.find("rem") != std::string::npos ||
                        t.find("vh") != std::string::npos ||
                        t.find("vw") != std::string::npos;
        if (has_unit) {
            size_idx = i;
            break;
        }
        /* bare number before a sized token = font-weight */
        bool numeric = !t.empty() &&
                       (t[0] >= '0' && t[0] <= '9' || t[0] == '.');
        if (numeric) {
            weight = t;
            continue;
        }
        /* anything else so far is a weight keyword or family start */
        if (weight.empty() &&
            (t == "100" || t == "200" || t == "300" || t == "400" ||
             t == "500" || t == "600" || t == "700" || t == "800" ||
             t == "900")) {
            weight = t;
            continue;
        }
    }
    if (size_idx == std::string::npos) {
        return; /* malformed shorthand: ignore */
    }
    size = toks[size_idx];
    size_t slash = size.find('/');
    if (slash != std::string::npos) {
        lh = size.substr(slash + 1);
        size = size.substr(0, slash);
    }
    for (size_t j = size_idx + 1; j < toks.size(); ++j) {
        if (!family.empty()) {
            family += ' ';
        }
        family += toks[j];
    }
    s["font-size"] = size;
    if (!lh.empty()) {
        s["line-height"] = lh;
    }
    if (!weight.empty() && s.find("font-weight") == s.end()) {
        s["font-weight"] = weight;
    }
    if (!style.empty() && s.find("font-style") == s.end()) {
        s["font-style"] = style;
    }
    if (!family.empty() && s.find("font-family") == s.end()) {
        s["font-family"] = family;
    }
}

/* "border-top: 1px solid #fff" -> border-top-width + border-color */
void expand_border_side(WhaleUIComputedStyle& s, const char* side,
                        const char* wprop)
{
    WhaleUIComputedStyle::const_iterator it = s.find(side);
    if (it == s.end() || it->second.empty()) {
        return;
    }
    const std::string& v = it->second;
    if (s.find(wprop) == s.end()) {
        std::vector<std::string> toks = split_space(v);
        for (size_t i = 0; i < toks.size(); ++i) {
            const std::string& t = toks[i];
            if (t == "solid" || t == "dashed" || t == "dotted" ||
                t == "none" || t == "hidden" || t == "double" ||
                t == "groove" || t == "ridge" || t == "inset" ||
                t == "outset") {
                continue;
            }
            char* end = nullptr;
            std::strtof(t.c_str(), &end);
            if (end != t.c_str() && *end != '\0' && t[0] != '#') {
                s[wprop] = t; /* a length token */
                break;
            }
        }
    }
    if (s.find("border-color") == s.end()) {
        std::string c = value_color(v);
        if (!c.empty()) {
            s["border-color"] = c;
        }
    }
}

void expand_shorthands(WhaleUIComputedStyle& s)
{
    expand_font(s);
    /* background shorthand carries the color: a page's
     * "background:#10141d" must win over the UA default's
     * "background-color:var(--bg)" - without this the UA color stayed
     * and every page that set a plain background kept the theme's
     * (背景/文字颜色被默认样式顶掉). Gradients/images survive: their
     * value does not parse as a color, so paint falls back to the
     * background key itself. */
    WhaleUIComputedStyle::iterator bgs = s.find("background");
    if (bgs != s.end() && bgs->second != "none") {
        s["background-color"] = bgs->second;
    }
    /* border-block: 1px solid red -> border-top + border-bottom (before the
     * side expansion so the width/color shorthands apply to both) */
    WhaleUIComputedStyle::const_iterator bb = s.find("border-block");
    if (bb != s.end()) {
        if (s.find("border-top") == s.end()) {
            s["border-top"] = bb->second;
        }
        if (s.find("border-bottom") == s.end()) {
            s["border-bottom"] = bb->second;
        }
    }
    /* border: 1px solid red -> all four sides (same reasoning as above) */
    WhaleUIComputedStyle::const_iterator bd = s.find("border");
    if (bd != s.end()) {
        if (s.find("border-top") == s.end()) {
            s["border-top"] = bd->second;
        }
        if (s.find("border-bottom") == s.end()) {
            s["border-bottom"] = bd->second;
        }
        if (s.find("border-left") == s.end()) {
            s["border-left"] = bd->second;
        }
        if (s.find("border-right") == s.end()) {
            s["border-right"] = bd->second;
        }
    }
    expand_border_side(s, "border-top", "border-top-width");
    expand_border_side(s, "border-bottom", "border-bottom-width");
    expand_border_side(s, "border-left", "border-left-width");
    expand_border_side(s, "border-right", "border-right-width");
    /* inset: TRBL shorthand -> top/right/bottom/left ("inset: 0" covers
     * fixed/absolute elements pinning to all four edges) */
    WhaleUIComputedStyle::const_iterator ins = s.find("inset");
    if (ins != s.end()) {
        std::vector<std::string> toks = split_space(ins->second);
        static const char* kInset[] = {"top", "right", "bottom", "left"};
        for (int i = 0; i < 4; ++i) {
            if (s.find(kInset[i]) != s.end()) {
                continue; /* longhand wins */
            }
            size_t n = toks.size();
            if (n == 1) {
                s[kInset[i]] = toks[0];
            } else if (n == 2) {
                s[kInset[i]] = toks[i % 2];
            } else if (n == 3) {
                s[kInset[i]] = toks[i == 3 ? 1 : i];
            } else if (n >= 4) {
                s[kInset[i]] = toks[i];
            }
        }
    }
    WhaleUIComputedStyle::const_iterator b = s.find("border");
    if (b != s.end() && s.find("border-color") == s.end()) {
        std::string c = value_color(b->second);
        if (!c.empty()) {
            s["border-color"] = c;
        }
    }
}

} // namespace

/* cached selector chain (parse once per selector, reused across the
 * per-element cascade scans - the chain split + paren scan was a hot cost
 * on rule-heavy pages) */
struct SelChain { std::string sel; int comb; };
static std::map<std::string, std::vector<SelChain>> g_chain_cache;

extern "C" int whaleui_style_match(const char* selector, lxb_dom_element* el,
                                   const whaleui_style_state* st)
{
    if (!selector || !el) {
        return 0;
    }
    /* split on '>'/'+' and whitespace into a chain of simple selectors.
     * comb: 0 = descendant, 1 = child (prev part's relation), 2 = adjacent
     * sibling (this part must match the previous sibling of the target). */
    std::vector<SelChain>& chain = g_chain_cache[selector];
    if (chain.empty() && *selector) {
        int comb = 0;
        const char* p = selector;
        while (*p && chain.size() < 32) {
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            if (*p == '>') {
                comb = 1;
                ++p;
                continue;
            }
            if (*p == '+') {
                comb = 2;
                ++p;
                continue;
            }
            if (!*p) {
                break;
            }
            const char* s = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '>' && *p != '+') {
                if (*p == '(') {
                    int depth = 1;
                    ++p;
                    while (*p && depth) {
                        if (*p == '(') {
                            ++depth;
                        } else if (*p == ')') {
                            --depth;
                        }
                        ++p;
                    }
                } else {
                    ++p;
                }
            }
            std::string raw(s, static_cast<size_t>(p - s));
            if (!raw.empty()) {
                SelChain sc;
                sc.sel = raw;
                sc.comb = comb;
                chain.push_back(sc);
                comb = 0;
            }
        }
    }
    const int n = static_cast<int>(chain.size());
    if (n == 0) {
        return 0;
    }
    /* match from the last part backwards. `cur` is the node the previous
     * (right-hand) part matched; each combinator picks the next candidate
     * from it: descendant = an ancestor, child = the direct parent,
     * adjacent = the previous ELEMENT sibling. */
    lxb_dom_node* cur = &el->node;
    for (int i = n - 1; i >= 0; --i) {
        SelPart part;
        if (!parse_simple(chain[i].sel.c_str(), chain[i].sel.size(), part)) {
            return 0;
        }
        if (i == n - 1) {
            /* must match the element itself - no ancestor walk */
            if (cur->type != LXB_DOM_NODE_TYPE_ELEMENT ||
                !part_match(part, lxb_dom_interface_element(cur))) {
                return 0;
            }
        } else {
            const int c = chain[i + 1].comb;
            if (c == 2) {
                /* adjacent: the previous ELEMENT sibling of cur */
                lxb_dom_node* s = cur->prev;
                while (s && s->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                    s = s->prev;
                }
                if (!s || !part_match(part, lxb_dom_interface_element(s))) {
                    return 0;
                }
                cur = s;
            } else if (c == 1) {
                /* child: the direct parent */
                lxb_dom_node* a = cur->parent;
                if (!a || a->type != LXB_DOM_NODE_TYPE_ELEMENT ||
                    !part_match(part, lxb_dom_interface_element(a))) {
                    return 0;
                }
                cur = a;
            } else {
                /* descendant: any ancestor */
                bool matched = false;
                for (lxb_dom_node* a = cur->parent; a; a = a->parent) {
                    if (a->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                        part_match(part, lxb_dom_interface_element(a))) {
                        cur = a;
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    return 0;
                }
            }
            /* fall through: the state pseudo-classes of THIS compound must
             * be tested against `cur` (the ancestor), not skipped. Skipping
             * them made every `A:hover B` selector match unconditionally -
             * `.card:hover .acts{display:block}` applied at page load, and
             * the reported "overlay styles are simply wrong" symptom. */
        }
        /* pseudo-classes apply to the compound they were written on (the
         * target element for the last part, the matched ancestor for a
         * preceding one). :hover and :active bubble per CSS: they also
         * match when the interaction target is a descendant of this
         * element (a button containing a <span> stays :hover while the
         * mouse is over the span) */
        lxb_dom_element* cur_el = lxb_dom_interface_element(cur);
        if (part.hover) {
            bool ok = false;
            for (lxb_dom_node* h = (st && st->hover) ? &st->hover->node : nullptr;
                 h; h = h->parent) {
                if (h->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                    lxb_dom_interface_element(h) == cur_el) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                return 0;
            }
        }
        if (part.active) {
            bool ok = false;
            for (lxb_dom_node* h = (st && st->pressed) ? &st->pressed->node : nullptr;
                 h; h = h->parent) {
                if (h->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                    lxb_dom_interface_element(h) == cur_el) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                return 0;
            }
        }
        if (part.focus && (!st || !st->focus || cur_el != st->focus)) {
            return 0;
        }
        if (part.focus_within) {
            /* :focus-within: the element itself or any descendant holds
             * the focus (same target test as :focus, walked downward) */
            bool ok = (st && st->focus && cur_el == st->focus);
            if (!ok && st && st->focus) {
                for (lxb_dom_node* h = st->focus->node.parent; h;
                     h = h->parent) {
                    if (h->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                        lxb_dom_interface_element(h) == cur_el) {
                        ok = true;
                        break;
                    }
                }
            }
            if (!ok) {
                return 0;
            }
        }
        /* ":not(:hover/:active/:focus)" - the same state test, inverted
         * (with the same bubble rule as the positive form). A part like
         * "button:not(:hover)" matches a button while the pointer is
         * anywhere else. */
        if (part.not_hover) {
            for (lxb_dom_node* h = (st && st->hover) ? &st->hover->node : nullptr;
                 h; h = h->parent) {
                if (h->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                    lxb_dom_interface_element(h) == cur_el) {
                    return 0;
                }
            }
        }
        if (part.not_active) {
            for (lxb_dom_node* h = (st && st->pressed) ? &st->pressed->node : nullptr;
                 h; h = h->parent) {
                if (h->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                    lxb_dom_interface_element(h) == cur_el) {
                    return 0;
                }
            }
        }
        if (part.not_focus && (st && st->focus && cur_el == st->focus)) {
            return 0;
        }
    }
    return 1;
}

extern "C" int whaleui_style_match_pseudo(const char* selector,
                                          lxb_dom_element* el,
                                          const whaleui_style_state* st,
                                          int* pseudo)
{
    if (!selector || !el || !pseudo) {
        return 0;
    }
    *pseudo = 0;
    size_t len = std::strlen(selector);
    const char* pe = nullptr;
    int pv = 0;
    if (len >= 7 && std::strcmp(selector + len - 7, "::after") == 0) {
        pe = selector + len - 7;
        pv = 2;
    } else if (len >= 8 && std::strcmp(selector + len - 8, "::before") == 0) {
        pe = selector + len - 8;
        pv = 1;
    } else if (len >= 8 && std::strcmp(selector + len - 8, "::marker") == 0) {
        pe = selector + len - 8;
        pv = 3;
    } else if (len >= 12 &&
               std::strcmp(selector + len - 12, "::first-line") == 0) {
        pe = selector + len - 12;
        pv = 4;
    } else if (len >= 14 &&
               std::strcmp(selector + len - 14, "::first-letter") == 0) {
        pe = selector + len - 14;
        pv = 5;
    }
    if (!pe) {
        return whaleui_style_match(selector, el, st);
    }
    if (pe == selector) {
        return 0; /* "::after" alone */
    }
    std::string base(selector, static_cast<size_t>(pe - selector));
    if (!whaleui_style_match(base.c_str(), el, st)) {
        return 0;
    }
    *pseudo = pv;
    return 1;
}

extern "C" int whaleui_style_media_ok(const char* media, int theme,
                                      int viewport_w, int reduced_motion,
                                      int viewport_h)
{
    if (!media || !*media) {
        return 1;
    }
    std::string m(media);
    /* media TYPE (the leading token, e.g. "print", "screen and (...)"). This
     * engine renders to a screen, so only `all` and `screen` apply; every
     * other type must fail or `@media print` / `media="print"` rules render on
     * screen (they did - the type was simply never looked at, so any unknown
     * condition counted as a match). Parenthesized feature groups are stripped
     * first: a bare word inside them (`(pointer: fine)`) is not a type. */
    {
        std::string bare;
        int depth = 0;
        for (size_t i = 0; i < m.size(); ++i) {
            char c = m[i];
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (depth > 0) {
                    --depth;
                }
            } else if (depth == 0) {
                bare += c;
            }
        }
        size_t i = 0;
        while (i < bare.size()) {
            if (!std::isalnum(static_cast<unsigned char>(bare[i]))) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < bare.size() &&
                   std::isalnum(static_cast<unsigned char>(bare[j]))) {
                ++j;
            }
            std::string tok = bare.substr(i, j - i);
            if (tok == "print" || tok == "speech" || tok == "tty" ||
                tok == "tv" || tok == "projection" || tok == "handheld" ||
                tok == "braille" || tok == "embossed" || tok == "aural") {
                return 0;
            }
            i = j;
        }
    }
    size_t pos = 0;
    while ((pos = m.find("prefers-reduced-motion", pos)) != std::string::npos) {
        size_t colon = m.find(':', pos);
        if (colon != std::string::npos) {
            std::string v = m.substr(colon + 1);
            size_t b = v.find_first_not_of(" \t");
            bool reduce = b != std::string::npos &&
                          v.compare(b, 6, "reduce") == 0;
            if (reduce && !reduced_motion) {
                return 0;
            }
            if (!reduce && reduced_motion) {
                return 0; /* "no-preference" does not match reduced mode */
            }
        }
        pos += 23;
    }
    pos = 0;
    while ((pos = m.find("prefers-color-scheme", pos)) != std::string::npos) {
        size_t colon = m.find(':', pos);
        if (colon != std::string::npos) {
            std::string v = m.substr(colon + 1);
            size_t b = v.find_first_not_of(" \t");
            bool dark = b != std::string::npos && v.compare(b, 4, "dark") == 0;
            bool light = b != std::string::npos && v.compare(b, 5, "light") == 0;
            int want = dark ? 2 : (light ? 1 : 0); /* WHALEUI_THEME_DARK / LIGHT */
            if (want && want != theme) {
                return 0;
            }
        }
        pos += 19;
    }
    pos = 0;
    while ((pos = m.find("min-width", pos)) != std::string::npos) {
        size_t colon = m.find(':', pos);
        size_t close = m.find(')', pos);
        if (colon != std::string::npos && close != std::string::npos) {
            float v = static_cast<float>(std::atof(m.substr(colon + 1, close - colon - 1).c_str()));
            if (viewport_w < static_cast<int>(v)) {
                return 0;
            }
        }
        pos += 9;
    }
    pos = 0;
    while ((pos = m.find("max-width", pos)) != std::string::npos) {
        size_t colon = m.find(':', pos);
        size_t close = m.find(')', pos);
        if (colon != std::string::npos && close != std::string::npos) {
            float v = static_cast<float>(std::atof(m.substr(colon + 1, close - colon - 1).c_str()));
            if (viewport_w > static_cast<int>(v)) {
                return 0;
            }
        }
        pos += 9;
    }
    /* ---- extended conditions (viewport_h < 0 = unknown: height-gated
     * ones fail closed, device-constant ones evaluate) ---- */
    pos = 0;
    while ((pos = m.find("min-height", pos)) != std::string::npos) {
        size_t colon = m.find(':', pos);
        size_t close = m.find(')', pos);
        if (colon != std::string::npos && close != std::string::npos) {
            float v = static_cast<float>(std::atof(m.substr(colon + 1, close - colon - 1).c_str()));
            if (viewport_h < 0 || viewport_h < static_cast<int>(v)) {
                return 0;
            }
        }
        pos += 10;
    }
    pos = 0;
    while ((pos = m.find("max-height", pos)) != std::string::npos) {
        size_t colon = m.find(':', pos);
        size_t close = m.find(')', pos);
        if (colon != std::string::npos && close != std::string::npos) {
            float v = static_cast<float>(std::atof(m.substr(colon + 1, close - colon - 1).c_str()));
            if (viewport_h < 0 || viewport_h > static_cast<int>(v)) {
                return 0;
            }
        }
        pos += 10;
    }
    pos = 0;
    while ((pos = m.find("orientation", pos)) != std::string::npos) {
        size_t colon = m.find(':', pos);
        if (colon != std::string::npos) {
            std::string v = m.substr(colon + 1);
            size_t b = v.find_first_not_of(" \t");
            bool portrait = b != std::string::npos &&
                            v.compare(b, 8, "portrait") == 0;
            bool landscape = b != std::string::npos &&
                             v.compare(b, 9, "landscape") == 0;
            if (viewport_h < 0) {
                return 0;
            }
            bool is_portrait = viewport_h >= viewport_w;
            if ((portrait && !is_portrait) ||
                (landscape && is_portrait)) {
                return 0;
            }
        }
        pos += 11;
    }
    /* aspect-ratio: "W/H" (also min-/max-). Compares viewport_w/h. */
    {
        const char* keys[3] = {"min-aspect-ratio", "max-aspect-ratio",
                               "aspect-ratio"};
        for (int ki = 0; ki < 3; ++ki) {
            size_t klen = std::strlen(keys[ki]);
            pos = 0;
            while ((pos = m.find(keys[ki], pos)) != std::string::npos) {
                /* the bare scan must not eat the min-/max- forms */
                if (ki == 2 &&
                    ((pos >= 4 && m.compare(pos - 4, 4, "min-") == 0) ||
                     (pos >= 4 && m.compare(pos - 4, 4, "max-") == 0))) {
                    pos += klen;
                    continue;
                }
                size_t colon = m.find(':', pos);
                size_t close = m.find(')', pos);
                if (colon != std::string::npos &&
                    close != std::string::npos) {
                    std::string v = m.substr(colon + 1, close - colon - 1);
                    size_t slash = v.find('/');
                    if (slash != std::string::npos && viewport_h > 0) {
                        float aw =
                            static_cast<float>(std::atof(v.c_str()));
                        float ah = static_cast<float>(
                            std::atof(v.c_str() + slash + 1));
                        if (ah > 0) {
                            float have = viewport_w /
                                         static_cast<float>(viewport_h);
                            float want = aw / ah;
                            if ((ki == 0 && have < want - 0.01f) ||
                                (ki == 1 && have > want + 0.01f) ||
                                (ki == 2 && std::fabs(have - want) > 0.01f)) {
                                return 0;
                            }
                        }
                    } else if (viewport_h <= 0) {
                        return 0;
                    }
                }
                pos += klen;
            }
        }
    }
    /* hover/pointer: this engine is a desktop mouse renderer - hover is
     * always available, the primary pointer is always fine. */
    pos = 0;
    while ((pos = m.find("hover", pos)) != std::string::npos) {
        /* careful: "hover" is also inside... no other keyword contains it */
        size_t colon = m.find(':', pos);
        if (colon != std::string::npos) {
            std::string v = m.substr(colon + 1);
            size_t b = v.find_first_not_of(" \t");
            /* "(hover: hover)" matches; "(hover: none)" does not */
            bool want_hover = b != std::string::npos &&
                              v.compare(b, 5, "hover") == 0;
            if (!want_hover) {
                return 0;
            }
        }
        pos += 5;
    }
    pos = 0;
    while ((pos = m.find("pointer", pos)) != std::string::npos) {
        size_t colon = m.find(':', pos);
        if (colon != std::string::npos) {
            std::string v = m.substr(colon + 1);
            size_t b = v.find_first_not_of(" \t");
            /* "(pointer: fine)" matches; coarse/none do not */
            bool fine = b != std::string::npos &&
                        v.compare(b, 4, "fine") == 0;
            if (!fine) {
                return 0;
            }
        }
        pos += 7;
    }
    /* resolution: layout px are CSS px (96dpi reference). min- fails only
     * above 96, max- fails only below 96. dppx/x units convert. */
    pos = 0;
    while ((pos = m.find("resolution", pos)) != std::string::npos) {
        bool is_min = pos >= 4 && m.compare(pos - 4, 4, "min-") == 0;
        bool is_max = pos >= 4 && m.compare(pos - 4, 4, "max-") == 0;
        size_t colon = m.find(':', pos);
        size_t close = m.find(')', pos);
        if ((is_min || is_max) && colon != std::string::npos &&
            close != std::string::npos) {
            std::string v = m.substr(colon + 1, close - colon - 1);
            float dpi = static_cast<float>(std::atof(v.c_str()));
            if (v.find("dppx") != std::string::npos) {
                dpi *= 96.0f;
            } else if (v.find("dpcm") != std::string::npos) {
                dpi *= 2.54f;
            }
            if ((is_min && 96.0f < dpi) || (is_max && 96.0f > dpi)) {
                return 0;
            }
        }
        pos += 10;
    }
    return 1;
}

extern "C" void whaleui_style_collect_vars(lxb_dom_element* root,
                                           std::map<std::string, std::string>& vars)
{
    /* scan :root (html) and the whole tree for --* declarations; deepest
     * element wins for matching names. Simple pass: html first, then any
     * element's inline style. */
    if (!root) {
        return;
    }
    lxb_dom_element* html = root;
    /* html element custom props via inline style only (rule matching for
     * --* lives in the cascade, see compute) */
    size_t alen = 0;
    const lxb_char_t* st = lxb_dom_element_get_attribute(html, (const lxb_char_t*)"style", 5, &alen);
    if (st) {
        std::string style(reinterpret_cast<const char*>(st), alen);
        size_t pos = 0;
        while ((pos = style.find("--", pos)) != std::string::npos) {
            size_t colon = style.find(':', pos);
            size_t semi = style.find(';', pos);
            if (colon != std::string::npos && (semi == std::string::npos || colon < semi)) {
                std::string name = style.substr(pos, colon - pos);
                size_t nb = name.find_first_not_of(" \t");
                size_t ne = name.find_last_not_of(" \t");
                name = name.substr(nb, ne - nb + 1);
                size_t end = semi == std::string::npos ? style.size() : semi;
                std::string val = style.substr(colon + 1, end - colon - 1);
                size_t vb = val.find_first_not_of(" \t");
                size_t ve = val.find_last_not_of(" \t\r\n");
                val = val.substr(vb, ve - vb + 1);
                vars[name] = val;
            }
            pos += 2;
        }
    }
}

namespace {

/* tag index over a rule set, cached across elements AND frames: the rules
 * array is stable per render context, and rebuilding the index for every
 * element on every relayout (a width animation rebuilds its subtree each
 * frame) was the dominant cost of animated frames. Invalidated when the
 * rules pointer/count changes. Mutex-guarded for the async first-layout
 * worker, which can technically run beside nothing (first frame has no
 * interaction) but costs nothing to protect. */
struct RuleTagIndex
{
    const whaleui_css_rule_t* rules = nullptr;
    size_t count = 0;
    std::map<std::string, std::vector<size_t>> tag_idx;
    /* tag-less rules whose LAST simple selector carries a class: bucketed
     * by that class so elements without it skip them (a .card rule can
     * never match a class-less li). Cascade order stays exact: the Best
     * comparison uses the rule index i as its order tiebreak, so bucket
     * traversal order does not matter. */
    std::map<std::string, std::vector<size_t>> class_idx;
    /* everything else tag-less and class-less (attr / pseudo-class / *
     * rules): still probed per element */
    std::vector<size_t> generic_idx;
};
RuleTagIndex g_rule_index;
std::mutex g_rule_index_mtx;

/* The index is keyed on the rules array identity, but the allocator can
 * hand the same heap address to the next whaleui_css_parse after the old
 * set was destroyed. The destroy path must invalidate explicitly, or the
 * next document silently reuses a stale index (its class rules land in
 * buckets built for the previous sheet and never match). */
extern "C" void whaleui_style_index_drop(const whaleui_css_rule_t* rules)
{
    if (!rules) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_rule_index_mtx);
    if (g_rule_index.rules == rules) {
        g_rule_index.rules = nullptr;
        g_rule_index.count = 0;
        g_rule_index.tag_idx.clear();
        g_rule_index.class_idx.clear();
        g_rule_index.generic_idx.clear();
    }
}

} // namespace

extern "C" WhaleUIComputedStyle whaleui_style_compute(
    lxb_dom_element* el,
    const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>& vars,
    const whaleui_style_state* st)
{
    WhaleUIComputedStyle out;
    if (!el) {
        return out;
    }
    /* per-property best: (important, specificity, order) */
    struct Best { int important; Specificity sp; size_t order; std::string value; };
    std::map<std::string, Best> best;

    /* tag-index the rules by the LAST simple selector's tag (the one that
     * must match el itself): only those rules plus the tag-less ones can
     * possibly match, so the cascade scan skips the rest. The index is
     * cached per rule set (see RuleTagIndex above). */
    std::string el_tag;
    {
        size_t nl = 0;
        const lxb_char_t* nm = lxb_dom_element_local_name(el, &nl);
        if (nm) {
            el_tag.assign(reinterpret_cast<const char*>(nm), nl);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_rule_index_mtx);
        if (g_rule_index.rules != rules || g_rule_index.count != count) {
            g_rule_index.rules = rules;
            g_rule_index.count = count;
            g_rule_index.tag_idx.clear();
            g_rule_index.class_idx.clear();
            g_rule_index.generic_idx.clear();
            for (size_t i = 0; i < count; ++i) {
                const char* sel = rules[i].selector;
                if (!sel) {
                    g_rule_index.generic_idx.push_back(i);
                    continue;
                }
                /* last simple selector: walk back from the end past
                 * whitespace and combinators, then read the last segment's
                 * leading tag */
                size_t len = std::strlen(sel);
                const char* seg_end = sel + len;
                while (seg_end > sel) {
                    char c = seg_end[-1];
                    if (c == ' ' || c == '\t' || c == '>' || c == '+') {
                        --seg_end;
                    } else {
                        break;
                    }
                }
                const char* seg_start = seg_end;
                while (seg_start > sel) {
                    char c = seg_start[-1];
                    if (c == ' ' || c == '\t' || c == '>' || c == '+') {
                        break;
                    }
                    if (c == ')') {
                        /* walk back past the matching '(' - function args
                         * like nth-child(n+4) contain '+'/spaces that are
                         * NOT combinators */
                        int depth = 1;
                        --seg_start;
                        while (seg_start > sel && depth) {
                            if (seg_start[-1] == ')') {
                                ++depth;
                            } else if (seg_start[-1] == '(') {
                                --depth;
                            }
                            --seg_start;
                        }
                        continue;
                    }
                    --seg_start;
                }
                const char* t = seg_start;
                while (t < seg_end && *t != '#' && *t != '.' && *t != ':' &&
                       *t != '[' && *t != ' ' && *t != '\t' && *t != '>' &&
                       *t != '+') {
                    ++t;
                }
                if (t > seg_start &&
                    !(t - seg_start == 1 && *seg_start == '*')) {
                    g_rule_index
                        .tag_idx[std::string(
                            seg_start,
                            static_cast<size_t>(t - seg_start))]
                        .push_back(i);
                } else {
                    /* tag-less: index by the first class of the last simple
                     * selector (.card, "section .card") so class-less
                     * elements skip every class rule in one check */
                    const char* dot = std::strchr(seg_start, '.');
                    const char* cn = dot ? dot + 1 : nullptr;
                    const char* ce = cn;
                    while (ce && ce < seg_end &&
                           ((*ce >= 'a' && *ce <= 'z') ||
                            (*ce >= 'A' && *ce <= 'Z') ||
                            (*ce >= '0' && *ce <= '9') || *ce == '-' ||
                            *ce == '_')) {
                        ++ce;
                    }
                    if (cn && ce > cn) {
                        g_rule_index
                            .class_idx[std::string(cn,
                                                   static_cast<size_t>(
                                                       ce - cn))]
                            .push_back(i);
                    } else {
                        g_rule_index.generic_idx.push_back(i);
                    }
                }
            }
        }
    }
    /* candidate rule indices: this element's tag + tag-less rules */
    const std::vector<size_t>* tagged = nullptr;
    {
        std::map<std::string, std::vector<size_t>>::iterator it =
            g_rule_index.tag_idx.find(el_tag);
        if (it != g_rule_index.tag_idx.end()) {
            tagged = &it->second;
        }
    }
    auto cascade_rule = [&](size_t i) {
        const whaleui_css_rule_t* r = &rules[i];
        if (!r->selector || !whaleui_style_match(r->selector, el, st)) {
            return;
        }
        Specificity sp = sel_specificity(r->selector);
        for (size_t d = 0; d < r->decl_count; ++d) {
            char* kv = r->decls[d];
            char* eq = std::strchr(kv, '=');
            if (!eq) {
                continue;
            }
            *eq = '\0';
            std::string name(kv);
            std::string value(eq + 1);
            *eq = '=';
            if (name.empty()) {
                continue;
            }
            int imp = r->important;
            auto it = best.find(name);
            if (it == best.end() ||
                imp > it->second.important ||
                (imp == it->second.important && better(sp, it->second.sp)) ||
                (imp == it->second.important && !better(sp, it->second.sp) && !better(it->second.sp, sp) &&
                 i >= it->second.order)) {
                Best b;
                b.important = imp;
                b.sp = sp;
                b.order = i;
                b.value = value;
                best[name] = b;
            }
        }
    };
    if (tagged) {
        for (size_t ci = 0; ci < tagged->size(); ++ci) {
            cascade_rule((*tagged)[ci]);
        }
    }
    /* class rules: probe only the buckets of the element's own classes
     * (most elements carry no class and skip every class rule here) */
    {
        size_t clen = 0;
        const lxb_char_t* cattr = lxb_dom_element_get_attribute(
            el, (const lxb_char_t*)"class", 5, &clen);
        if (cattr && clen) {
            const char* p = reinterpret_cast<const char*>(cattr);
            const char* end = p + clen;
            while (p < end) {
                while (p < end &&
                       (*p == ' ' || *p == '\t' || *p == '\n' ||
                        *p == '\r')) {
                    ++p;
                }
                const char* tok = p;
                while (p < end && *p != ' ' && *p != '\t' && *p != '\n' &&
                       *p != '\r') {
                    ++p;
                }
                if (p > tok) {
                    std::map<std::string, std::vector<size_t>>::iterator it =
                        g_rule_index.class_idx.find(
                            std::string(tok, static_cast<size_t>(p - tok)));
                    if (it != g_rule_index.class_idx.end()) {
                        for (size_t ci = 0; ci < it->second.size(); ++ci) {
                            cascade_rule(it->second[ci]);
                        }
                    }
                }
            }
        }
    }
    for (size_t ci = 0; ci < g_rule_index.generic_idx.size(); ++ci) {
        cascade_rule(g_rule_index.generic_idx[ci]);
    }
    for (auto& kv : best) {
        out[kv.first] = kv.second.value;
    }

    /* inline style wins unless the rule was !important */
    size_t alen = 0;
    const lxb_char_t* stv = lxb_dom_element_get_attribute(el, (const lxb_char_t*)"style", 5, &alen);
    if (stv) {
        std::string style(reinterpret_cast<const char*>(stv), alen);
        size_t pos = 0;
        while (pos < style.size()) {
            size_t colon = style.find(':', pos);
            if (colon == std::string::npos) {
                break;
            }
            size_t semi = style.find(';', colon);
            if (semi == std::string::npos) {
                semi = style.size();
            }
            std::string name = style.substr(pos, colon - pos);
            size_t nb = name.find_first_not_of(" \t");
            size_t ne = name.find_last_not_of(" \t");
            name = name.substr(nb, ne - nb + 1);
            std::string value = style.substr(colon + 1, semi - colon - 1);
            size_t vb = value.find_first_not_of(" \t");
            size_t ve = value.find_last_not_of(" \t\r\n");
            value = value.substr(vb, ve - vb + 1);
            bool imp = value.find("!important") != std::string::npos;
            if (imp) {
                size_t bang = value.find("!important");
                value = value.substr(0, bang);
                vb = value.find_first_not_of(" \t");
                ve = value.find_last_not_of(" \t");
                value = value.substr(vb, ve - vb + 1);
            }
            auto it = best.find(name);
            if (imp || it == best.end() || !it->second.important) {
                out[name] = value;
            }
            pos = semi + 1;
        }
    }

    /* resolve var() everywhere */
    for (auto& kv : out) {
        resolve_var(kv.second, vars);
    }
    /* expand shorthands the layout/render consume as longhands */
    expand_shorthands(out);
    /* ::selection colors (text-selection highlight). background and color
     * ride as virtual "selection-bg"/"selection-fg" properties the text
     * paint reads (render_text.cpp). Rules are theme-first/page-last, so
     * the LAST ::selection rule wins (page overrides theme, like the
     * cascade). Values go through resolve_var (the theme rule uses
     * var(--selection-bg/fg)). */
    if (out.find("selection-bg") == out.end() ||
        out.find("selection-fg") == out.end()) {
        for (size_t i = count; i-- > 0;) {
            const char* sel = rules[i].selector;
            size_t n2 = sel ? std::strlen(sel) : 0;
            if (n2 >= 11 &&
                std::strcmp(sel + n2 - 11, "::selection") == 0) {
                if (out.find("selection-bg") == out.end()) {
                    const char* bg = whaleui_css_get_property(&rules[i], "background");
                    if (!bg || !*bg) {
                        bg = whaleui_css_get_property(&rules[i],
                                                      "background-color");
                    }
                    if (bg && *bg) {
                        std::string bgs = bg;
                        resolve_var(bgs, vars);
                        if (!bgs.empty()) {
                            out["selection-bg"] = bgs;
                        }
                    }
                }
                if (out.find("selection-fg") == out.end()) {
                    const char* fg = whaleui_css_get_property(&rules[i], "color");
                    if (fg && *fg) {
                        std::string fgs = fg;
                        resolve_var(fgs, vars);
                        if (!fgs.empty()) {
                            out["selection-fg"] = fgs;
                        }
                    }
                }
                if (out.find("selection-bg") != out.end() &&
                    out.find("selection-fg") != out.end()) {
                    break;
                }
            }
        }
    }
    /* ::placeholder color rides as virtual "placeholder-color" (the input /
     * textarea paint reads it; falls back to --muted when absent). Same
     * last-wins + resolve_var treatment as ::selection above. */
    if (out.find("placeholder-color") == out.end()) {
        for (size_t i = count; i-- > 0;) {
            const char* sel = rules[i].selector;
            size_t n2 = sel ? std::strlen(sel) : 0;
            if (n2 >= 13 &&
                std::strcmp(sel + n2 - 13, "::placeholder") == 0) {
                const char* fg = whaleui_css_get_property(&rules[i], "color");
                if (fg && *fg) {
                    std::string fgs = fg;
                    resolve_var(fgs, vars);
                    if (!fgs.empty()) {
                        out["placeholder-color"] = fgs;
                    }
                }
                break;
            }
        }
    }
    return out;
}

extern "C" void whaleui_style_collect_vars_full(
    lxb_dom_element* root,
    const whaleui_css_rule_t* rules, size_t count,
    std::map<std::string, std::string>& vars)
{
    whaleui_style_collect_vars(root, vars);
    if (!root) {
        return;
    }
    /* :root == html element; a rule whose selector is ":root"/"html" applies */
    for (size_t i = 0; i < count; ++i) {
        const whaleui_css_rule_t* r = &rules[i];
        if (!r->selector) {
            continue;
        }
        bool is_root = std::strcmp(r->selector, ":root") == 0 ||
                       std::strcmp(r->selector, "html") == 0;
        if (!is_root) {
            continue;
        }
        for (size_t d = 0; d < r->decl_count; ++d) {
            char* kv = r->decls[d];
            if (kv[0] != '-' || kv[1] != '-') {
                continue;
            }
            char* eq = std::strchr(kv, '=');
            if (!eq) {
                continue;
            }
            *eq = '\0';
            vars[kv] = eq + 1;
            *eq = '=';
        }
    }
}

extern "C" int whaleui_style_parse_len(const char* value, float* num, int* unit)
{
    if (!value || !num || !unit) {
        return -1;
    }
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    if (std::strcmp(value, "auto") == 0 || std::strcmp(value, "none") == 0) {
        *num = 0;
        *unit = 3;
        return 0;
    }
    char* end = nullptr;
    float v = std::strtof(value, &end);
    if (end == value) {
        return -2;
    }
    *num = v;
    if (*end == '\0') {
        *unit = 4; /* unitless number */
    } else if (end[0] == 'p' && end[1] == 'x') {
        *unit = 0;
    } else if (*end == '%') {
        *unit = 1;
    } else if (end[0] == 'e' && end[1] == 'm') {
        *unit = 2;
    } else {
        return -3;
    }
    return 0;
}

/* --- supported-property table (CSS.supports) ---
 *
 * The CSS parser keeps unknown `name: value` declarations on purpose (a page
 * may carry vendor prefixes or future syntax without its whole declaration
 * list being rejected), so the parser cannot answer "is this supported" - it
 * said yes to everything. CSS.supports therefore needs an explicit list of the
 * properties the engine reads. Extend it whenever a property gains an
 * implementation (the authoritative set is what layout/paint/animate read). */
extern "C" int whaleui_style_property_supported(const char* name)
{
    if (!name || !*name) {
        return 0;
    }
    /* custom properties are stored and resolved, never interpreted */
    if (name[0] == '-' && name[1] == '-') {
        return 1;
    }
    static const char* const kProps[] = {
        "align-content", "align-items", "align-self", "animation",
        "animation-delay", "animation-direction", "animation-duration",
        "animation-fill-mode", "animation-iteration-count", "animation-name",
        "animation-play-state", "animation-timing-function", "aspect-ratio",
        "backdrop-filter", "background", "background-blend-mode",
        "background-color", "background-image", "background-position",
        "background-repeat", "background-size", "border", "border-color",
        "border-radius", "border-style", "border-width", "border-top",
        "border-right", "border-bottom", "border-left", "border-top-color",
        "border-right-color", "border-bottom-color", "border-left-color",
        "border-top-width", "border-right-width", "border-bottom-width",
        "border-left-width", "border-top-style", "border-right-style",
        "border-bottom-style", "border-left-style", "bottom", "box-shadow",
        "box-sizing", "clip-path", "color", "column-gap", "contain",
        "content", "counter-increment", "counter-reset", "counter-set",
        "cursor", "display", "filter", "flex", "flex-basis", "flex-direction",
        "flex-grow", "flex-shrink", "flex-wrap", "font", "font-family",
        "font-size", "font-style", "font-weight", "gap", "grid-column",
        "grid-column-end", "grid-column-start", "grid-row", "grid-row-end",
        "grid-row-start", "grid-template-columns", "grid-template-rows",
        "height", "inset", "justify-content", "left", "letter-spacing",
        "line-height", "list-style", "list-style-type", "margin",
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "mask-image", "max-height", "max-width", "min-height", "min-width",
        "mix-blend-mode", "object-fit", "object-position", "opacity", "order",
        "outline", "outline-color", "outline-offset", "outline-style",
        "outline-width", "overflow", "overflow-x", "overflow-y", "padding",
        "padding-top", "padding-right", "padding-bottom", "padding-left",
        "place-items", "pointer-events", "position", "right", "row-gap",
        "scrollbar-color", "scrollbar-width", "text-align", "text-decoration",
        "text-shadow", "text-transform", "top", "transform",
        "transform-origin", "transition", "transition-delay",
        "transition-duration", "transition-property",
        "transition-timing-function", "vertical-align", "visibility",
        "white-space", "width", "word-break", "z-index",
        /* engine extensions (see doc/external/html-css-support.md) */
        "-webkit-text-stroke", "-webkit-text-stroke-color",
        "-webkit-text-stroke-width", "text-stroke", "text-stroke-color",
        "text-stroke-width", "placeholder-color", "selection-bg",
        "selection-fg", "-webkit-mask-image",
    };
    for (size_t i = 0; i < sizeof(kProps) / sizeof(kProps[0]); ++i) {
        if (std::strcmp(name, kProps[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* --- value plausibility (CSS.supports second half) ---
 *
 * Property support is a table (above); VALUE support is checked with the
 * engine's own parsers wherever one exists, so the answer tracks what the
 * renderer would really do:
 *   - color properties  -> whaleui_render_parse_color
 *   - length properties -> whaleui_style_parse_len (plus keywords/calc())
 *   - the enumerated keyword properties (display, position, overflow, ...)
 *     -> an explicit keyword set
 * A property whose value syntax the engine does not parse yet (a filter list,
 * a shadow, a transform function) answers 1 for any non-empty value: the
 * renderer would attempt it, and claiming otherwise would be a guess. That is
 * deliberately the opposite bias from the property check, which must be
 * conservative because a wrong "yes" there is what made CSS.supports useless. */
extern "C" int whaleui_style_value_supported(const char* name, const char* value)
{
    if (!name || !*name || !value || !*value) {
        return 0;
    }
    std::string v(value);
    size_t b = v.find_first_not_of(" \t");
    size_t e = v.find_last_not_of(" \t");
    if (b == std::string::npos) {
        return 0;
    }
    v = v.substr(b, e - b + 1);
    /* "inherit"/"initial"/"unset" are valid for every property */
    if (v == "inherit" || v == "initial" || v == "unset" || v == "revert") {
        return 1;
    }
    /* var()/calc()/clamp()/min()/max() resolve at cascade time */
    if (v.find("var(") != std::string::npos || v.find("calc(") != std::string::npos ||
        v.find("clamp(") != std::string::npos || v.find("min(") != std::string::npos ||
        v.find("max(") != std::string::npos) {
        return 1;
    }
    /* gradients / images / url() are opaque payloads here */
    if (v.find("gradient(") != std::string::npos ||
        v.find("url(") != std::string::npos) {
        return 1;
    }
    std::string n(name);
    for (size_t i = 0; i < n.size(); ++i) {
        char c = n[i];
        if (c >= 'A' && c <= 'Z') {
            n[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    /* value keywords are ASCII case-insensitive too ("FLEX" == "flex") */
    {
        for (size_t i = 0; i < v.size(); ++i) {
            char c = v[i];
            if (c >= 'A' && c <= 'Z') {
                v[i] = static_cast<char>(c - 'A' + 'a');
            }
        }
    }
    /* color-valued properties */
    static const char* const kColors[] = {
        "color", "background-color", "border-color", "border-top-color",
        "border-right-color", "border-bottom-color", "border-left-color",
        "outline-color", "text-decoration-color", "caret-color",
        "scrollbar-color", "placeholder-color", "selection-bg", "selection-fg",
        "background", "border", "outline", "background-image", "border-top",
        "border-right", "border-bottom", "border-left",
    };
    for (size_t i = 0; i < sizeof(kColors) / sizeof(kColors[0]); ++i) {
        if (n == kColors[i]) {
            unsigned int probe = 0;
            if (whaleui_render_parse_color(v.c_str(), &probe) == 0) {
                return 1;
            }
            /* shorthand: a colour anywhere in the list is enough */
            if (n == "background" || n == "border" || n == "outline" ||
                n == "border-top" || n == "border-right" ||
                n == "border-bottom" || n == "border-left") {
                return v.find("none") != std::string::npos ||
                       v.find("solid") != std::string::npos ||
                       v.find("transparent") != std::string::npos;
            }
            return 0;
        }
    }
    /* length-valued properties */
    static const char* const kLengths[] = {
        "width", "height", "min-width", "max-width", "min-height",
        "max-height", "top", "right", "bottom", "left", "margin",
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding", "padding-top", "padding-right", "padding-bottom",
        "padding-left", "font-size", "line-height", "letter-spacing",
        "border-radius", "border-width", "border-top-width",
        "border-right-width", "border-bottom-width", "border-left-width",
        "outline-width", "outline-offset", "gap", "row-gap", "column-gap",
        "flex-basis", "inset", "vertical-align", "text-stroke-width",
        "text-stroke", "-webkit-text-stroke", "-webkit-text-stroke-width",
        "scrollbar-width",
    };
    for (size_t i = 0; i < sizeof(kLengths) / sizeof(kLengths[0]); ++i) {
        if (n == kLengths[i]) {
            float num = 0.0f;
            int unit = 0;
            if (whaleui_style_parse_len(v.c_str(), &num, &unit) == 0) {
                return 1;
            }
            /* keywords these properties also accept */
            static const char* const kKw[] = {
                "auto", "none", "normal", "thin", "medium", "thick",
                "baseline", "middle", "sub", "super", "text-top",
                "text-bottom", "inherit",
            };
            for (size_t k = 0; k < sizeof(kKw) / sizeof(kKw[0]); ++k) {
                if (v == kKw[k]) {
                    return 1;
                }
            }
            /* multi-value (padding/margin/border-radius/inset shorthands) */
            if (n == "padding" || n == "margin" || n == "border-radius" ||
                n == "inset" || n == "gap") {
                size_t sp = v.find(' ');
                if (sp != std::string::npos) {
                    float n2 = 0.0f;
                    int u2 = 0;
                    if (whaleui_style_parse_len(v.substr(sp + 1).c_str(), &n2,
                                                &u2) == 0) {
                        return 1;
                    }
                }
            }
            return 0;
        }
    }
    /* enumerated keyword properties */
    struct KeywordSet { const char* prop; const char* const* words; size_t n; };
    static const char* const kDisplay[] = {
        "none", "block", "inline", "inline-block", "flex", "inline-flex",
        "grid", "inline-grid", "table", "table-row", "table-cell",
        "table-header-group", "table-row-group", "table-footer-group",
        "list-item", "contents", "flow-root",
    };
    static const char* const kPosition[] = {
        "static", "relative", "absolute", "fixed", "sticky",
    };
    static const char* const kOverflow[] = {
        "visible", "hidden", "scroll", "auto", "clip",
    };
    static const char* const kBoxSizing[] = {"content-box", "border-box"};
    static const char* const kTextAlign[] = {
        "left", "right", "center", "justify", "start", "end",
    };
    static const char* const kWhiteSpace[] = {
        "normal", "nowrap", "pre", "pre-wrap", "pre-line", "break-spaces",
    };
    static const char* const kFontStyle[] = {"normal", "italic", "oblique"};
    static const char* const kTextTransform[] = {
        "none", "uppercase", "lowercase", "capitalize",
    };
    static const char* const kVisibility[] = {"visible", "hidden", "collapse"};
    static const char* const kCursor[] = {
        "auto", "default", "pointer", "text", "move", "wait", "help",
        "progress", "crosshair", "not-allowed", "grab", "grabbing",
        "col-resize", "row-resize", "ew-resize", "ns-resize", "none",
    };
    static const char* const kPointerEvents[] = {"auto", "none"};
    static const char* const kFlexDirection[] = {
        "row", "row-reverse", "column", "column-reverse",
    };
    static const char* const kFlexWrap[] = {"nowrap", "wrap", "wrap-reverse"};
    static const char* const kJustify[] = {
        "flex-start", "flex-end", "center", "space-between", "space-around",
        "space-evenly", "start", "end", "left", "right", "stretch",
    };
    static const char* const kAlign[] = {
        "flex-start", "flex-end", "center", "baseline", "stretch", "start",
        "end", "self-start", "self-end", "normal",
    };
    static const char* const kObjectFit[] = {
        "fill", "contain", "cover", "none", "scale-down",
    };
    static const char* const kBorderStyle[] = {
        "none", "hidden", "solid", "dashed", "dotted", "double", "groove",
        "ridge", "inset", "outset",
    };
    static const char* const kBlend[] = {
        "normal", "multiply", "screen", "overlay", "darken", "lighten",
        "color-dodge", "color-burn", "hard-light", "soft-light",
        "difference", "exclusion", "hue", "saturation", "color", "luminosity",
        "plus-lighter",
    };
    static const KeywordSet kSets[] = {
        {"display", kDisplay, sizeof(kDisplay) / sizeof(kDisplay[0])},
        {"position", kPosition, sizeof(kPosition) / sizeof(kPosition[0])},
        {"overflow", kOverflow, sizeof(kOverflow) / sizeof(kOverflow[0])},
        {"overflow-x", kOverflow, sizeof(kOverflow) / sizeof(kOverflow[0])},
        {"overflow-y", kOverflow, sizeof(kOverflow) / sizeof(kOverflow[0])},
        {"box-sizing", kBoxSizing, sizeof(kBoxSizing) / sizeof(kBoxSizing[0])},
        {"text-align", kTextAlign, sizeof(kTextAlign) / sizeof(kTextAlign[0])},
        {"white-space", kWhiteSpace, sizeof(kWhiteSpace) / sizeof(kWhiteSpace[0])},
        {"font-style", kFontStyle, sizeof(kFontStyle) / sizeof(kFontStyle[0])},
        {"text-transform", kTextTransform,
         sizeof(kTextTransform) / sizeof(kTextTransform[0])},
        {"visibility", kVisibility, sizeof(kVisibility) / sizeof(kVisibility[0])},
        {"cursor", kCursor, sizeof(kCursor) / sizeof(kCursor[0])},
        {"pointer-events", kPointerEvents,
         sizeof(kPointerEvents) / sizeof(kPointerEvents[0])},
        {"flex-direction", kFlexDirection,
         sizeof(kFlexDirection) / sizeof(kFlexDirection[0])},
        {"flex-wrap", kFlexWrap, sizeof(kFlexWrap) / sizeof(kFlexWrap[0])},
        {"justify-content", kJustify, sizeof(kJustify) / sizeof(kJustify[0])},
        {"align-items", kAlign, sizeof(kAlign) / sizeof(kAlign[0])},
        {"align-self", kAlign, sizeof(kAlign) / sizeof(kAlign[0])},
        {"align-content", kJustify, sizeof(kJustify) / sizeof(kJustify[0])},
        {"object-fit", kObjectFit, sizeof(kObjectFit) / sizeof(kObjectFit[0])},
        {"border-style", kBorderStyle,
         sizeof(kBorderStyle) / sizeof(kBorderStyle[0])},
        {"mix-blend-mode", kBlend, sizeof(kBlend) / sizeof(kBlend[0])},
        {"background-blend-mode", kBlend, sizeof(kBlend) / sizeof(kBlend[0])},
    };
    for (size_t i = 0; i < sizeof(kSets) / sizeof(kSets[0]); ++i) {
        if (n != kSets[i].prop) {
            continue;
        }
        for (size_t k = 0; k < kSets[i].n; ++k) {
            if (v == kSets[i].words[k]) {
                return 1;
            }
        }
        /* overflow-x/-y also take "overlay"; shorthands take two keywords */
        if (v.find(' ') != std::string::npos) {
            size_t sp = v.find(' ');
            std::string first = v.substr(0, sp);
            for (size_t k = 0; k < kSets[i].n; ++k) {
                if (first == kSets[i].words[k]) {
                    return 1;
                }
            }
        }
        return 0;
    }
    /* everything else: the engine has no value parser for it, so accept a
     * non-empty value (see the note above) */
    return 1;
}

/* --- document-level programmatic stylesheet (whaleui_css_apply) ---
 *
 * Keyed on the lxb_dom_document (the address the DOM layer uses as the
 * document identity). Cleared on document destroy - the same address-reuse
 * hazard the other per-document maps document. */
namespace {
struct DocRules
{
    whaleui_css_rule_t* rules;
    size_t count;
};
std::map<lxb_dom_document*, DocRules>& g_doc_rules()
{
    static std::map<lxb_dom_document*, DocRules> m;
    return m;
}

void doc_rule_free(whaleui_css_rule_t* r)
{
    if (!r) {
        return;
    }
    std::free(r->selector);
    std::free(r->media);
    for (size_t d = 0; d < r->decl_count; ++d) {
        std::free(r->decls[d]);
    }
    std::free(r->decls);
}

/* deep copy one rule (the API caller owns its array and may free it right
 * after the call) */
bool doc_rule_copy(whaleui_css_rule_t* dst, const whaleui_css_rule_t* src)
{
    std::memset(dst, 0, sizeof(*dst));
    dst->important = src->important;
    const char* sel = src->selector ? src->selector : "";
    size_t n = std::strlen(sel);
    dst->selector = static_cast<char*>(std::malloc(n + 1));
    if (!dst->selector) {
        return false;
    }
    std::memcpy(dst->selector, sel, n + 1);
    if (src->media) {
        size_t m = std::strlen(src->media);
        dst->media = static_cast<char*>(std::malloc(m + 1));
        if (!dst->media) {
            return false;
        }
        std::memcpy(dst->media, src->media, m + 1);
    }
    if (src->decl_count) {
        dst->decls = static_cast<char**>(
            std::malloc(src->decl_count * sizeof(char*)));
        if (!dst->decls) {
            return false;
        }
        for (size_t d = 0; d < src->decl_count; ++d) {
            const char* v = src->decls[d] ? src->decls[d] : "";
            size_t vn = std::strlen(v);
            dst->decls[d] = static_cast<char*>(std::malloc(vn + 1));
            if (!dst->decls[d]) {
                dst->decl_count = d;
                return false;
            }
            std::memcpy(dst->decls[d], v, vn + 1);
        }
        dst->decl_count = src->decl_count;
    }
    return true;
}
} // namespace

void whaleui_style_doc_rules_set(whaleui_dom_document_t* doc,
                                 const whaleui_css_rule_t* rules, size_t count)
{
    if (!doc) {
        return;
    }
    lxb_dom_document* key = &reinterpret_cast<lxb_html_document*>(doc)->dom_document;
    whaleui_style_doc_rules_clear(doc);
    if (!rules || count == 0) {
        return;
    }
    whaleui_css_rule_t* copy = static_cast<whaleui_css_rule_t*>(
        std::calloc(count, sizeof(whaleui_css_rule_t)));
    if (!copy) {
        return;
    }
    size_t made = 0;
    for (; made < count; ++made) {
        if (!doc_rule_copy(&copy[made], &rules[made])) {
            break;
        }
    }
    if (made < count) {
        /* allocation failed part-way: keep the sheet consistent (no half
         * rule) rather than leaving a partially copied array */
        for (size_t i = 0; i <= made && i < count; ++i) {
            doc_rule_free(&copy[i]);
        }
        std::free(copy);
        return;
    }
    DocRules dr;
    dr.rules = copy;
    dr.count = count;
    g_doc_rules()[key] = dr;
}

const whaleui_css_rule_t* whaleui_style_doc_rules_get(whaleui_dom_document_t* doc,
                                                      size_t* count)
{
    if (count) {
        *count = 0;
    }
    if (!doc) {
        return nullptr;
    }
    lxb_dom_document* key = &reinterpret_cast<lxb_html_document*>(doc)->dom_document;
    std::map<lxb_dom_document*, DocRules>::const_iterator it =
        g_doc_rules().find(key);
    if (it == g_doc_rules().end()) {
        return nullptr;
    }
    if (count) {
        *count = it->second.count;
    }
    return it->second.rules;
}

void whaleui_style_doc_rules_clear(whaleui_dom_document_t* doc)
{
    if (!doc) {
        return;
    }
    lxb_dom_document* key = &reinterpret_cast<lxb_html_document*>(doc)->dom_document;
    std::map<lxb_dom_document*, DocRules>::iterator it = g_doc_rules().find(key);
    if (it == g_doc_rules().end()) {
        return;
    }
    for (size_t i = 0; i < it->second.count; ++i) {
        doc_rule_free(&it->second.rules[i]);
    }
    std::free(it->second.rules);
    g_doc_rules().erase(it);
}

/* css_apply: attach a parsed stylesheet to the document. The rules are kept as
 * the document's OWN sheet (deep-copied, replacing any previous set) and are
 * appended to the stylesheet the window assembles (theme default + <style> /
 * <link>), so they cascade like any other sheet on the next frame. The old
 * implementation only computed a throwaway style map here - nothing rendered
 * from it, so the documented "apply a set of rules to a document" changed no
 * pixel (the "CSS from the API does nothing" report; the same CSS in the
 * markup worked because the collector read it). */
extern "C" int whaleui_css_apply(whaleui_dom_document_t* doc,
                                 const whaleui_css_rule_t* rules, size_t count)
{
    if (!doc) {
        return -1;
    }
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    if (!lxb_dom_document_element(&hd->dom_document)) {
        return 0;
    }
    whaleui_style_doc_rules_set(doc, rules, count);
    /* the window's assembled sheet is stale: re-collect before the next frame
     * (the DOM layer owns that flag; see dom.h whaleui_dom_mark_css_dirty) */
    whaleui_dom_mark_css_dirty(doc);
    return 0;
}

/* Merge the declarations of every rule whose selector equals `selector`
 * (a synthetic element with no DOM node). Used by the renderer for chrome
 * that has no element - the select dropdown popup (.select-popup /
 * .select-option-hover). var() values resolve against vars; later rules
 * win per declaration (cascade order). */
extern "C" void whaleui_style_virtual(
    const char* selector, const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>& vars,
    WhaleUIComputedStyle& out);

/* Shadow-host variant: the popup is the select's shadow child, so page CSS
 * can target it through the host (`select .select-popup`,
 * `select.foo .select-option-hover`, `#theme > .select-popup`). A branch
 * matches when its right-most compound IS the virtual class and the
 * ancestor part matches the HOST element itself: for `P .popup` the spec
 * wants P on the virtual child's ancestor-or-self (= host or its
 * ancestors); for `P > .popup` it wants P on the parent (= host). Feeding
 * the ancestor part to the normal matcher with the host as the target
 * satisfies both (descendant matching is transitive over the host's own
 * ancestors). Bare `.select-popup` (no ancestor part) always matches.
 * Sibling combinators (`+`/`~`) have no meaning for a shadow child and do
 * not match. `st` carries the interaction state so `:hover` on the host
 * (open select is hovered) applies. */
extern "C" void whaleui_style_virtual_host(
    const char* vclass, lxb_dom_element* host,
    const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>& vars,
    const whaleui_style_state* st, WhaleUIComputedStyle& out)
{
    if (!vclass || !host || !rules) {
        return;
    }
    const size_t vn = std::strlen(vclass);
    for (size_t i = 0; i < count; ++i) {
        const char* sel = rules[i].selector;
        if (!sel || !*sel) {
            continue;
        }
        /* paren-aware comma split (functional pseudo-classes carry
         * top-level commas that must not split) */
        size_t start = 0;
        int depth = 0;
        for (size_t k = 0; k <= std::strlen(sel); ++k) {
            char c = sel[k];
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (depth > 0) {
                    --depth;
                }
            }
            if ((c == '\0' || c == ',') && depth == 0) {
                std::string branch(sel + start, k - start);
                start = k + 1;
                /* trim */
                size_t b = branch.find_first_not_of(" \t");
                if (b == std::string::npos) {
                    continue;
                }
                size_t e = branch.find_last_not_of(" \t");
                branch = branch.substr(b, e - b + 1);
                if (branch.empty()) {
                    continue;
                }
                /* the branch must END with the virtual class as its own
                 * compound: strip trailing paren-depth first (a trailing
                 * `:hover` on the popup itself has no state source - the
                 * popup rows use open_select_hover instead - so any
                 * functional/pseudo suffix rejects the branch) */
                if (branch.size() < vn ||
                    branch.compare(branch.size() - vn, vn, vclass) != 0) {
                    continue;
                }
                std::string head =
                    branch.substr(0, branch.size() - vn);
                size_t hb = head.find_last_not_of(" \t");
                if (hb == std::string::npos) {
                    /* bare virtual class: always matches */
                } else {
                    head = head.substr(0, hb + 1);
                    /* the char before the virtual class must be a
                     * descendant or child combinator */
                    char comb = head[head.size() - 1];
                    if (comb != ' ' && comb != '\t' && comb != '>') {
                        continue; /* `+`/`~`/compound: no shadow meaning */
                    }
                    head = head.substr(0, head.size() - 1);
                    size_t h2 = head.find_last_not_of(" \t");
                    if (h2 == std::string::npos) {
                        continue;
                    }
                    head = head.substr(0, h2 + 1);
                    if (!whaleui_style_match(head.c_str(), host, st)) {
                        continue;
                    }
                }
                for (size_t d = 0; d < rules[i].decl_count; ++d) {
                    char* kv = rules[i].decls[d];
                    char* eq = std::strchr(kv, '=');
                    if (!eq) {
                        continue;
                    }
                    std::string name(kv, static_cast<size_t>(eq - kv));
                    if (name.empty() ||
                        (name.size() >= 2 && name[0] == '-' &&
                         name[1] == '-')) {
                        continue;
                    }
                    std::string val(eq + 1);
                    resolve_var(val, vars);
                    out[name] = val;
                }
            }
        }
    }
}
extern "C" void whaleui_style_virtual(
    const char* selector, const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>& vars,
    WhaleUIComputedStyle& out)
{
    if (!selector || !rules) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const char* sel = rules[i].selector;
        if (!sel || std::strcmp(sel, selector) != 0) {
            continue;
        }
        for (size_t d = 0; d < rules[i].decl_count; ++d) {
            char* kv = rules[i].decls[d];
            char* eq = std::strchr(kv, '=');
            if (!eq) {
                continue;
            }
            std::string name(kv, static_cast<size_t>(eq - kv));
            if (name.empty() ||
                (name.size() >= 2 && name[0] == '-' && name[1] == '-')) {
                continue; /* custom properties are not element styles */
            }
            std::string val(eq + 1);
            resolve_var(val, vars);
            out[name] = val;
        }
    }
}

/* The ancestor part of a selector: everything before its LAST top-level
 * compound, with trailing combinators trimmed. "" when the selector is a
 * single compound (nothing can match an ancestor). */
std::string whaleui_style_ancestor_prefix(const char* selector)
{
    if (!selector || !*selector) {
        return std::string();
    }
    const char* last_start = selector;
    int depth = 0;
    for (const char* p = selector; *p; ++p) {
        char c = *p;
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth > 0) {
                --depth;
            }
        } else if (depth == 0 &&
                   (c == ' ' || c == '\t' || c == '>' || c == '+')) {
            const char* q = p;
            while (*q == ' ' || *q == '\t' || *q == '>' || *q == '+') {
                ++q;
            }
            if (*q) {
                last_start = q;
            }
            p = q - 1;
        }
    }
    if (last_start == selector) {
        return std::string();
    }
    size_t plen = static_cast<size_t>(last_start - selector);
    while (plen > 0) {
        char c = selector[plen - 1];
        if (c == ' ' || c == '\t' || c == '>' || c == '+') {
            --plen;
        } else {
            break;
        }
    }
    return std::string(selector, plen);
}

/* See style.h. */
extern "C" int whaleui_style_may_affect_descendants(
    const char* selector, lxb_dom_element* ancestor,
    const whaleui_style_state* st)
{
    std::string prefix = whaleui_style_ancestor_prefix(selector);
    if (prefix.empty()) {
        return 0;
    }
    return whaleui_style_match(prefix.c_str(), ancestor, st);
}
