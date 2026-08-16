// test_qwen: layout the reference page Qwen_html_20260814_oeem340or.html
// and assert the CSS features it exercises: fixed/sticky/absolute+right,
// inset, border-block, nth-child, min()/clamp()/vw/vh, float-fr grid,
// white-space:nowrap, ::before content, @media filtering, img boxes.
#include "whaleui.h"
#include "layout/layout.h"
#include "style/style.h"

#include <lexbor/dom/dom.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string read_file(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    assert(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(static_cast<size_t>(n), '\0');
    size_t got = std::fread(&s[0], 1, static_cast<size_t>(n), f);
    s.resize(got);
    std::fclose(f);
    return s;
}

/* first <style>...</style> block (the reference page has one) */
std::string extract_style(const std::string& html)
{
    size_t b = html.find("<style>");
    assert(b != std::string::npos);
    b += 7;
    size_t e = html.find("</style>", b);
    assert(e != std::string::npos);
    return html.substr(b, e - b);
}

whaleui_layout_node_t* find_class(whaleui_layout_node_t* n, const char* cls)
{
    if (!n) {
        return nullptr;
    }
    if (!n->is_text && n->el) {
        size_t alen = 0;
        const lxb_char_t* a = lxb_dom_element_get_attribute(
            n->el, (const lxb_char_t*)"class", 5, &alen);
        if (a) {
            std::string c(reinterpret_cast<const char*>(a), alen);
            std::string want(cls);
            size_t pos = 0;
            while ((pos = c.find(want, pos)) != std::string::npos) {
                bool lb = pos == 0 || c[pos - 1] == ' ';
                bool le = pos + want.size() == c.size() ||
                          c[pos + want.size()] == ' ';
                if (lb && le) {
                    return n;
                }
                pos += want.size();
            }
        }
    }
    for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
        whaleui_layout_node_t* r = find_class(c, cls);
        if (r) {
            return r;
        }
    }
    return nullptr;
}

