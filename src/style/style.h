#ifndef WHALEUI_STYLE_STYLE_H
#define WHALEUI_STYLE_STYLE_H

/* Style engine - internal interface.
 *
 * Selector matching + cascade + var() resolution + computed styles. The
 * result is a plain property->value map consumed by layout and render. */

#include "style/css.h"

#include <map>
#include <string>

/* lexbor element type (full def in lexbor headers) */
struct lxb_dom_element;

typedef std::map<std::string, std::string> WhaleUIComputedStyle;

#ifdef __cplusplus
extern "C" {
#endif

/* Interaction state driving pseudo-class matching (:hover/:active/:focus).
 * hover = element under the mouse; focus = last clicked control;
 * pressed = element the left button is held down on. */
struct whaleui_style_state
{
    lxb_dom_element* hover;
    lxb_dom_element* focus;
    lxb_dom_element* pressed;
};

/* Does the (already comma-split) selector match this element?
 * Supports: tag, #id, .class, combinations ("div.card"), descendant chains
 * ("div .card"), child chains (">"), pseudo-classes :hover/:active/:focus
 * (matched against st), :disabled (via the disabled attribute), and skips
 * other pseudo-class suffixes. */
int whaleui_style_match(const char* selector, lxb_dom_element* el,
                        const whaleui_style_state* st);

/* Like whaleui_style_match but recognizes a trailing "::before"/"::after"
 * pseudo-element: the base selector must match el, and *pseudo is set to
 * 1 (before) / 2 (after). Plain selectors return 1 with *pseudo = 0. */
int whaleui_style_match_pseudo(const char* selector, lxb_dom_element* el,
                               const whaleui_style_state* st, int* pseudo);

/* Evaluate an @media condition against the current theme + viewport width.
 * Unsupported conditions evaluate to false (safe default). */
int whaleui_style_media_ok(const char* media, int theme, int viewport_w,
                           int reduced_motion);

/* Drop rules whose @media condition does not match the current context
 * (theme, viewport width, reduced-motion). In-place compaction of the
 * array; destroyed rules are freed. */
int whaleui_style_filter_media(whaleui_css_rule_t* rules, size_t* count,
                               int theme, int viewport_w, int reduced_motion);

/* Collect custom properties (--*) defined on root into vars. */
void whaleui_style_collect_vars(lxb_dom_element* root,
                                std::map<std::string, std::string>& vars);

/* Like collect_vars but also includes --* from rules matching :root/html. */
void whaleui_style_collect_vars_full(lxb_dom_element* root,
                                     const whaleui_css_rule_t* rules, size_t count,
                                     std::map<std::string, std::string>& vars);

/* Cascade rules (+ inline style) into a computed style for el.
 * vars: custom properties (from :root and the theme) for var() resolution.
 * st: interaction state for pseudo-class matching (:hover/:active/:focus). */
WhaleUIComputedStyle whaleui_style_compute(lxb_dom_element* el,
                                           const whaleui_css_rule_t* rules, size_t count,
                                           const std::map<std::string, std::string>& vars,
                                           const whaleui_style_state* st);

/* Length helpers for layout: "12px"/"1.5em"/"50%"/"auto" -> number + unit id.
 * unit: 0=px, 1=%, 2=em, 3=auto, 4=number(unitless). Returns 0 on success. */
int whaleui_style_parse_len(const char* value, float* num, int* unit);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_STYLE_STYLE_H */
