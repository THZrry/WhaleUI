/* Layout: small box/flex engine.
 *
 * Lexbor parses HTML/CSS but computes no layout, so this module implements
 * the layout pass itself: box model (margin/border/padding/content),
 * block flow, a basic flex layout, position (static/relative/absolute/
 * fixed), z-index and opacity. The result is a flat tree the renderer
 * walks. Layout-related CSS properties are the ones that matter here -
 * see README-css.md. */

#include "layout/layout.h"

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

namespace {

/* --- tiny value helpers (kept local so minimal target builds without
 *      style.cpp) --- */

std::string get(const WhaleUIComputedStyle& s, const char* k)
{
    auto it = s.find(k);
    return it == s.end() ? std::string() : it->second;
}

/* "12px"/"1.5em"/"50%"/"auto" -> px value relative to parent/base.
 * unit 0=px 1=% 2=em 4=unitless. Returns value in px. */
float len_px(const std::string& v, float base_px, float em_base)
{
    if (v.empty() || v == "auto" || v == "none") {
        return 0;
    }
    char* end = nullptr;
    float n = std::strtof(v.c_str(), &end);
    if (end == v.c_str()) {
        return 0;
    }
    if (*end == '%') {
        return n * base_px / 100.0f;
    }
    if (end[0] == 'e' && end[1] == 'm') {
        return n * em_base;
    }
    return n; /* px or unitless */
}

/* parse a length that may be "auto" (out_auto set) */
float len_or_auto(const std::string& v, float base_px, float em_base, bool* is_auto)
{
    if (v == "auto" || v.empty()) {
        *is_auto = true;
        return 0;
    }
    *is_auto = false;
    return len_px(v, base_px, em_base);
}

/* margin/padding/border-width shorthand: 1-4 values */
void sides(const std::string& v, int out[4], float base_px, float em_base, int def)
{
    for (int i = 0; i < 4; ++i) {
        out[i] = def;
    }
    if (v.empty()) {
        return;
    }
    std::vector<std::string> toks;
    const char* p = v.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char* s = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
        toks.push_back(std::string(s, static_cast<size_t>(p - s)));
    }
    float vals[4] = {0, 0, 0, 0};
    size_t n = toks.size();
    if (n == 1) {
        vals[0] = vals[1] = vals[2] = vals[3] = len_px(toks[0], base_px, em_base);
    } else if (n == 2) {
        vals[0] = vals[2] = len_px(toks[0], base_px, em_base);
        vals[1] = vals[3] = len_px(toks[1], base_px, em_base);
    } else if (n == 3) {
        vals[0] = len_px(toks[0], base_px, em_base);
        vals[1] = vals[3] = len_px(toks[1], base_px, em_base);
        vals[2] = len_px(toks[2], base_px, em_base);
    } else if (n >= 4) {
        for (int i = 0; i < 4; ++i) {
            vals[i] = len_px(toks[i], base_px, em_base);
        }
    }
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<int>(vals[i]);
    }
}

int border_width(const std::string& v, float em_base)
{
    if (v.empty() || v == "none" || v == "0") {
        return 0;
    }
    return static_cast<int>(len_px(v, 0, em_base));
}

/* --- display / position kinds --- */

int display_kind(const std::string& d)
{
    if (d == "none") {
        return 0;
    }
    if (d == "flex" || d == "inline-flex") {
        return 1;
    }
    if (d == "inline" || d == "inline-block") {
        return 2;
    }
    return 3; /* block (default) */
}

/* flex-grow, honoring the "flex: <grow> ..." shorthand */
float flex_grow(const WhaleUIComputedStyle& s)
{
    std::string g = get(s, "flex-grow");
    if (!g.empty()) {
        return std::strtof(g.c_str(), nullptr);
    }
    std::string f = get(s, "flex");
    if (!f.empty()) {
        return std::strtof(f.c_str(), nullptr);
    }
    return 0;
}

