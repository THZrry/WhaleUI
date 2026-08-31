// test_anim: animation engine tests (@keyframes + transition).
#include "whaleui.h"
#include "animate/animate.h"
#include "layout/layout.h"
#include "style/css.h"
#include "style/style.h"

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

static float num(const std::string& s)
{
    return static_cast<float>(std::atof(s.c_str()));
}

static void load_kf(whaleui_css_keyframes_t* kf, const char* css)
{
    whaleui_css_rule_t* rules = nullptr;
    size_t count = 0;
    assert(whaleui_css_parse_full(css, std::strlen(css), &rules, &count, kf) == 0);
    whaleui_css_rules_destroy(rules, count);
}

/* find the first element node with the given tag, depth-first */
static whaleui_layout_node_t* find_el(whaleui_layout_node_t* n, const char* tag)
{
    if (!n) {
        return nullptr;
    }
    if (n->el && !n->is_text) {
        size_t len = 0;
        const lxb_char_t* name = lxb_dom_element_local_name(n->el, &len);
        if (name && len == std::strlen(tag) &&
            std::memcmp(name, tag, len) == 0) {
            return n;
        }
    }
    for (whaleui_layout_node_t* c = n->first_child; c; c = c->next) {
        whaleui_layout_node_t* r = find_el(c, tag);
        if (r) {
            return r;
        }
    }
    return nullptr;
}

