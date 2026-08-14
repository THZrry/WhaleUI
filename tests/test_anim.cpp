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
            doc, nullptr, 0, &vars, 800, 600, &st, &scrolls, a);
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

    return 0;
}
