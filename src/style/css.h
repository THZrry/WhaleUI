#ifndef WHALEUI_STYLE_CSS_H
#define WHALEUI_STYLE_CSS_H

/* CSS parsing - internal interface.
 *
 * Step 3: rules keep the selector + flat "property=value" declarations
 * (same storage as before), plus optional media-condition grouping and
 * keyframe blocks. The parser is self-contained (no third-party CSS parser):
 * it handles comments, @media / @keyframes / @font-face, comma selector
 * lists, !important markers and custom properties (--*). Selector matching
 * and cascade live in style.cpp. */

#include "whaleui.h"

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/* one @keyframes block: name + frames ("0%"/"from"/"to"/... -> decls) */
struct whaleui_keyframes
{
    char* name;
    char** frames;      /* "percentage|prop=value;..." encoded, see css.cpp */
    size_t frame_count;
};
typedef struct whaleui_keyframes whaleui_keyframes_t;

struct whaleui_css_rule
{
    char* selector;
    char** decls;
    size_t decl_count;

    /* step 3 additions */
    char* media;        /* @media condition, NULL for plain rules */
    int important;      /* rule-level: any decl marked !important */
};

/* @keyframes registry parsed out of a stylesheet */
struct whaleui_css_keyframes
{
    whaleui_keyframes_t* items;
    size_t count;
};
typedef struct whaleui_css_keyframes whaleui_css_keyframes_t;

/* Full parse including @keyframes (used by the style engine + white-box
 * tests; the public whaleui_css_parse discards keyframes). */
int whaleui_css_parse_full(const char* css, size_t len,
                           whaleui_css_rule_t** rules, size_t* count,
                           whaleui_css_keyframes_t* keyframes);
void whaleui_css_keyframes_destroy(whaleui_css_keyframes_t* keyframes);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_STYLE_CSS_H */