int main(void)
{
    /* @keyframes: interpolate between frames, end after one iteration */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "fade 1000ms linear";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(num(s["opacity"]) < 0.01f);
        assert(whaleui_anim_apply(a, nullptr, s, 500) == 1);
        assert(num(s["opacity"]) > 0.49f && num(s["opacity"]) < 0.51f);
        assert(whaleui_anim_apply(a, nullptr, s, 999) == 1);
        assert(num(s["opacity"]) > 0.99f);
        /* finished (fill-mode none): no longer animating, style reverts */
        assert(whaleui_anim_apply(a, nullptr, s, 1000) == 0);
        assert(num(s["opacity"]) > 0.99f);
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* fill-mode forwards: hold the last frame after the run */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "fade 1000ms linear forwards";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(whaleui_anim_apply(a, nullptr, s, 1500) == 0);
        assert(num(s["opacity"]) > 0.99f); /* held at the 100% frame */
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* infinite iteration loops */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "fade 1000ms linear infinite";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(whaleui_anim_apply(a, nullptr, s, 2500) == 1); /* 2.5 loops -> p=0.5 */
        assert(num(s["opacity"]) > 0.49f && num(s["opacity"]) < 0.51f);
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* 3-frame round-trip (0%->50%->100%), infinite: the pulse pattern
     * (opacity .45 -> 1 -> .45). Regression: multi-frame keyframes with a
     * middle frame must interpolate both segments and loop. */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf,
                "@keyframes pulse { 0% { opacity: 0.45; } "
                "50% { opacity: 1; } 100% { opacity: 0.45; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "pulse 1000ms linear infinite";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(num(s["opacity"]) > 0.40f && num(s["opacity"]) < 0.50f);
        assert(whaleui_anim_apply(a, nullptr, s, 500) == 1);
        std::fprintf(stderr, "[pulse] t=500 opacity=%s\n", s["opacity"].c_str());
        assert(num(s["opacity"]) > 0.99f); /* 50% frame: 1 */
        assert(whaleui_anim_apply(a, nullptr, s, 750) == 1);
        std::fprintf(stderr, "[pulse] t=750 opacity=%s\n", s["opacity"].c_str());
        assert(num(s["opacity"]) > 0.70f && num(s["opacity"]) < 0.80f);
        assert(whaleui_anim_apply(a, nullptr, s, 1000) == 1); /* loops */
        std::fprintf(stderr, "[pulse] t=1000 opacity=%s\n", s["opacity"].c_str());
        assert(num(s["opacity"]) > 0.40f && num(s["opacity"]) < 0.50f);
        assert(whaleui_anim_apply(a, nullptr, s, 1250) == 1);
        std::fprintf(stderr, "[pulse] t=1250 opacity=%s\n", s["opacity"].c_str());
        assert(num(s["opacity"]) > 0.70f && num(s["opacity"]) < 0.80f); /* 2nd loop @25% */
        assert(whaleui_anim_apply(a, nullptr, s, 1500) == 1);
        std::fprintf(stderr, "[pulse] t=1500 opacity=%s\n", s["opacity"].c_str());
        assert(num(s["opacity"]) > 0.99f); /* second loop @50% */
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* single-`from` keyframes imply 100% = the element's base style
     * ("rise{from{opacity:0}}" animates 0 -> base 1, not 0 forever) */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes rise { from { opacity: 0; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "rise 1000ms linear";
        s["opacity"] = "1"; /* base */
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(num(s["opacity"]) < 0.01f); /* from frame: 0 */
        assert(whaleui_anim_apply(a, nullptr, s, 500) == 1);
        assert(num(s["opacity"]) > 0.49f && num(s["opacity"]) < 0.51f);
        assert(whaleui_anim_apply(a, nullptr, s, 999) == 1);
        assert(num(s["opacity"]) > 0.99f); /* toward base 1 */
        assert(whaleui_anim_apply(a, nullptr, s, 1000) == 0); /* done */
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* transition: interpolate a changed opacity toward the new value */
    {
        whaleui_anim_t* a = whaleui_anim_create();
        WhaleUIComputedStyle s;
        s["transition"] = "opacity 1000ms linear";
        s["opacity"] = "0";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 0); /* baseline */
        s["opacity"] = "1"; /* the computed style changed */
        assert(whaleui_anim_apply(a, nullptr, s, 100) == 1);
        assert(num(s["opacity"]) < 0.01f); /* starts from the old value */
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 600) == 1);
        assert(num(s["opacity"]) > 0.49f && num(s["opacity"]) < 0.51f);
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 1100) == 0); /* done */
        assert(num(s["opacity"]) > 0.99f);
        whaleui_anim_destroy(a);
    }

    /* transition: colors interpolate channel-wise */
    {
        whaleui_anim_t* a = whaleui_anim_create();
        WhaleUIComputedStyle s;
        s["transition"] = "background-color 1000ms linear";
        s["background-color"] = "#000000";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 0);
        s["background-color"] = "#ffffff";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(s["background-color"] == "#000000"); /* old value shown first */
        s["background-color"] = "#ffffff";
        assert(whaleui_anim_apply(a, nullptr, s, 500) == 1);
        assert(s["background-color"] == "#ff808080");
        s["background-color"] = "#ffffff";
        assert(whaleui_anim_apply(a, nullptr, s, 1000) == 0);
        assert(s["background-color"] == "#ffffff");
        whaleui_anim_destroy(a);
    }

    /* transition: non-interpolable values snap, no animation */
    {
        whaleui_anim_t* a = whaleui_anim_create();
        WhaleUIComputedStyle s;
        s["transition"] = "opacity 1000ms";
        s["opacity"] = "0";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 0);
        s["opacity"] = "auto";
        assert(whaleui_anim_apply(a, nullptr, s, 500) == 0);
        assert(s["opacity"] == "auto");
        whaleui_anim_destroy(a);
    }

    /* no animation/transition configured: style untouched */
    {
        whaleui_anim_t* a = whaleui_anim_create();
        WhaleUIComputedStyle s;
        s["color"] = "red";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 0);
        assert(s["color"] == "red");
        whaleui_anim_destroy(a);
    }

    /* layout integration: animated opacity is injected into the layout tree
     * and keeps the animation flagged as running */
    {
        const char* html =
            "<html><body><div style=\"animation: fade 2000ms linear;\"></div></body></html>";
        whaleui_dom_document_t* doc = whaleui_dom_parse_html(html, std::strlen(html));
        assert(doc != nullptr);
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        std::map<std::string, std::string> vars;
        whaleui_style_state st = {nullptr, nullptr, nullptr};
        std::map<lxb_dom_element*, int> scrolls;
        whaleui_layout_tree_t* t = whaleui_layout_compute(
            doc, nullptr, 0, &vars, 800, 600, &st, &scrolls, a, 1.0f);
        assert(t != nullptr);
        assert(whaleui_anim_active(a) == 1);
        whaleui_layout_node_t* div = find_el(t->root, "div");
        assert(div != nullptr);
        /* animation just started: opacity near the 0% frame */
        assert(num(div->style["opacity"]) < 0.5f);
        whaleui_layout_destroy(t);
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
        whaleui_dom_document_destroy(doc);
    }

    /* cubic-bezier timing: ease-out-ish curve lands above linear at t=0.5 */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "fade 1000ms cubic-bezier(.22,1,.36,1)";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(whaleui_anim_apply(a, nullptr, s, 500) == 1);
        assert(num(s["opacity"]) > 0.90f); /* y(0.5) ~= 0.96 */
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* steps(n): discrete jumps, no in-between interpolation */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes blink { 0% { opacity: 1; } 100% { opacity: 0; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "blink 1000ms steps(2) infinite";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 250) == 1);
        assert(num(s["opacity"]) > 0.99f); /* first step (0..0.5) */
        assert(whaleui_anim_apply(a, nullptr, s, 750) == 1);
        assert(num(s["opacity"]) > 0.49f && num(s["opacity"]) < 0.51f); /* second step */
        assert(whaleui_anim_apply(a, nullptr, s, 999) == 1);
        assert(num(s["opacity"]) > 0.49f && num(s["opacity"]) < 0.51f);
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* fill-mode backwards: first frame shown during the delay */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "fade 500ms linear 200ms backwards";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1); /* starts in delay */
        assert(num(s["opacity"]) < 0.01f);
        assert(whaleui_anim_apply(a, nullptr, s, 100) == 1); /* still in delay */
        assert(num(s["opacity"]) < 0.01f);
        assert(whaleui_anim_apply(a, nullptr, s, 450) == 1); /* 250ms into the run */
        assert(num(s["opacity"]) > 0.49f && num(s["opacity"]) < 0.51f);
        s["opacity"] = "1"; /* recomputed style after the run */
        assert(whaleui_anim_apply(a, nullptr, s, 750) == 0); /* done */
        assert(num(s["opacity"]) > 0.99f); /* reverts to computed */
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* fill-mode both: hold the last frame after the run */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "fade 500ms linear 100ms both";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1); /* backwards in delay */
        assert(num(s["opacity"]) < 0.01f);
        assert(whaleui_anim_apply(a, nullptr, s, 700) == 0); /* finished */
        assert(num(s["opacity"]) > 0.99f); /* forwards holds the end */
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* direction alternate: even cycles forward, odd cycles backward */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "fade 1000ms linear infinite alternate";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1); /* start of cycle 0 */
        assert(whaleui_anim_apply(a, nullptr, s, 250) == 1); /* forward */
        assert(num(s["opacity"]) > 0.24f && num(s["opacity"]) < 0.26f);
        assert(whaleui_anim_apply(a, nullptr, s, 1250) == 1); /* cycle 1: backward */
        assert(num(s["opacity"]) > 0.74f && num(s["opacity"]) < 0.76f);
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* animation longhands: delay set via animation-delay rule */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "fade 1000ms linear";
        s["animation-delay"] = "100ms";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1); /* starts in delay */
        assert(whaleui_anim_apply(a, nullptr, s, 50) == 1); /* still in delay */
        assert(whaleui_anim_apply(a, nullptr, s, 600) == 1); /* 500ms into the run */
        assert(num(s["opacity"]) > 0.49f && num(s["opacity"]) < 0.51f);
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* transition property whitelist: only listed properties animate */
    {
        whaleui_anim_t* a = whaleui_anim_create();
        WhaleUIComputedStyle s;
        s["transition"] = "background-color 1000ms linear";
        s["opacity"] = "0";
        s["background-color"] = "#000000";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 0);
        s["opacity"] = "1"; /* not in the whitelist: snaps */
        s["background-color"] = "#ffffff"; /* in the whitelist: animates */
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(s["opacity"] == "1");
        assert(s["background-color"] == "#000000");
        whaleui_anim_destroy(a);
    }

    /* transition-delay longhand: hold the old value during the delay */
    {
        whaleui_anim_t* a = whaleui_anim_create();
        WhaleUIComputedStyle s;
        s["transition"] = "opacity 1000ms linear";
        s["transition-delay"] = "200ms";
        s["opacity"] = "0";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 0);
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 100) == 1); /* change: +200ms delay */
        assert(num(s["opacity"]) < 0.01f); /* still in delay */
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 800) == 1); /* 500ms into the run */
        assert(num(s["opacity"]) > 0.49f && num(s["opacity"]) < 0.51f);
        whaleui_anim_destroy(a);
    }

    /* transform keyframes interpolate translate() -> none */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes rise { from { transform: translateY(16px); } "
                     "to { transform: none; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "rise 1000ms linear";
        s["transform"] = "none";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(s["transform"] == "translate(0px, 16px)");
        assert(whaleui_anim_apply(a, nullptr, s, 500) == 1);
        assert(s["transform"] == "translate(0px, 8px)");
        assert(whaleui_anim_apply(a, nullptr, s, 999) == 1);
        s["transform"] = "none"; /* recomputed style after the run */
        assert(whaleui_anim_apply(a, nullptr, s, 1000) == 0); /* finished */
        assert(s["transform"] == "none");
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    /* transform eval: translate/scale resolution for the painter */
    {
        whaleui_transform_t t;
        assert(whaleui_transform_eval("none", 100, 50, &t) == 0);
        assert(t.tx == 0 && t.ty == 0 && t.sx == 1 && t.sy == 1);
        assert(whaleui_transform_eval("translateY(16px)", 100, 50, &t) == 0);
        assert(t.tx == 0 && t.ty == 16);
        assert(whaleui_transform_eval("translate(10%, 20%)", 100, 50, &t) == 0);
        assert(t.tx == 10 && t.ty == 10);
        assert(whaleui_transform_eval("scale(1.09) translate(1.5%, -1%)", 100, 50, &t) == 0);
        assert(t.sx > 1.08f && t.sx < 1.10f && t.tx > 1.4f && t.tx < 1.6f);
        assert(t.ty > -0.6f && t.ty < -0.4f);
        assert(whaleui_transform_eval("rotate(45deg)", 100, 50, &t) != 0);
    }
    /* paint-only vs layout animation classification (tick fast path) */
    {
        whaleui_css_keyframes_t kf = {nullptr, 0};
        load_kf(&kf, "@keyframes fade { 0% { opacity: 0; } "
                     "100% { opacity: 1; } }");
        whaleui_anim_t* a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        WhaleUIComputedStyle s;
        s["animation"] = "fade 1000ms linear infinite";
        s["opacity"] = "1";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(whaleui_anim_tick(a, 500) == 1);
        assert(whaleui_anim_needs_layout(a) == 0); /* opacity is paint-only */
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);

        /* width animation must force layout rebuilds */
        kf = {nullptr, 0};
        load_kf(&kf, "@keyframes w { from { width: 10px; } "
                     "to { width: 100px; } }");
        a = whaleui_anim_create();
        whaleui_anim_set_keyframes(a, &kf);
        s.clear();
        s["animation"] = "w 1000ms linear infinite";
        s["width"] = "50px";
        assert(whaleui_anim_apply(a, nullptr, s, 0) == 1);
        assert(whaleui_anim_tick(a, 500) == 1);
        assert(whaleui_anim_needs_layout(a) == 1); /* width is layout */
        whaleui_anim_destroy(a);
        whaleui_css_keyframes_destroy(&kf);
    }

    return 0;
}
