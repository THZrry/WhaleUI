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

/* Does the (already comma-split) selector match this element?
 * Supports: tag, #id, .class, combinations ("div.card"), descendant chains
 * ("div .card"), child chains (">"), and skips pseudo-class suffixes. */
int whaleui_style_match(const char* selector, lxb_dom_element* el);

/* Evaluate an @media condition against the current theme + viewport width.
 * Unsupported conditions evaluate to false (safe default). */
int whaleui_style_media_ok(const char* media, int theme, int viewport_w);

/* Collect custom properties (--*) defined on root into vars. */
void whaleui_style_collect_vars(lxb_dom_element* root,
                                std::map<std::string, std::string>& vars);

/* Like collect_vars but also includes --* from rules matching :root/html. */
void whaleui_style_collect_vars_full(lxb_dom_element* root,
                                     const whaleui_css_rule_t* rules, size_t count,
                                     std::map<std::string, std::string>& vars);

/* Cascade rules (+ inline style) into a computed style for el.
 * vars: custom properties (from :root and the theme) for var() resolution. */
WhaleUIComputedStyle whaleui_style_compute(lxb_dom_element* el,
                                           const whaleui_css_rule_t* rules, size_t count,
                                           const std::map<std::string, std::string>& vars);

/* Length helpers for layout: "12px"/"1.5em"/"50%"/"auto" -> number + unit id.
 * unit: 0=px, 1=%, 2=em, 3=auto, 4=number(unitless). Returns 0 on success. */
int whaleui_style_parse_len(const char* value, float* num, int* unit);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_STYLE_STYLE_H */