/* rough content width of a node: summed direct text runs + padding.
 * Used to size auto-width flex row items so they don't collapse to 1px. */
float estimate_content_width(whaleui_layout_node_t* k, float em)
{
    float fs = len_px(get(k->style, "font-size"), 0, em);
    if (fs <= 0) {
        fs = em;
    }
    float w = 0;
    for (whaleui_layout_node_t* c = k->first_child; c; c = c->next) {
        if (c->is_text) {
            w += static_cast<float>(c->text.size()) * fs * 0.5f;
        }
    }
    /* padding left/right */
    std::string p = get(k->style, "padding");
    if (!p.empty()) {
        float base = 0;
        float l = len_px(p, base, em);
        w += l * 2;
    }
    return w;
}

int position_kind(const std::string& p)
{
    if (p == "absolute") {
        return 1;
    }
    if (p == "fixed") {
        return 2;
    }
    if (p == "relative") {
        return 3;
    }
    return 0; /* static */
}

/* collect a text run (concatenated text children) for the renderer */
std::string collect_text(lxb_dom_element* el)
{
    std::string out;
    lxb_dom_node* n = el->node.first_child;
    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
            const lexbor_str_t* s = &lxb_dom_interface_text(n)->char_data.data;
            if (s->data) {
                out.append(reinterpret_cast<const char*>(s->data), s->length);
            }
        }
        n = n->next;
    }
    return out;
}

struct Builder
{
    whaleui_layout_tree_t* tree;
    const whaleui_css_rule_t* rules;
    size_t rule_count;
    std::map<std::string, std::string> vars;
    lxb_dom_element* hover_el;

    whaleui_layout_node_t* new_node()
    {
        tree->arena.emplace_back();
        return &tree->arena.back();
    }

