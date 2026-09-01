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
    bool last_child;    /* ":last-child" present */
    bool first_child;   /* ":first-child" present */
    bool nth;           /* ":nth-child(An+B)" present */
    int nth_a, nth_b;   /* position p matches when p = A*n + B, n >= 0 */
    bool pseudo_el;     /* "::before"/"::after" suffix: never matches el */
    bool has_attr;      /* "[name]" / "[name=value]" present */
    std::string attr;   /* attribute name */
    std::string attr_val; /* expected value ("" = presence only) */
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
            /* attribute selector: [name] presence, [name="value"] equality.
             * Quotes inside the value are handled so "=" within them is not
             * mistaken for the operator. */
            ++p;
            const char* as = p;
            while (p < end && *p != ']' && *p != '=') {
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
            }
            if (p < end && *p == '=') {
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
            while (p < end && *p != '#' && *p != '.' && *p != ':' &&
                   *p != '[') {
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
            out.last_child || out.first_child || out.nth || out.has_attr);
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
        size_t alen = 0;
        const lxb_char_t* d = lxb_dom_element_get_attribute(el,
                                                            (const lxb_char_t*)"disabled", 8, &alen);
        if (!d || alen == 0) {
            return false;
        }
    }
    if (p.has_attr) {
        size_t alen = 0;
        const lxb_char_t* a = lxb_dom_element_get_attribute(
            el, reinterpret_cast<const lxb_char_t*>(p.attr.c_str()),
            p.attr.size(), &alen);
        if (!a) {
            return false; /* [name]: the attribute must exist */
        }
        if (!p.attr_val.empty() &&
            (alen != p.attr_val.size() ||
             std::memcmp(a, p.attr_val.data(), p.attr_val.size()) != 0)) {
            return false; /* [name="value"]: exact match */
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
                                      int viewport_w, int reduced_motion)
{
    if (!media || !*media) {
        return 1;
    }
    std::string m(media);
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
    std::vector<size_t> generic_idx;
};
RuleTagIndex g_rule_index;
std::mutex g_rule_index_mtx;

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
                    g_rule_index.generic_idx.push_back(i);
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
    /* ::selection background (text-selection highlight color) */
    if (out.find("selection-bg") == out.end()) {
        for (size_t i = 0; i < count; ++i) {
            const char* sel = rules[i].selector;
            size_t n2 = sel ? std::strlen(sel) : 0;
            if (n2 >= 11 &&
                std::strcmp(sel + n2 - 11, "::selection") == 0) {
                const char* bg = whaleui_css_get_property(&rules[i], "background");
                if (bg && *bg) {
                    out["selection-bg"] = bg;
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

/* Merge the declarations of every rule whose selector equals `selector`
 * (a synthetic element with no DOM node). Used by the renderer for chrome
 * that has no element - the select dropdown popup (.select-popup /
 * .select-option-hover). var() values resolve against vars; later rules
 * win per declaration (cascade order). */
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

