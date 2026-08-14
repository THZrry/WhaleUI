// test_layout: box model + block flow + basic flex.
#include "whaleui.h"
#include "layout/layout.h"

#include <lexbor/dom/dom.h>

#include <cassert>
#include <cstring>

namespace {

whaleui_layout_tree_t* do_layout(const char* html, int w, int h)
{
    whaleui_dom_document_t* doc = whaleui_dom_parse_html(html, std::strlen(html));
    assert(doc != nullptr);
    return whaleui_layout_compute(doc, nullptr, 0, nullptr, w, h, nullptr, nullptr, nullptr);
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
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr);
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
                                   nullptr, nullptr, nullptr);
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
                                   nullptr, nullptr, nullptr);
        assert(t != nullptr);
        d = find_tag(t->root, "div");
        assert(d != nullptr);
        assert(d->border.w == 100);
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
            doc, nullptr, 0, nullptr, 800, 600, nullptr, nullptr, nullptr);
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
            doc, nullptr, 0, nullptr, 800, 600, nullptr, &scrolls, nullptr);
        assert(t2 != nullptr);
        whaleui_layout_node_t* sc2 = find_tag(t2->root, "div");
        assert(sc2 != nullptr);
        whaleui_layout_node_t* inner2 = sc2->first_child;
        assert(inner2 != nullptr);
        assert(inner2->border.y == sc2->content.y - 50);
        /* scrolling the max amount reaches the content end */
        scrolls[sc2->el] = sc2->scroll_max;
        whaleui_layout_tree_t* t3 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 800, 600, nullptr, &scrolls, nullptr);
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
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr);
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
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr);
        assert(t != nullptr);
        scrolls[find_tag(t->root, "div")->el] = 30;
        whaleui_layout_tree_t* t1 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, &scrolls, nullptr);
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
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr);
        assert(t != nullptr);
        assert(t->root->scroll_max == 500); /* 800 content - 300 viewport */
        whaleui_layout_destroy(t);

        /* scrolling the page shifts the root's children up */
        whaleui_layout_tree_t* t0 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, nullptr, nullptr);
        assert(t0 != nullptr);
        scrolls.clear();
        scrolls[t0->root->el] = 200;
        whaleui_layout_tree_t* t1 = whaleui_layout_compute(
            doc, nullptr, 0, nullptr, 400, 300, nullptr, &scrolls, nullptr);
        assert(t1 != nullptr);
        assert(t1->root->scroll_max == 500);
        whaleui_layout_node_t* d = find_tag(t1->root, "div");
        assert(d != nullptr);
        assert(d->border.y == t1->root->content.y - 200);
        whaleui_layout_destroy(t1);
        whaleui_layout_destroy(t0);
        whaleui_dom_document_destroy(doc);
    }

    return 0;
}
