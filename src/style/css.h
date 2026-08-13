#ifndef WHALEUI_STYLE_CSS_H
#define WHALEUI_STYLE_CSS_H

/* CSS parsing - internal interface.
 *
 * A rule = selector + flat "property=value" declarations (same storage as
 * DOM attributes, step 2 contract). Step 3 adds full cascade/theme mapping. */

#include "whaleui.h"

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_css_rule
{
    char* selector;
    char** decls;
    size_t decl_count;
};

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_STYLE_CSS_H */
