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
 * pressed = element the left button is held down on. Default-initialized
 * to nullptr: a caller that forgets to fill the state must never feed
 * garbage element pointers into the cascade (style_match dereferences
 * them). */
struct whaleui_style_state
{
    lxb_dom_element* hover = nullptr;
    lxb_dom_element* focus = nullptr;
    lxb_dom_element* pressed = nullptr;
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

/* Evaluate an @media condition against the current theme + viewport.
 * viewport_h < 0 means unknown (height/orientation/aspect conditions then
 * evaluate to false... see the impl: height-gated conditions need h).
 * Unsupported conditions evaluate to true (paint more, not less). */
int whaleui_style_media_ok(const char* media, int theme, int viewport_w,
                           int reduced_motion, int viewport_h = -1);

/* Drop rules whose @media condition does not match the current context
 * (theme, viewport, reduced-motion). In-place compaction of the
 * array; destroyed rules are freed. */
int whaleui_style_filter_media(whaleui_css_rule_t* rules, size_t* count,
                               int theme, int viewport_w, int reduced_motion,
                               int viewport_h = -1);

/* Collect custom properties (--*) defined on root into vars. */
void whaleui_style_collect_vars(lxb_dom_element* root,
                                std::map<std::string, std::string>& vars);

/* Document-level programmatic stylesheet (whaleui_css_apply).
 *
 * The engine assembles a window's real sheet per frame source: the theme
 * default CSS plus the document's <style>/<link> text. Rules handed in through
 * the public CSS API are kept here instead, as the document's OWN sheet, and
 * are appended to that assembly - so they cascade like any other sheet instead
 * of being computed into a throwaway map nothing rendered from (the API used
 * to be a silent no-op: the documented "apply a set of rules to a document"
 * changed no pixel).
 *
 * set() deep-copies and REPLACES the document's set (count == 0 clears it, the
 * "swap the theme sheet" use case); get() returns the borrowed array the
 * collector appends (valid until the next set/clear or document destroy). */
void whaleui_style_doc_rules_set(whaleui_dom_document_t* doc,
                                 const whaleui_css_rule_t* rules, size_t count);
const whaleui_css_rule_t* whaleui_style_doc_rules_get(
    whaleui_dom_document_t* doc, size_t* count);
void whaleui_style_doc_rules_clear(whaleui_dom_document_t* doc);

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

/* 1 when `name` is a property the engine actually reads (see style.cpp for the
 * list). The CSS parser keeps unknown declarations on purpose, so it cannot
 * answer "is this supported" - CSS.supports needs this table. */
int whaleui_style_property_supported(const char* name);

/* 1 when `value` is plausible for `name`: checked with the engine's own value
 * parsers where one exists (lengths, colors, the display/position/overflow
 * keyword sets), accepted otherwise. Properties whose value syntax the engine
 * does not parse yet answer 1 - see the note in style.cpp. */
int whaleui_style_value_supported(const char* name, const char* value);

/* Merge the declarations of every rule whose selector equals `selector`
 * (a synthetic element with no DOM node - e.g. the select popup
 * ".select-popup"/".select-option-hover"). var() values resolve against
 * vars; later rules win per declaration. */
void whaleui_style_virtual(const char* selector,
                           const whaleui_css_rule_t* rules, size_t count,
                           const std::map<std::string, std::string>& vars,
                           WhaleUIComputedStyle& out);

/* Shadow-host variant: the popup is the select's shadow child, so page CSS
 * can target it through the host (`select .select-popup`,
 * `select.foo .select-option-hover`, `#theme > .select-popup`). The right-
 * most compound must be the virtual class; the ancestor part matches
 * against the host element with the normal matcher (descendant matching
 * is transitive, so `P .popup` and `P > .popup` both reduce correctly).
 * Bare `.select-popup` always matches. `st` carries interaction state. */
void whaleui_style_virtual_host(const char* vclass, lxb_dom_element* host,
                                const whaleui_css_rule_t* rules, size_t count,
                                const std::map<std::string, std::string>& vars,
                                const whaleui_style_state* st,
                                WhaleUIComputedStyle& out);

/* Drop the cached rule tag index for a destroyed rule set. The cascade
 * index is keyed on the rules array identity; the allocator can hand the
 * same address to the next parse, so a destroyed set must explicitly
 * invalidate it (otherwise the next document reuses a stale index and its
 * class rules never match). Called from whaleui_css_rules_destroy. */
void whaleui_style_index_drop(const whaleui_css_rule_t* rules);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_STYLE_STYLE_H */