    /* build node + children; returns the node (display:none still built) */
    whaleui_layout_node_t* build(lxb_dom_element* el, whaleui_layout_node_t* parent)
    {
        whaleui_layout_node_t* n = new_node();
        /* scalars reset here; style/text are STL containers (default-
         * constructed by new_node) and must NOT be memset */
        n->el = el;
        n->parent = parent;
        n->z = 0;
        n->opacity = 1.0f;
        n->visible = 1;
        n->is_text = 0;
        std::memset(&n->border, 0, sizeof(n->border));
        std::memset(&n->content, 0, sizeof(n->content));
        std::memset(n->margin, 0, sizeof(n->margin));
        std::memset(n->padding, 0, sizeof(n->padding));
        std::memset(n->border_w, 0, sizeof(n->border_w));

        n->style = whaleui_style_compute(el, rules, rule_count, vars, hover_el);

        /* inherit font-size / color / font-family from parent */
        if (parent) {
            if (n->style.find("font-size") == n->style.end() &&
                parent->style.find("font-size") != parent->style.end()) {
                n->style["font-size"] = parent->style["font-size"];
            }
            if (n->style.find("color") == n->style.end() &&
                parent->style.find("color") != parent->style.end()) {
                n->style["color"] = parent->style["color"];
            }
            if (n->style.find("font-family") == n->style.end() &&
                parent->style.find("font-family") != parent->style.end()) {
                n->style["font-family"] = parent->style["font-family"];
            }
        }

        std::string disp = get(n->style, "display");
        if (display_kind(disp) == 0) {
            n->visible = 0;
        }
        std::string zs = get(n->style, "z-index");
        if (!zs.empty()) {
            n->z = std::atoi(zs.c_str());
        }
        std::string op = get(n->style, "opacity");
        if (!op.empty()) {
            float o = std::strtof(op.c_str(), nullptr);
            if (o < 0) {
                o = 0;
            }
            if (o > 1) {
                o = 1;
            }
            n->opacity = o;
        }
        if (parent) {
            n->opacity *= parent->opacity;
        }

        /* text run (if any text children) */
        std::string txt = collect_text(el);
        if (!txt.empty() && n->visible) {
            tree->text_arena.push_back(txt);
            whaleui_layout_node_t* t = new_node();
            t->el = el;
            t->parent = n;
            t->is_text = 1;
            t->visible = 1;
            t->opacity = n->opacity;
            t->text = tree->text_arena.back();
            t->style = n->style;
            if (!n->first_child) {
                n->first_child = t;
            } else {
                whaleui_layout_node_t* last = n->first_child;
                while (last->next) {
                    last = last->next;
                }
                last->next = t;
            }
        }

        /* element children */
        whaleui_layout_node_t** link = &n->first_child;
        while (*link && (*link)->next) {
            link = &(*link)->next;
        }
        lxb_dom_node* c = el->node.first_child;
        while (c) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                whaleui_layout_node_t* child = build(lxb_dom_interface_element(c), n);
                if (!*link) {
                    *link = child;
                } else {
                    (*link)->next = child;
                }
                link = &child->next;
            }
            c = c->next;
        }
        return n;
    }

    /* --- layout pass --- */

    void layout(whaleui_layout_node_t* n, int cx, int cy, int cw, int ch,
                int font_px, int* cursor_y)
    {
        if (!n->visible) {
            return;
        }
        bool text_run = n->is_text != 0;
        float em = font_px > 0 ? static_cast<float>(font_px) : 16.0f;

        if (text_run) {
            /* inline text: width approximated, real metrics at render time */
            int fs = static_cast<int>(len_px(get(n->style, "font-size"), 0, em));
            if (fs <= 0) {
                fs = font_px > 0 ? font_px : 16;
            }
            n->border.x = cx;
            n->border.y = *cursor_y;
            n->border.w = static_cast<int>(n->text.size() * fs * 0.5f);
            n->border.h = static_cast<int>(fs * 1.2f);
            n->content = n->border;
            *cursor_y += n->border.h;
            return;
        }

        /* margins/padding/border */
        int m[4], p[4];
        sides(get(n->style, "margin"), m, static_cast<float>(cw), em, 0);
        sides(get(n->style, "padding"), p, static_cast<float>(cw), em, 0);
        static const char* kBorderWidth[] = {
            "border-top-width", "border-right-width",
            "border-bottom-width", "border-left-width",
        };
        for (int i = 0; i < 4; ++i) {
            n->margin[i] = m[i];
            n->padding[i] = p[i];
            n->border_w[i] = border_width(get(n->style, kBorderWidth[i]), em);
        }
        /* border shorthand "border: 1px solid red" / border-width */
        std::string border = get(n->style, "border");
        if (!border.empty()) {
            char* end = nullptr;
            float bw = std::strtof(border.c_str(), &end);
            if (end != border.c_str()) {
                for (int i = 0; i < 4; ++i) {
                    n->border_w[i] = static_cast<int>(bw);
                }
            }
        }
        std::string bw_all = get(n->style, "border-width");
        if (!bw_all.empty()) {
            int bw4[4];
            sides(bw_all, bw4, static_cast<float>(cw), em, 0);
            for (int i = 0; i < 4; ++i) {
                n->border_w[i] = bw4[i];
            }
        }

        /* width/height */
        bool w_auto = true, h_auto = true;
        float wpx = len_or_auto(get(n->style, "width"), static_cast<float>(cw), em, &w_auto);
        float hpx = len_or_auto(get(n->style, "height"), static_cast<float>(ch), em, &h_auto);
        std::string box_sizing = get(n->style, "box-sizing");
        bool border_box = box_sizing == "border-box";

        /* position */
        int pkind = position_kind(get(n->style, "position"));
        float off_top = len_px(get(n->style, "top"), static_cast<float>(ch), em);
        float off_left = len_px(get(n->style, "left"), static_cast<float>(cw), em);

        int x = cx, y = *cursor_y;
        int avail_w = cw;
        if (pkind == 1 || pkind == 2) {
            /* absolute/fixed: relative to viewport (parent content ignored) */
            x = cx + static_cast<int>(off_left);
            y = cy + static_cast<int>(off_top);
            avail_w = cw;
        } else if (pkind == 3) {
            x = cx + static_cast<int>(off_left);
            y = *cursor_y + static_cast<int>(off_top);
        }

        int my = m[0] + m[2], mx = m[1] + m[3];

        /* box width */
        int bw;
        if (w_auto) {
            if (display_kind(get(n->style, "display")) == 2) {
                /* inline / inline-block shrink to content */
                bw = static_cast<int>(estimate_content_width(n, em)) + mx;
            } else {
                bw = avail_w - mx;
            }
        } else {
            bw = static_cast<int>(wpx);
            if (!border_box) {
                /* content-box: width is the content width, add padding/border */
                bw += p[1] + p[3] + n->border_w[1] + n->border_w[3];
            }
        }
        if (bw < 0) {
            bw = 0;
        }
        n->border.x = x + m[1];
        n->border.y = y + m[0];
        n->border.w = bw;
        n->content.x = n->border.x + p[1] + n->border_w[1];
        n->content.y = n->border.y + p[0] + n->border_w[0];

        /* children */
        int inner_w = bw - p[1] - p[3] - n->border_w[1] - n->border_w[3];
        int inner_h = static_cast<int>(hpx);
        int kid_cursor = 0;
        int dk = display_kind(get(n->style, "display"));

        /* position:absolute children search the nearest positioned ancestor;
         * simplified: any positioned ancestor. */
        if (dk == 1) {
            layout_flex(n, inner_w, inner_h, font_px, kid_cursor);
        } else {
            layout_block(n, inner_w, inner_h, font_px, kid_cursor);
        }

        /* height: explicit or content height */
        int bh;
        if (!h_auto) {
            bh = static_cast<int>(hpx);
            if (!border_box) {
                /* content-box: height is the content height */
                bh += p[0] + p[2] + n->border_w[0] + n->border_w[2];
            }
            if (bh < kid_cursor) {
                bh = kid_cursor; /* content can overflow; keep explicit h */
            }
        } else {
            bh = kid_cursor + p[0] + p[2] + n->border_w[0] + n->border_w[2];
        }
        n->border.h = bh;
        n->content.w = n->border.w - p[1] - p[3] - n->border_w[1] - n->border_w[3];
        n->content.h = bh - p[0] - p[2] - n->border_w[0] - n->border_w[2];

        /* a <select> always reserves enough height for its value text */
        if (n->el) {
            size_t tlen = 0;
            const lxb_char_t* tname = lxb_dom_element_local_name(n->el, &tlen);
            if (tname && tlen == 6 && std::memcmp(tname, "select", 6) == 0 &&
                n->border.h < 28) {
                n->border.h = 28;
                n->content.h = 28 - p[0] - p[2] - n->border_w[0] - n->border_w[2];
            }
        }

        /* min/max */
        std::string mn = get(n->style, "min-width");
        std::string mw = get(n->style, "max-width");
        if (!mn.empty() && n->border.w < static_cast<int>(len_px(mn, static_cast<float>(cw), em))) {
            n->border.w = static_cast<int>(len_px(mn, static_cast<float>(cw), em));
        }
        if (!mw.empty() && n->border.w > static_cast<int>(len_px(mw, static_cast<float>(cw), em))) {
            n->border.w = static_cast<int>(len_px(mw, static_cast<float>(cw), em));
        }
        std::string mnh = get(n->style, "min-height");
        std::string mxh = get(n->style, "max-height");
        if (!mnh.empty() && n->border.h < static_cast<int>(len_px(mnh, static_cast<float>(ch), em))) {
            n->border.h = static_cast<int>(len_px(mnh, static_cast<float>(ch), em));
        }
        if (!mxh.empty() && n->border.h > static_cast<int>(len_px(mxh, static_cast<float>(ch), em))) {
            n->border.h = static_cast<int>(len_px(mxh, static_cast<float>(ch), em));
        }

        /* advance flow cursor for block flow (static/relative) */
        if (pkind == 0 || pkind == 3) {
            *cursor_y = y + m[0] + n->border.h + m[2];
        }
        if (cursor_y && pkind == 0) {
            /* nothing extra */
        }
    }

    void layout_block(whaleui_layout_node_t* n, int inner_w, int inner_h,
                      int font_px, int& kid_cursor)
    {
        (void)inner_h;
        /* block flow: children stack below each other, starting at the
         * container's content origin (padding/border are already excluded
         * via content.y) */
        int cursor = n->content.y;
        whaleui_layout_node_t* c = n->first_child;
        while (c) {
            layout(c, n->content.x, n->content.y, inner_w, inner_h,
                   font_px, &cursor);
            c = c->next;
        }
        kid_cursor = cursor - n->content.y;
    }

    void layout_flex(whaleui_layout_node_t* n, int inner_w, int inner_h,
                     int font_px, int& kid_cursor)
    {
        /* collect visible children */
        std::vector<whaleui_layout_node_t*> kids;
        whaleui_layout_node_t* c = n->first_child;
        while (c) {
            if (c->visible) {
                kids.push_back(c);
            }
            c = c->next;
        }
        if (kids.empty()) {
            return;
        }
        std::string dir = get(n->style, "flex-direction");
        bool column = dir == "column" || dir == "column-reverse";
        std::string justify = get(n->style, "justify-content");
        std::string align = get(n->style, "align-items");
        float gap = len_px(get(n->style, "gap"), static_cast<float>(inner_w), font_px > 0 ? font_px : 16);
        float em = font_px > 0 ? static_cast<float>(font_px) : 16;

        /* measure each child's main-axis size (min: content) */
        std::vector<int> main_size(kids.size(), 0);
        for (size_t i = 0; i < kids.size(); ++i) {
            whaleui_layout_node_t* k = kids[i];
            float fs = len_px(get(k->style, "font-size"), 0, em);
            if (fs <= 0) {
                fs = font_px > 0 ? font_px : 16;
            }
            bool is_auto = true;
            if (column) {
                float h = len_or_auto(get(k->style, "height"), static_cast<float>(inner_h), em, &is_auto);
                if (is_auto) {
                    h = estimate_content_width(k, em); /* rough height proxy */
                }
                main_size[i] = k->is_text ? static_cast<int>(k->text.size() * fs * 0.5f)
                                          : static_cast<int>(h);
            } else {
                float w = len_or_auto(get(k->style, "width"), static_cast<float>(inner_w), em, &is_auto);
                if (is_auto) {
                    w = estimate_content_width(k, em);
                }
                /* min-width caps the measurement so space-between doesn't
                 * push items past the container edge */
                std::string mnw = get(k->style, "min-width");
                if (!mnw.empty()) {
                    float m = len_px(mnw, static_cast<float>(inner_w), em);
                    if (w < m) {
                        w = m;
                    }
                }
                main_size[i] = k->is_text ? static_cast<int>(k->text.size() * fs * 0.5f)
                                          : static_cast<int>(w);
            }
            if (is_auto && main_size[i] == 0) {
                main_size[i] = 1; /* avoid zero-size main axis items */
            }
        }

        /* gap spacing */
        float total_gap = gap * (kids.size() > 0 ? kids.size() - 1 : 0);
        float total_main = 0;
        for (size_t i = 0; i < kids.size(); ++i) {
            total_main += main_size[i];
        }
        float free = (column ? inner_h : inner_w) - total_main - total_gap;
        if (free < 0) {
            free = 0;
        }
        /* flex-grow distributes free space */
        float grow_sum = 0;
        for (size_t i = 0; i < kids.size(); ++i) {
            float g = flex_grow(kids[i]->style);
            if (g > 0) {
                grow_sum += g;
            }
        }
        float extra = 0;
        if (grow_sum > 0 && free > 0) {
            extra = free / grow_sum;
        }

        /* leading space from justify-content */
        float lead = 0;
        float between = gap; /* spacing between items (space-between) */
        if (justify == "center") {
            lead = free / 2;
        } else if (justify == "flex-end" || justify == "end") {
            lead = free;
        } else if (justify == "space-between") {
            lead = 0;
            between = kids.size() > 1 ? free / (kids.size() - 1) : 0;
        } else if (justify == "space-around") {
            lead = kids.size() ? free / (kids.size() * 2) : 0;
        }

        float pos = lead;
        for (size_t i = 0; i < kids.size(); ++i) {
            whaleui_layout_node_t* k = kids[i];
            float grow = flex_grow(k->style);
            int sz = main_size[i] + (grow > 0 ? static_cast<int>(extra * grow) : 0);
            if (column) {
                int c = n->content.y + static_cast<int>(pos);
                layout(k, n->content.x, n->content.y, inner_w, sz, font_px, &c);
                /* advance by the item's ACTUAL box (auto-height content grows
                 * beyond the measured main size) */
                pos = (k->border.y + k->border.h) - n->content.y + between;
            } else {
                int c = n->content.y;
                layout(k, n->content.x + static_cast<int>(pos), n->content.y,
                       sz, inner_h, font_px, &c);
                pos = (k->border.x + k->border.w) - n->content.x + between;
                /* align-items: stretch (default) / center / flex-end */
                if (align == "center") {
                    k->border.y += (inner_h - k->border.h) / 2;
                } else if (align == "flex-end" || align == "end") {
                    k->border.y += inner_h - k->border.h;
                } else if (inner_h > 0) {
                    /* stretch: only when the container has a definite height */
                    k->border.h = inner_h;
                }
            }
        }
        if (column) {
            kid_cursor = static_cast<int>(pos - between);
        } else {
            /* row: the container height is the tallest item */
            int maxh = 0;
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kids[i]->border.h > maxh) {
                    maxh = kids[i]->border.h;
                }
            }
            kid_cursor = maxh;
        }
    }
};

} // namespace