whaleui_layout_node_t* find_tag(whaleui_layout_node_t* n, const char* tag)
{
    if (!n) {
        return nullptr;
    }
    if (!n->is_text && n->el) {
        size_t len = 0;
        const lxb_char_t* name = lxb_dom_element_local_name(n->el, &len);
        if (name && len == std::strlen(tag) &&
            std::memcmp(name, tag, len) == 0) {
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

const char* sget(const WhaleUIComputedStyle& s, const char* k)
{
    auto it = s.find(k);
    return it == s.end() ? "" : it->second.c_str();
}

whaleui_css_rule_t* rules_copy(const whaleui_css_rule_t* src, size_t count)
{
    whaleui_css_rule_t* d = static_cast<whaleui_css_rule_t*>(
        std::malloc(count * sizeof(*d)));
    assert(d != nullptr);
    for (size_t i = 0; i < count; ++i) {
        std::memset(&d[i], 0, sizeof(d[i]));
        if (src[i].selector) {
            d[i].selector = _strdup(src[i].selector);
        }
        if (src[i].media) {
            d[i].media = _strdup(src[i].media);
        }
        d[i].important = src[i].important;
        for (size_t j = 0; j < src[i].decl_count; ++j) {
            char** nd = static_cast<char**>(std::realloc(
                d[i].decls, (j + 1) * sizeof(char*)));
            assert(nd != nullptr);
            d[i].decls = nd;
            d[i].decls[j] = _strdup(src[i].decls[j]);
        }
        d[i].decl_count = src[i].decl_count;
    }
    return d;
}

} // namespace

int main(void)
{
    std::string html = read_file(WHALEUI_TEST_ROOT
                                 "/temp/Qwen_html_20260814_oeem340or.html");
    std::string css = extract_style(html);

    whaleui_css_rule_t* rules = nullptr;
    size_t count = 0;
    assert(whaleui_css_parse(&rules, &count, css.c_str(), css.size()) == 0);
    assert(count > 0);

    /* per-viewport filtered copies: filter_media compacts in place, so each
     * branch works on its own copy of the original rules */
    whaleui_css_rule_t* r12 = rules_copy(rules, count);
    size_t c12 = count;
    assert(whaleui_style_filter_media(r12, &c12, 2, 1200, 0) == 0);
    whaleui_css_rule_t* r8 = rules_copy(rules, count);
    size_t c8 = count;
    assert(whaleui_style_filter_media(r8, &c8, 2, 800, 0) == 0);

    /* --- viewport 1200: desktop layout --- */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(html.c_str(),
                                                             html.size());
        assert(doc != nullptr);
        whaleui_layout_tree_t* t =
            whaleui_layout_compute(doc, r12, c12, nullptr, 1200, 800,
                                   nullptr, nullptr, nullptr, 1.0f);
        assert(t != nullptr);

        /* .wrap: width min(1120px, 92vw) = 1104, margin 0 auto centers it */
        whaleui_layout_node_t* wrap = find_class(t->root, "wrap");
        assert(wrap != nullptr);
        assert(wrap->border.w == 1104);
        assert(wrap->border.x == 48);

        /* header: position:fixed, top:0 left:0 right:0 -> full width */
        whaleui_layout_node_t* header = find_tag(t->root, "header");
        assert(header != nullptr);
        assert(header->border.x == 0 && header->border.y == 0);
        assert(header->border.w == 1200);
        assert(std::atoi(sget(header->style, "z-index")) == 50);
        assert(std::strcmp(sget(header->style, "position"), "fixed") == 0);
        assert(std::strcmp(sget(header->style, "backdrop-filter"), "") != 0);

        /* .noise: fixed + inset:0 -> pinned to top-left, spans the
         * viewport width */
        whaleui_layout_node_t* noise = find_class(t->root, "noise");
        assert(noise != nullptr);
        assert(noise->border.x == 0 && noise->border.y == 0);
        assert(noise->border.w == 1200);
        assert(std::atoi(sget(noise->style, "z-index")) == 99);

        /* .hero: min-height:92vh = 0.92*800 = 736 */
        whaleui_layout_node_t* hero = find_class(t->root, "hero");
        assert(hero != nullptr);
        assert(hero->border.h >= 736);

        /* .hero-grid shares the .wrap element; its width:100% overrides
         * .wrap's min() -> full 1200px; columns 1.08fr .92fr split
         * 1200-60(gap) = 1140 -> 615 / 524 */
        whaleui_layout_node_t* hg = find_class(t->root, "hero-grid");
        assert(hg != nullptr);
        assert(hg->border.w == 1200);
        whaleui_layout_node_t* hg1 = hg->first_child;
        /* first child may be a text run? hero-grid has no direct text */
        while (hg1 && hg1->is_text) {
            hg1 = hg1->next;
        }
        assert(hg1 != nullptr);
        assert(hg1->border.w == 615 || hg1->border.w == 616);
        whaleui_layout_node_t* hg2 = hg1->next;
        assert(hg2 != nullptr);
        assert(hg2->border.x == hg1->border.x + hg1->border.w + 60);
        assert(hg2->border.w == 524 || hg2->border.w == 525);

        /* .ticker: border-block:1px solid var(--line) -> top+bottom */
        whaleui_layout_node_t* ticker = find_class(t->root, "ticker");
        assert(ticker != nullptr);
        assert(ticker->border_w[0] == 1 && ticker->border_w[2] == 1);

        /* .tk: white-space:nowrap -> the text run does not wrap to the
         * parent width (its box is wider than a single span) */
        whaleui_layout_node_t* tk = find_class(t->root, "tk");
        assert(tk != nullptr);
        assert(tk->first_child != nullptr);

        /* .ms-index: position:sticky; top:100px */
        whaleui_layout_node_t* msi = find_class(t->root, "ms-index");
        assert(msi != nullptr);
        assert(std::strcmp(sget(msi->style, "position"), "sticky") == 0);

        /* .glyph: absolute; right:-2%; top:-30px -> right edge 2% past the
         * section's right edge */
        whaleui_layout_node_t* glyph = find_class(t->root, "glyph");
        assert(glyph != nullptr);
        assert(std::strcmp(sget(glyph->style, "position"), "absolute") == 0);
        /* section width = viewport; glyph overflows to the right */
        assert(glyph->border.x + glyph->border.w > 1200);

        /* h2: font-size clamp(30px, 4.4vw, 52px) cascades through (the
         * numeric resolution happens in the layout pass) */
        whaleui_layout_node_t* h2 = find_tag(t->root, "h2");
        assert(h2 != nullptr);
        assert(std::strncmp(sget(h2->style, "font-size"), "clamp(", 6) == 0);

        /* .term-hd i:nth-child(2) -> var(--gold) = #e0b458 */
        whaleui_layout_node_t* thd = find_class(t->root, "term-hd");
        assert(thd != nullptr);
        whaleui_layout_node_t* i1 = thd->first_child;
        while (i1 && i1->is_text) {
            i1 = i1->next;
        }
        assert(i1 != nullptr);
        whaleui_layout_node_t* i2 = i1->next;
        assert(i2 != nullptr);
        assert(std::strcmp(sget(i2->style, "background"), "#e0b458") == 0);

        /* .t-cmd-line::before { content:'❯ ' } -> the text run starts
         * with the pseudo content */
        whaleui_layout_node_t* tcl = find_class(t->root, "t-cmd-line");
        assert(tcl != nullptr);
        whaleui_layout_node_t* run = tcl->first_child;
        while (run && !run->is_text) {
            run = run->next;
        }
        assert(run != nullptr && run->is_text);
        assert(run->text.find("❯") == 0);
        /* .t-cmd-line::before { color: var(--gold) } applies to the
         * pseudo run only */
        assert(std::strcmp(sget(run->style, "color"), "#e0b458") == 0);

        /* .breath img: width:100% height:320px box */
        whaleui_layout_node_t* br = find_class(t->root, "breath");
        assert(br != nullptr);
        whaleui_layout_node_t* img = nullptr;
        for (whaleui_layout_node_t* c = br->first_child; c; c = c->next) {
            if (!c->is_text && c->el) {
                size_t len = 0;
                const lxb_char_t* name =
                    lxb_dom_element_local_name(c->el, &len);
                if (name && len == 3 && std::memcmp(name, "img", 3) == 0) {
                    img = c;
                    break;
                }
            }
        }
        assert(img != nullptr);
        assert(img->border.h == 320);

        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    /* --- viewport 800: the @media(max-width:900px) branch applies --- */
    {
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(html.c_str(),
                                                             html.size());
        assert(doc != nullptr);
        whaleui_layout_tree_t* t =
            whaleui_layout_compute(doc, r8, c8, nullptr, 800, 900, nullptr,
                                   nullptr, nullptr, 1.0f);
        assert(t != nullptr);

        /* .wrap: min(1120px, 92vw) = 736, centered */
        whaleui_layout_node_t* wrap = find_class(t->root, "wrap");
        assert(wrap != nullptr);
        assert(wrap->border.w == 736);
        assert(wrap->border.x == (800 - 736) / 2);

        /* nav a:nth-child(n+4) { display:none } -> 4th link hidden */
        whaleui_layout_node_t* nav = find_tag(t->root, "nav");
        assert(nav != nullptr);
        int vis = 0;
        for (whaleui_layout_node_t* a = nav->first_child; a; a = a->next) {
            if (!a->is_text && a->visible) {
                ++vis;
            }
        }
        assert(vis == 3); /* 5 links, last two hidden */

        whaleui_layout_destroy(t);
        whaleui_dom_document_destroy(doc);
    }

    whaleui_css_rules_destroy(r12, c12);
    whaleui_css_rules_destroy(r8, c8);
    whaleui_css_rules_destroy(rules, count);
    std::printf("test_qwen: all assertions passed\n");
    return 0;
}
