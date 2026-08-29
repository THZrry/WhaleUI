// test_layout: box model + block flow + basic flex.
#include "whaleui.h"
#include "dom/dom.h"
#include "layout/layout.h"
#include "style/theme.h"

#include <lexbor/dom/dom.h>

#include <cassert>
#include <cstring>
#include <vector>

namespace {

whaleui_layout_tree_t* do_layout(const char* html, int w, int h)
{
    whaleui_dom_document_t* doc = whaleui_dom_parse_html(html, std::strlen(html));
    assert(doc != nullptr);
    return whaleui_layout_compute(doc, nullptr, 0, nullptr, w, h, nullptr, nullptr, nullptr, 1.0f);
}

/* find first element node with the given tag, depth-first */
whaleui_layout_node_t* find_tag(whaleui_layout_node_t* n, const char* tag)
{
    if (!n) {
        return nullptr;
    }
    if (!n->is_text && n->el) {
        size_t len = 0;
        const lxb_char_t* name = lxb_dom_element_local_name(n->el, &len);
        if (name && len == std::strlen(tag) && std::memcmp(name, tag, len) == 0) {
            return n;
        }
    }
    for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
        whaleui_layout_node_t* r = find_tag(c, tag);
        if (r) {
            return r;
        }
    }
    return nullptr;
}

} // namespace