extern "C" whaleui_layout_tree_t* whaleui_layout_compute(
    whaleui_dom_document_t* doc,
    const whaleui_css_rule_t* rules, size_t count,
    const std::map<std::string, std::string>* theme_vars,
    int viewport_w, int viewport_h,
    lxb_dom_element* hover_el)
{
    if (!doc || viewport_w <= 0 || viewport_h <= 0) {
        return nullptr;
    }
    lxb_html_document* hd = reinterpret_cast<lxb_html_document*>(doc);
    lxb_dom_element* root = lxb_dom_document_element(&hd->dom_document);
    if (!root) {
        return nullptr;
    }

    whaleui_layout_tree_t* tree = new whaleui_layout_tree_t;
    tree->viewport_w = viewport_w;
    tree->viewport_h = viewport_h;

    Builder b;
    b.tree = tree;
    b.rules = rules;
    b.rule_count = count;
    b.hover_el = hover_el;
    if (theme_vars) {
        b.vars = *theme_vars;
    }
    whaleui_style_collect_vars_full(root, rules, count, b.vars);

    whaleui_layout_node_t* n = b.build(root, nullptr);
    tree->root = n;

    /* layout the html element into the viewport */
    int cursor = 0;
    b.layout(n, 0, 0, viewport_w, viewport_h, 16, &cursor);
    /* the root element spans the whole viewport so its background covers the
     * window even when the content is shorter than the viewport */
    n->border.h = viewport_h;
    n->content.h = viewport_h;
    return tree;
}

extern "C" void whaleui_layout_destroy(whaleui_layout_tree_t* tree)
{
    delete tree;
}
