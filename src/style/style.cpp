/* Style engine: selector matching, cascade, var() resolution.
 *
 * Computed styles are plain property->value maps. Cascade order:
 * !important > specificity (id, class, type) > source order; inline style
 * overrides everything except !important. */

#include "style/style.h"

#include "render/render.h" /* whaleui_render_parse_color */

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <cstdlib>
#include <cstring>
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
    bool last_child;    /* ":last-child" present */
};

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
        while (p < end && *p != '#' && *p != '.' && *p != ':') {
            ++p;
        }
        out.tag.assign(t, static_cast<size_t>(p - t));
        if (out.tag == "*") {
            out.tag.clear(); /* universal selector */
        }
    }
    while (p < end) {
        if (*p == ':') {
            ++p;
            const char* s = p;
            while (p < end && *p != '#' && *p != '.' && *p != ':') {
                ++p;
            }
            std::string v(s, static_cast<size_t>(p - s));
            if (v == "hover") {
                out.hover = true;
            } else if (v == "active") {
                out.active = true;
            } else if (v == "focus" || v == "focus-visible") {
                out.focus = true;
            } else if (v == "disabled") {
                out.disabled = true;
            } else if (v == "last-child") {
                out.last_child = true;
            }
            continue;
        }
        char kind = *p++;
        const char* s = p;
        while (p < end && *p != '#' && *p != '.' && *p != ':') {
            ++p;
        }
        std::string v(s, static_cast<size_t>(p - s));
        if (kind == '#') {
            out.id = v;
        } else if (kind == '.') {
            if (out.cls.empty()) {
                out.cls = v;
            }
        }
    }
    return !out.tag.empty() || !out.id.empty() || !out.cls.empty() ||
           out.hover || out.active || out.focus || out.disabled ||
           out.last_child;
}

bool el_has_class(lxb_dom_element* el, const std::string& cls)
{
    size_t alen = 0;
    const lxb_char_t* a = lxb_dom_element_get_attribute(el, (const lxb_char_t*)"class", 5, &alen);
    if (!a) {
        return false;
    }
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
        if (static_cast<size_t>(c - tok) == cls.size() && std::memcmp(tok, cls.data(), cls.size()) == 0) {
            return true;
        }
    }
    return false;
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
        size_t alen = 0;
        const lxb_char_t* d = lxb_dom_element_get_attribute(el,
                                                            (const lxb_char_t*)"disabled", 8, &alen);
        if (!d || alen == 0) {
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
    return true;
}

/* --- specificity --- */

struct Specificity { int a, b, c; };

Specificity sel_specificity(const char* sel)
{
    Specificity sp = {0, 0, 0};
    for (const char* p = sel; *p; ++p) {
        if (*p == '#') {
            ++sp.a;
        } else if (*p == '.') {
            ++sp.b;
        } else if (*p == ':') {
            ++sp.b; /* pseudo-class counts as class */
        }
    }
    return sp;
}

bool better(const Specificity& a, const Specificity& b)
{
    if (a.a != b.a) {
        return a.a > b.a;
    }
    if (a.b != b.b) {
        return a.b > b.b;
    }
    return a.c > b.c;
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
    if (s.find("font-size") != s.end()) {
        return; /* longhand wins */
    }
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
    expand_border_side(s, "border-top", "border-top-width");
    expand_border_side(s, "border-bottom", "border-bottom-width");
    expand_border_side(s, "border-left", "border-left-width");
    expand_border_side(s, "border-right", "border-right-width");
    WhaleUIComputedStyle::const_iterator b = s.find("border");
    if (b != s.end() && s.find("border-color") == s.end()) {
        std::string c = value_color(b->second);
        if (!c.empty()) {
            s["border-color"] = c;
        }
    }
}

} // namespace

extern "C" int whaleui_style_match(const char* selector, lxb_dom_element* el,
                                   const whaleui_style_state* st)
{
    if (!selector || !el) {
        return 0;
    }
    /* split on '>'/'+' and whitespace into a chain of simple selectors.
     * comb: 0 = descendant, 1 = child (prev part's relation), 2 = adjacent
     * sibling (this part must match the previous sibling of the target). */
    struct Chain { std::string sel; int comb; };
    Chain chain[32];
    int n = 0;
    int comb = 0;
    const char* p = selector;
    while (*p && n < 32) {
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
            ++p;
        }
        /* keep pseudo-classes: parse_simple handles ":hover" */
        std::string raw(s, static_cast<size_t>(p - s));
        if (!raw.empty()) {
            chain[n].sel = raw;
            chain[n].comb = comb;
            comb = 0;
            ++n;
        }
    }
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
            continue; /* pseudo-classes only apply to the target element */
        }
        /* pseudo-classes apply to the target element itself. :hover and
         * :active bubble per CSS: they also match when the interaction
         * target is a descendant of this element (a button containing a
         * <span> stays :hover while the mouse is over the span) */
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
    }
    return 1;
}

extern "C" int whaleui_style_media_ok(const char* media, int theme, int viewport_w)
{
    if (!media || !*media) {
        return 1;
    }
    std::string m(media);
    size_t pos = 0;
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

    for (size_t i = 0; i < count; ++i) {
        const whaleui_css_rule_t* r = &rules[i];
        if (!r->selector || !whaleui_style_match(r->selector, el, st)) {
            continue;
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

/* css_apply: walk the document computing styles for every element. The
 * results are not cached here - layout/render call whaleui_style_compute
 * per pass. This keeps the public contract (returns 0) while exercising the
 * whole cascade pipeline. */
extern "C" int whaleui_css_apply(whaleui_dom_document_t* doc,
                                 const whaleui_css_rule_t* rules, size_t count)
{
    if (!doc) {
        return -1;
    }
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    lxb_dom_element* root = lxb_dom_document_element(&hd->dom_document);
    if (!root) {
        return 0;
    }
    std::map<std::string, std::string> vars;
    whaleui_style_collect_vars_full(root, rules, count, vars);
    std::function<void(lxb_dom_element*)> walk = [&](lxb_dom_element* el) {
        whaleui_style_compute(el, rules, count, vars, nullptr);
        for (lxb_dom_node* c = el->node.first_child; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                walk(lxb_dom_interface_element(c));
            }
        }
    };
    walk(root);
    return 0;
}