int main(void)
{
    /* block flow: two stacked divs with margin */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<body><div id=\"a\" style=\"width:100px;height:50px;margin:10px;\"></div>"
            "<div id=\"b\" style=\"width:80px;height:30px;\"></div></body>", 800, 600);
        assert(t != nullptr);
        whaleui_layout_node_t* a = find_tag(t->root, "div");
        assert(a != nullptr);
        assert(a->border.x == 10 && a->border.y == 10);
        assert(a->border.w == 100 && a->border.h == 50);
        whaleui_layout_node_t* b = a->next;
        assert(b != nullptr);
        /* b sits below a + a's margins */
        assert(b->border.y == 10 + 50 + 10);
        assert(b->border.x == 0);
        whaleui_layout_destroy(t);
    }

    /* width percentage + box-sizing border-box */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div id=\"p\" style=\"width:50%;height:40px;padding:10px;"
            "border:2px solid black;\"></div>", 400, 300);
        assert(t != nullptr);
        whaleui_layout_node_t* p = find_tag(t->root, "div");
        assert(p != nullptr);
        /* 50% of 400 = 200 content width; content-box default adds
         * padding(20) + border(4) to the border box */
        assert(p->border.w == 224);
        assert(p->content.w == 200);
        assert(p->border.h == 40 + 24);
        whaleui_layout_destroy(t);

        t = do_layout(
            "<div id=\"q\" style=\"box-sizing:border-box;width:200px;height:40px;"
            "padding:10px;border:2px solid black;\"></div>", 400, 300);
        assert(t != nullptr);
        whaleui_layout_node_t* q = find_tag(t->root, "div");
        assert(q != nullptr);
        assert(q->border.w == 200);
        assert(q->content.w == 200 - 20 - 4);
        whaleui_layout_destroy(t);
    }

    /* flex row: three items, gap + justify-content: center */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div id=\"f\" style=\"display:flex;width:300px;height:50px;"
            "gap:10px;justify-content:center;\">"
            "<span style=\"width:50px;\">1</span>"
            "<span style=\"width:50px;\">2</span>"
            "<span style=\"width:50px;\">3</span></div>", 800, 600);
        assert(t != nullptr);
        whaleui_layout_node_t* f = find_tag(t->root, "div");
        assert(f != nullptr);
        assert(f->first_child != nullptr);
        /* free = 300 - 150 - 20 = 130; lead = 65 */
        whaleui_layout_node_t* s1 = f->first_child;
        assert(s1 != nullptr);
        assert(s1->border.x == 65);
        assert(s1->border.w == 50);
        whaleui_layout_node_t* s2 = s1->next;
        assert(s2 != nullptr);
        assert(s2->border.x == 65 + 50 + 10);
        whaleui_layout_destroy(t);
    }

    /* flex column */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div id=\"fc\" style=\"display:flex;flex-direction:column;"
            "width:100px;height:120px;gap:10px;\">"
            "<span style=\"height:20px;\">1</span>"
            "<span style=\"height:20px;\">2</span></div>", 800, 600);
        assert(t != nullptr);
        whaleui_layout_node_t* fc = find_tag(t->root, "div");
        assert(fc != nullptr);
        whaleui_layout_node_t* s1 = fc->first_child;
        assert(s1 != nullptr);
        assert(s1->border.y == 0);
        whaleui_layout_node_t* s2 = s1->next;
        assert(s2 != nullptr);
        assert(s2->border.y == 20 + 10);
        whaleui_layout_destroy(t);
    }

    /* display:none takes no space */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div id=\"x\" style=\"display:none;height:30px;\"></div>"
            "<div id=\"y\" style=\"height:20px;\"></div>", 800, 600);
        assert(t != nullptr);
        whaleui_layout_node_t* x = find_tag(t->root, "div");
        assert(x != nullptr && !x->visible);
        whaleui_layout_node_t* y = x->next;
        assert(y != nullptr);
        assert(y->visible);
        assert(y->border.y == 0);
        whaleui_layout_destroy(t);
    }

    /* position: relative offset */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div id=\"r\" style=\"position:relative;top:10px;left:5px;height:10px;\"></div>",
            800, 600);
        assert(t != nullptr);
        whaleui_layout_node_t* r = find_tag(t->root, "div");
        assert(r != nullptr);
        assert(r->border.x == 5 && r->border.y == 10);
        whaleui_layout_destroy(t);
    }

    /* flex column with auto-height items must not overlap (regression:
     * items were placed by their measured 1px height, stacking on top) */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div id=\"fc2\" style=\"display:flex;flex-direction:column;"
            "gap:10px;width:200px;\">"
            "<div class=\"a\"><p>one</p></div>"
            "<div class=\"b\"><p>two</p></div></div>", 400, 300);
        assert(t != nullptr);
        whaleui_layout_node_t* fc = find_tag(t->root, "div");
        /* first div = the flex container (find_tag returns it) */
        assert(fc != nullptr);
        whaleui_layout_node_t* a = fc->first_child;
        assert(a != nullptr);
        assert(a->border.h > 10); /* auto height from the <p> content */
        whaleui_layout_node_t* b = a->next;
        assert(b != nullptr);
        /* b starts below a's bottom + gap */
        assert(b->border.y >= a->border.y + a->border.h + 10);
        whaleui_layout_destroy(t);
    }

    /* block children start at the parent's content origin (padding respected) */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div class=\"card\" style=\"padding:16px 18px;\">"
            "<h2>Title</h2><p>body</p></div>", 400, 300);
        assert(t != nullptr);
        whaleui_layout_node_t* card = find_tag(t->root, "div");
        whaleui_layout_node_t* h2 = find_tag(t->root, "h2");
        assert(card != nullptr && h2 != nullptr);
        assert(card->content.y == card->border.y + 16); /* padding-top */
        assert(h2->border.y == card->content.y);        /* starts inside content */
        whaleui_layout_destroy(t);
    }

    /* opacity + z-index recorded */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div id=\"z\" style=\"z-index:5;opacity:0.5;height:10px;\"></div>", 800, 600);
        assert(t != nullptr);
        whaleui_layout_node_t* z = find_tag(t->root, "div");
        assert(z != nullptr);
        assert(z->z == 5);
        assert(z->opacity > 0.49f && z->opacity < 0.51f);
        whaleui_layout_destroy(t);
    }

    /* length math functions: min()/clamp()/vw resolve against the viewport */
    {
        /* min(720px, 88vw) on an 800px viewport: 88vw = 704 wins */
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><div style=\"width:min(720px,88vw)\"></div></body>",
            46);
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* d = find_tag(t->root, "div");
        assert(d != nullptr);
        assert(d->border.w == 704);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);

        /* clamp(100px, 50%, 300px): 50% = 400 clamps down to 300 */
        doc = whaleui_dom_parse_html(
            "<body><div style=\"width:clamp(100px,50%,300px)\"></div></body>",
            54);
        assert(doc != nullptr);
        t = whaleui_layout_compute(doc, nullptr, 0, nullptr, 800, 600,
                                   nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        d = find_tag(t->root, "div");
        assert(d != nullptr);
        assert(d->border.w == 300);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);

        /* clamp(100px, 50%, 300px) on a narrow viewport: 50% = 40px,
         * clamps up to 100px */
        doc = whaleui_dom_parse_html(
            "<body><div style=\"width:clamp(100px,50%,300px)\"></div></body>",
            54);
        assert(doc != nullptr);
        t = whaleui_layout_compute(doc, nullptr, 0, nullptr, 80, 600,
                                   nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        d = find_tag(t->root, "div");
        assert(d != nullptr);
        assert(d->border.w == 100);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* grid: fixed + fr tracks, gap, repeat(), whole-row span */
    {
        /* 300px container, columns "100px 1fr", gap 10 -> 100 + 190 */
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><div style=\"display:grid;grid-template-columns:100px 1fr;"
            "gap:10px;width:300px;\">"
            "<div id=\"a\"></div><div id=\"b\"></div></div></body>",
            119);
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* g = find_tag(t->root, "div");
        assert(g != nullptr);
        assert(g->border.w == 300);
        /* children of the grid: a at column 0, b at column 1 */
        whaleui_layout_node_t* a2 = g->first_child;
        assert(a2 != nullptr);
        whaleui_layout_node_t* b2 = a2->next;
        assert(b2 != nullptr);
        assert(a2->border.w == 100);
        assert(b2->border.x == g->content.x + 100 + 10);
        assert(b2->border.w == 190); /* 300 - 100 - 10 gap */
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);

        /* repeat(3, 1fr) splits the space evenly, no gap */
        doc = whaleui_dom_parse_html(
            "<body><div style=\"display:grid;grid-template-columns:"
            "repeat(3,1fr);width:300px;\">"
            "<div></div><div></div><div></div></div></body>",
            108);
        assert(doc != nullptr);
        t = whaleui_layout_compute(doc, nullptr, 0, nullptr, 800, 600,
                                   nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        g = find_tag(t->root, "div");
        assert(g != nullptr);
        whaleui_layout_node_t* c0 = g->first_child;
        assert(c0 != nullptr && c0->border.w == 100);
        whaleui_layout_node_t* c1 = c0->next;
        assert(c1 != nullptr && c1->border.x == g->content.x + 100);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);

        /* grid-column: 1/-1 spans the whole row */
        doc = whaleui_dom_parse_html(
            "<body><div style=\"display:grid;grid-template-columns:1fr 1fr;"
            "width:200px;\">"
            "<div style=\"grid-column:1/-1\"></div>"
            "<div></div><div></div></div></body>",
            133);
        assert(doc != nullptr);
        t = whaleui_layout_compute(doc, nullptr, 0, nullptr, 800, 600,
                                   nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        g = find_tag(t->root, "div");
        assert(g != nullptr);
        whaleui_layout_node_t* span = g->first_child;
        assert(span != nullptr);
        assert(span->border.w == 200); /* spans both columns */
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* overflow:auto with a fixed height scrolls: children shift up by
     * scroll_y, scroll_max = content height - visible height */
    {
        const char* html = "<body><div id=\"sc\" style=\"overflow:auto;"
            "height:50px;\"><div style=\"height:200px;\"></div></div></body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        std::map<lxb_dom_element*, int> scrolls;

        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* sc = find_tag(t->root, "div");
        assert(sc != nullptr);
        whaleui_layout_node_t* inner = sc->first_child;
        assert(inner != nullptr);
        assert(sc->border.h == 50);          /* fixed height, not grown */
        assert(sc->scroll_max == 150);       /* 200 content - 50 visible */
        assert(inner->border.y == sc->content.y);
        whaleui_layout_destroy(t);

        scrolls[sc->el] = 50;
        whaleui_layout_tree_t* t2 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, &scrolls, nullptr, 1.0f);
        assert(t2 != nullptr);
        whaleui_layout_node_t* sc2 = find_tag(t2->root, "div");
        assert(sc2 != nullptr);
        whaleui_layout_node_t* inner2 = sc2->first_child;
        assert(inner2 != nullptr);
        assert(inner2->border.y == sc2->content.y - 50);
        /* scrolling the max amount reaches the content end */
        scrolls[sc2->el] = sc2->scroll_max;
        whaleui_layout_tree_t* t3 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, &scrolls, nullptr, 1.0f);
        assert(t3 != nullptr);
        whaleui_layout_node_t* sc3 = find_tag(t3->root, "div");
        assert(sc3 != nullptr);
        whaleui_layout_node_t* inner3 = sc3->first_child;
        assert(inner3->border.y == sc3->content.y - sc3->scroll_max);
        whaleui_layout_destroy(t3);
        whaleui_layout_destroy(t2);
        whaleui_dom_document_destroy(doc);
    }

    /* overflow:hidden clips but does not scroll */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div id=\"h\" style=\"overflow:hidden;height:30px;\">"
            "<div style=\"height:100px;\"></div></div>", 800, 600);
        assert(t != nullptr);
        whaleui_layout_node_t* h = find_tag(t->root, "div");
        assert(h != nullptr);
        assert(h->scroll_max == 0);
        whaleui_layout_destroy(t);
    }

    /* scroll container with a multi-line text run: the run shifts up by
     * scroll_y (the demo scroll-box regression) */
    {
        const char* html = "<body><div id=\"sc\" style=\"overflow:auto;"
            "height:50px;\">line1<br>line2<br>line3<br>line4</div></body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        std::map<lxb_dom_element*, int> scrolls;

        whaleui_layout_tree_t* t0 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr, 1.0f);
        assert(t0 != nullptr);
        whaleui_layout_node_t* sc = find_tag(t0->root, "div");
        assert(sc != nullptr);
        assert(sc->scroll_max > 0); /* 4 lines overflow the 50px box */
        whaleui_layout_node_t* tr = sc->first_child;
        assert(tr != nullptr && tr->is_text);
        assert(tr->border.y == sc->content.y);
        whaleui_layout_destroy(t0);

        /* grab the element pointer from a throwaway layout pass */
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        scrolls[find_tag(t->root, "div")->el] = 30;
        whaleui_layout_tree_t* t1 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, &scrolls, nullptr, 1.0f);
        assert(t1 != nullptr);
        whaleui_layout_node_t* sc1 = find_tag(t1->root, "div");
        assert(sc1 != nullptr);
        whaleui_layout_node_t* tr1 = sc1->first_child;
        assert(tr1 != nullptr && tr1->is_text);
        assert(tr1->border.y == sc1->content.y - 30);
        assert(sc1->scroll_y == 30);
        whaleui_layout_destroy(t1);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* text runs: <br> becomes a line break; height scales with lines */
    {
        whaleui_layout_tree_t* t = do_layout(
            "<div style=\"width:100px;\">a<br>b<br>c</div>", 400, 300);
        assert(t != nullptr);
        whaleui_layout_node_t* d = find_tag(t->root, "div");
        assert(d != nullptr);
        whaleui_layout_node_t* tr = d->first_child;
        assert(tr != nullptr && tr->is_text);
        assert(tr->text == "a\nb\nc");
        /* 3 lines at the default 16px font (16 * 1.2 per line) */
        assert(tr->border.h == static_cast<int>(16 * 1.2f) * 3);
        whaleui_layout_destroy(t);
    }

    /* page scroll: the html root scrolls when content exceeds the viewport */
    {
        const char* html = "<body><div style=\"height:800px;\"></div></body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        std::map<lxb_dom_element*, int> scrolls;

        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        assert(t->root->scroll_max == 500); /* 800 content - 300 viewport */
        whaleui_layout_destroy(t);

        /* scrolling the page shifts the root's children up */
        whaleui_layout_tree_t* t0 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr, 1.0f);
        assert(t0 != nullptr);
        scrolls.clear();
        scrolls[t0->root->el] = 200;
        whaleui_layout_tree_t* t1 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, &scrolls, nullptr, 1.0f);
        assert(t1 != nullptr);
        assert(t1->root->scroll_max == 500);
        whaleui_layout_node_t* d = find_tag(t1->root, "div");
        assert(d != nullptr);
        assert(d->border.y == t1->root->content.y - 200);
        whaleui_layout_destroy(t1);
        whaleui_layout_destroy(t0);
        whaleui_dom_document_destroy(doc);
    }

    /* margin: 0 auto centers a fixed-width block in the viewport */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><div style=\"width:200px;margin:0 auto;\"></div></body>",
            57);
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* d = find_tag(t->root, "div");
        assert(d != nullptr);
        assert(d->border.x == 300); /* (800 - 200) / 2 */
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* grid auto-height estimates wrapped text, not the text width */
    {
        std::string txt(200, 'a'); /* long paragraph */
        std::string html = "<body><div style=\"display:grid;"
                           "grid-template-columns:100px 1fr;width:400px;\">"
                           "<p>" + txt + "</p></div></body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html.c_str(), html.size());
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* p = find_tag(t->root, "p");
        assert(p != nullptr);
        /* 200 chars @ ~8px avg in a ~300px column -> ~6 lines, ~110px;
         * using the text WIDTH as height would give 200*8 = 1600px */
        assert(p->border.h < 400);
        assert(p->border.h >= 60);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* ::after content is a separate text run (pseudo styles apply to it) */
    {
        const char* css = "a::after { content: ' X'; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        assert(whaleui_css_parse(&rules, &count, css, std::strlen(css)) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><a>click</a></body>", 24);
        assert(doc != nullptr);
        std::map<std::string, std::string> vars;
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, rules, count, &vars, 800, 600, nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* a = find_tag(t->root, "a");
        assert(a != nullptr);
        whaleui_layout_node_t* run = a->first_child;
        assert(run != nullptr && run->is_text);
        assert(run->text == "click");
        whaleui_layout_node_t* run2 = run->next;
        assert(run2 != nullptr && run2->is_text);
        assert(run2->text == " X");
        whaleui_layout_destroy(t);
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    /* position:fixed is viewport-relative and immune to ancestor scroll */
    {
        const char* html = "<body><div id=\"sc\" style=\"overflow:auto;"
            "height:100px;\">"
            "<div id=\"fx\" style=\"position:fixed;top:10px;left:20px;"
            "width:50px;\"></div>"
            "<div style=\"height:300px;\"></div></div></body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        std::map<lxb_dom_element*, int> scrolls;
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* fx = find_tag(t->root, "div");
        /* find the fixed div (deepest, id fx) */
        whaleui_layout_node_t* sc = nullptr;
        for (auto& nd : t->arena) {
            if (nd.el) {
                size_t len = 0;
                const lxb_char_t* id = lxb_dom_element_get_attribute(
                    nd.el, (const lxb_char_t*)"id", 2, &len);
                if (id && len == 2 && std::memcmp(id, "fx", 2) == 0) {
                    fx = &nd;
                }
                if (id && len == 2 && std::memcmp(id, "sc", 2) == 0) {
                    sc = &nd;
                }
            }
        }
        assert(fx != nullptr && sc != nullptr);
        assert(fx->border.x == 20 && fx->border.y == 10);
        /* scroll the container: fixed element keeps its viewport position */
        scrolls[sc->el] = 50;
        whaleui_layout_tree_t* t2 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, &scrolls, nullptr, 1.0f);
        assert(t2 != nullptr);
        whaleui_layout_node_t* fx2 = nullptr;
        for (auto& nd : t2->arena) {
            if (nd.el) {
                size_t len = 0;
                const lxb_char_t* id = lxb_dom_element_get_attribute(
                    nd.el, (const lxb_char_t*)"id", 2, &len);
                if (id && len == 2 && std::memcmp(id, "fx", 2) == 0) {
                    fx2 = &nd;
                }
            }
        }
        assert(fx2 != nullptr);
        assert(fx2->border.x == 20 && fx2->border.y == 10);
        whaleui_layout_destroy(t2);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* global text scale multiplies px font-size in the layout tree */
    {
        const char* html = "<body><div style=\"font-size:20px;\">x</div></body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* d = find_tag(t->root, "div");
        assert(d != nullptr);
        assert(d->style["font-size"] == "20px");
        whaleui_layout_destroy(t);
        /* 125% -> 25px */
        t = whaleui_layout_compute(doc, nullptr, 0, nullptr, 800, 600,
                                   nullptr, nullptr, nullptr, 1.25f);
        assert(t != nullptr);
        d = find_tag(t->root, "div");
        assert(d != nullptr);
        assert(d->style["font-size"] == "25px");
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* source indentation whitespace must not inflate block heights */
    {
        const char* html =
            "<body>\n  <div style=\"height:30px;\"></div>\n</body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* d = find_tag(t->root, "div");
        assert(d != nullptr);
        assert(d->border.h == 30);
        /* the blank newline text nodes produced no tall text run */
        assert(d->border.y < 40);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* grid row sizing a stacked cell (b + span) fits both lines */
    {
        const char* html = "<body><div style=\"display:grid;"
            "grid-template-columns:50px 1fr;\">"
            "<i>01</i><div><b>title</b><span>note text</span></div></div>"
            "</body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* g = find_tag(t->root, "div");
        assert(g != nullptr);
        /* the right cell holds two stacked text lines: taller than one */
        whaleui_layout_node_t* cell = g->first_child;
        while (cell && cell->next) {
            cell = cell->next;
        }
        assert(cell != nullptr && !cell->is_text);
        assert(cell->border.h >= 30);
        assert(g->border.h >= cell->border.h);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* flex-wrap: overflowing items wrap to a new row */
    {
        const char* html = "<body><div style=\"display:flex;flex-wrap:wrap;"
            "gap:8px;width:120px;\">"
            "<div style=\"width:60px;height:20px;\"></div>"
            "<div style=\"width:60px;height:20px;\"></div>"
            "<div style=\"width:60px;height:20px;\"></div></div></body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* g = find_tag(t->root, "div");
        assert(g != nullptr);
        /* three 60px items in a 120px container with 8px gap: 2 per row,
         * the third wraps -> total height ~= 2 rows */
        assert(g->border.h >= 44); /* 20 + 8 + 20 */
        whaleui_layout_node_t* c2 = g->first_child->next->next;
        assert(c2 != nullptr);
        assert(c2->border.y > g->first_child->border.y + 10);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* line-height: 2.0 makes a single text line taller than 1.2 default */
    {
        const char* html = "<body><div style=\"line-height:2.0;\">"
            "<span>text</span></div></body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* d = find_tag(t->root, "div");
        assert(d != nullptr);
        assert(d->border.h >= 30); /* 16px font × 2.0 = 32 */
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* <details> without the open attribute collapses to its first
     * <summary>; with open it expands */
    {
        const char* html =
            "<body><details><summary>more</summary><p>body</p></details>"
            "</body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        assert(find_tag(t->root, "details") != nullptr);
        assert(find_tag(t->root, "p") == nullptr); /* body hidden */
        whaleui_layout_node_t* sum = find_tag(t->root, "summary");
        assert(sum != nullptr);
        /* the summary's run carries the collapse marker (▸) */
        assert(sum->first_child && sum->first_child->is_text);
        assert(sum->first_child->text.rfind("\xe2\x96\xb8", 0) == 0);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);

        const char* open_html =
            "<body><details open><summary>more</summary><p>body</p>"
            "</details></body>";
        doc = whaleui_dom_parse_html(open_html, std::strlen(open_html));
        assert(doc != nullptr);
        t = whaleui_layout_compute(doc, nullptr, 0, nullptr, 800, 600,
                                   nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);
        assert(find_tag(t->root, "p") != nullptr); /* body visible */
        sum = find_tag(t->root, "summary");
        assert(sum != nullptr && sum->first_child);
        assert(sum->first_child->text.rfind("\xe2\x96\xbe", 0) == 0); /* ▾ */
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* inline elements flow on one line with their surrounding text (UA
     * default display), so <em>/<strong> do not break the line */
    {
        std::string css = whaleui_theme_default_css("fluent");
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        whaleui_css_keyframes_t kf;
        assert(whaleui_css_parse_full(css.c_str(), css.size(), &rules,
                                      &count, &kf) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><p>a <em>e</em> b</p></body>", 28);
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, rules, count, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* p = find_tag(t->root, "p");
        assert(p != nullptr);
        whaleui_layout_node_t* r1 = p->first_child;
        assert(r1 && r1->is_text && r1->text == "a ");
        whaleui_layout_node_t* em = r1->next;
        assert(em && !em->is_text);
        assert(em->border.y == r1->border.y); /* same line */
        assert(em->border.x == r1->border.x + r1->border.w);
        whaleui_layout_node_t* r2 = em->next;
        assert(r2 && r2->is_text && r2->text == "b");
        assert(r2->border.y == r1->border.y);
        assert(r2->border.x == em->border.x + em->border.w);
        whaleui_layout_destroy(t);
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    /* <ul>/<ol> items get engine-injected list markers in their first
     * text run: bullet for ul, ordinal for ol (browser list-style) */
    {
        const char* html =
            "<body><ul><li>a</li><li>b</li></ul>"
            "<ol><li>one</li><li>two</li></ol></body>";
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* li = find_tag(t->root, "li");
        assert(li != nullptr && li->first_child && li->first_child->is_text);
        assert(li->first_child->text.rfind("\xe2\x80\xa2", 0) == 0); /* • */
        /* second ul li also bulleted */
        whaleui_layout_node_t* li2 = li->next;
        assert(li2 != nullptr && li2->first_child);
        assert(li2->first_child->text.rfind("\xe2\x80\xa2", 0) == 0);
        /* first ol li: ordinal "1. " */
        whaleui_layout_node_t* ol = find_tag(t->root, "ol");
        assert(ol != nullptr);
        whaleui_layout_node_t* o1 = ol->first_child;
        assert(o1 != nullptr && o1->first_child);
        assert(o1->first_child->text.rfind("1. ", 0) == 0);
        whaleui_layout_node_t* o2 = o1->next;
        assert(o2 != nullptr && o2->first_child);
        assert(o2->first_child->text.rfind("2. ", 0) == 0);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* text-align: center/right shifts the whole inline line (mixed runs
     * center as one line box, not per run) */
    {
        std::string css = whaleui_theme_default_css("fluent");
        css += ".c { text-align: center; }\n.r { text-align: right; }\n";
        whaleui_css_rule_t* rules = nullptr;
        size_t count = 0;
        whaleui_css_keyframes_t kf;
        assert(whaleui_css_parse_full(css.c_str(), css.size(), &rules,
                                      &count, &kf) == 0);
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><div class=\"c\" style=\"width:400px;\">aa <em>e</em>"
            " bb</div><div class=\"r\" style=\"width:400px;\">x "
            "<strong>y</strong></div></body>", 0);
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, rules, count, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* cdiv = find_tag(t->root, "div");
        assert(cdiv != nullptr);
        whaleui_layout_node_t* r1 = cdiv->first_child;
        assert(r1 && r1->is_text);
        /* centered: first run starts right of the content origin */
        assert(r1->border.x > cdiv->content.x + 50);
        /* all three members share one line */
        whaleui_layout_node_t* em = r1->next;
        assert(em && !em->is_text);
        assert(em->border.y == r1->border.y);
        assert(em->border.x == r1->border.x + r1->border.w);
        /* right-aligned container: the line hugs the right edge */
        whaleui_layout_node_t* rdiv = cdiv->next;
        assert(rdiv != nullptr);
        whaleui_layout_node_t* rr = rdiv->first_child;
        assert(rr && rr->is_text);
        assert(rr->border.x > rdiv->content.x + 200);
        whaleui_layout_destroy(t);
        whaleui_css_rules_destroy(rules, count);
        whaleui_dom_document_destroy(doc);
    }

    /* incremental relayout: a DOM change rebuilds only the affected
     * subtree; the untouched sibling keeps its node (stable pointer) and
     * the box pass re-positions it below the resized block */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><div id=\"a\" style=\"width:100px;height:50px;\"></div>"
            "<div id=\"b\" style=\"width:80px;height:30px;\"></div></body>",
            std::strlen(
                "<body><div id=\"a\" style=\"width:100px;height:50px;\"></div>"
                "<div id=\"b\" style=\"width:80px;height:30px;\"></div></body>"));
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        whaleui_layout_node_t* a = find_tag(t->root, "div");
        assert(a != nullptr);
        whaleui_layout_node_t* b = a->next;
        assert(b != nullptr && b->el != a->el);
        assert(b->border.y == 50); /* below a's original 50px height */

        /* grow a via the DOM API: it lands in the per-document dirty set */
        whaleui_dom_set_style(reinterpret_cast<whaleui_dom_element_t*>(a->el),
                              "height", "100px");
        std::vector<lxb_dom_element*> dirty;
        whaleui_dom_take_dirty(doc, dirty);
        assert(dirty.size() == 1 && dirty[0] == a->el);

        /* relayout: a's subtree is rebuilt, b keeps its node */
        assert(whaleui_layout_relayout(t, a->el, nullptr, 0, nullptr, nullptr,
                                       nullptr, nullptr, 1.0f) == 0);
        whaleui_layout_node_t* a2 = t->by_el[a->el];
        assert(a2 != nullptr && a2 != a);          /* fresh subtree */
        assert(a2->border.h == 100);               /* new height applied */
        assert(a2->next == b);                     /* sibling chain kept */
        assert(t->by_el[b->el] == b);              /* b untouched */
        assert(b->border.y == 100);                /* re-positioned below a */
        assert(whaleui_layout_relayout(t, b->el, nullptr, 0, nullptr, nullptr,
                                       nullptr, nullptr, 1.0f) == 0);
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* relayout of an element outside the tree is a no-op (rc == 1) */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(
            "<body><div style=\"width:10px;height:10px;\"></div></body>",
            std::strlen("<body><div style=\"width:10px;height:10px;\"></div></body>"));
        assert(doc != nullptr);
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr,
            1.0f);
        assert(t != nullptr);
        lxb_dom_element* alien = reinterpret_cast<lxb_dom_element*>(
            whaleui_dom_create_element(doc, "div"));
        assert(alien != nullptr);
        assert(whaleui_layout_relayout(t, alien, nullptr, 0, nullptr, nullptr,
                                       nullptr, nullptr, 1.0f) == 1);
        whaleui_dom_element_destroy(reinterpret_cast<whaleui_dom_element_t*>(alien));
        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* editable controls do not grow when text is typed: input/textarea
     * widths come from CSS (or the 12em default); a fixed-height textarea
     * keeps its height and scrolls multi-line content instead of being
     * stretched (browser UA behavior: textarea scrolls by default) */
    {
        const char* short_html =
            "<html><body><textarea style=\"width:200px;height:60px\">"
            "x</textarea></body></html>";
        const char* long_html =
            "<html><body><textarea style=\"width:200px;height:60px\">"
            "aaaaaaaaaa\nbbbbbbbbbb\ncccccccccc\ndddddddddd\neeeeeeeeee"
            "</textarea></body></html>";
        auto meas = [](const char* html, int* w, int* h, int* smax) {
            whaleui_dom_document_t* doc =
                whaleui_dom_parse_html(html, std::strlen(html));
            assert(doc != nullptr);
            whaleui_layout_tree_t* t = whaleui_layout_compute(
                doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr,
                nullptr, 1.0f);
            assert(t != nullptr);
            *w = *h = *smax = -1;
            for (auto& n : t->arena) {
                if (!n.visible || !n.el || n.is_text) {
                    continue;
                }
                size_t len = 0;
                const lxb_char_t* nm = lxb_dom_element_local_name(n.el, &len);
                if (nm && len == 8 && std::memcmp(nm, "textarea", 8) == 0) {
                    *w = n.border.w;
                    *h = n.border.h;
                    *smax = n.scroll_max;
                    break;
                }
            }
            whaleui_layout_destroy(t);
            whaleui_dom_document_destroy(doc);
        };
        int w1, h1, s1, w2, h2, s2;
        meas(short_html, &w1, &h1, &s1);
        meas(long_html, &w2, &h2, &s2);
        assert(w1 == w2 && w1 == 200);
        assert(h1 == h2 && h1 == 60); /* fixed height, not stretched */
        assert(s2 > 0);               /* multi-line content scrolls */
    }

    /* an auto-width inline-block with long editable text wraps: the width
     * follows the longest wrapped line (capped to the available width)
     * instead of stretching to the whole unwrapped text; the height grows
     * with the wrapped lines */
    {
        auto meas = [](const char* html, int* w, int* h) {
            whaleui_dom_document_t* doc =
                whaleui_dom_parse_html(html, std::strlen(html));
            assert(doc != nullptr);
            whaleui_layout_tree_t* t = whaleui_layout_compute(
                doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr,
                nullptr, 1.0f);
            assert(t != nullptr);
            *w = *h = -1;
            for (auto& n : t->arena) {
                if (!n.visible || !n.el || n.is_text) {
                    continue;
                }
                size_t len = 0;
                const lxb_char_t* nm = lxb_dom_element_local_name(n.el, &len);
                if (nm && len == 4 && std::memcmp(nm, "span", 4) == 0) {
                    *w = n.border.w;
                    *h = n.border.h;
                    break;
                }
            }
            whaleui_layout_destroy(t);
            whaleui_dom_document_destroy(doc);
        };
        int w1, h1, w2, h2;
        meas("<html><body><span style=\"display:inline-block\">x</span>"
             "</body></html>",
             &w1, &h1);
        meas("<html><body><span style=\"display:inline-block\">"
             "aaaaaaaaaa bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee "
             "ffffffffff gggggggggg hhhhhhhhhh</span></body></html>",
             &w2, &h2);
        assert(w1 > 0 && w1 < 400);
        assert(w2 <= 400);  /* capped to the available width, not stretched */
        assert(h2 > h1);    /* wrapped to multiple lines */
    }

    /* REPRO: editable controls' size when text is added/removed - does the
     * width follow the content (unwrapped) or stay CSS-defined? */
    {
        struct C
        {
            const char* name;
            const char* short_html;
            const char* long_html;
        };
        C cases[] = {
            {"textarea default",
             "<html><body><textarea>x</textarea></body></html>",
             "<html><body><textarea>aaaaaaaaaa bbbbbbbbbb cccccccccc "
             "dddddddddd eeeeeeeeee ffffffffff gggggggggg</textarea>"
             "</body></html>"},
            {"textarea w:auto",
             "<html><body><textarea style=\"width:auto\">x</textarea>"
             "</body></html>",
             "<html><body><textarea style=\"width:auto\">aaaaaaaaaa "
             "bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee ffffffffff "
             "gggggggggg</textarea></body></html>"},
            {"contenteditable span",
             "<html><body><span contenteditable=\"true\">x</span>"
             "</body></html>",
             "<html><body><span contenteditable=\"true\">aaaaaaaaaa "
             "bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee ffffffffff "
             "gggggggggg</span></body></html>"},
            {"contenteditable span w:auto",
             "<html><body><span contenteditable=\"true\" "
             "style=\"width:auto\">x</span></body></html>",
             "<html><body><span contenteditable=\"true\" "
             "style=\"width:auto\">aaaaaaaaaa bbbbbbbbbb cccccccccc "
             "dddddddddd eeeeeeeeee ffffffffff gggggggggg</span>"
             "</body></html>"},
            {"contenteditable div",
             "<html><body><div contenteditable=\"true\">x</div></body></html>",
             "<html><body><div contenteditable=\"true\">aaaaaaaaaa "
             "bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee ffffffffff "
             "gggggggggg</div></body></html>"},
            {"input default",
             "<html><body><input value=\"x\"></body></html>",
             "<html><body><input value=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\">"
             "</body></html>"},
            {"input w:auto",
             "<html><body><input style=\"width:auto\" value=\"x\">"
             "</body></html>",
             "<html><body><input style=\"width:auto\" "
             "value=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"></body></html>"},
        };
        auto find_ctrl = [](whaleui_layout_tree_t* t, const char* tag,
                            int* w, int* h) {
            for (auto& n : t->arena) {
                if (!n.visible || !n.el || n.is_text) {
                    continue;
                }
                size_t len = 0;
                const lxb_char_t* nm = lxb_dom_element_local_name(n.el, &len);
                if (nm && std::strlen(tag) == len &&
                    std::memcmp(nm, tag, len) == 0) {
                    *w = n.border.w;
                    *h = n.border.h;
                    return;
                }
            }
            *w = *h = -1;
        };
        for (auto& cs : cases) {
            const char* tag = std::strstr(cs.short_html, "<textarea")
                                  ? "textarea"
                                  : (std::strstr(cs.short_html, "<input")
                                         ? "input"
                                         : (std::strstr(cs.short_html,
                                                        "<div")
                                                ? "div"
                                                : "span"));
            int w1, h1, w2, h2;
            whaleui_layout_tree_t* t1 = do_layout(cs.short_html, 400, 300);
            whaleui_layout_tree_t* t2 = do_layout(cs.long_html, 400, 300);
            assert(t1 && t2);
            find_ctrl(t1, tag, &w1, &h1);
            find_ctrl(t2, tag, &w2, &h2);
            printf("[len] %-26s short(w%dh%d) long(w%dh%d) %s\n", cs.name,
                   w1, h1, w2, h2,
                   (w1 == w2 && h1 == h2)
                       ? "fixed"
                       : (w1 != w2 ? "<-- WIDTH changed"
                                   : "<-- HEIGHT changed"));
            whaleui_layout_destroy(t2);
            whaleui_layout_destroy(t1);
        }
    }

    return 0;
}
