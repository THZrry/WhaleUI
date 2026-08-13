#ifndef WHALEUI_DOM_DOM_H
#define WHALEUI_DOM_DOM_H

/* DOM - internal interface.
 *
 * Elements wrap lexbor nodes (step 3). Step 2 keeps a minimal tree so the
 * public contract is exercised; lexbor integration replaces the internals. */

#include "whaleui.h"

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_dom_element
{
    char* tag;
    char* id;
    char* text;
    /* attribute storage: flat array of "name=value" strings */
    char** attrs;
    size_t attr_count;
    /* inline style: flat array of "property=value" strings */
    char** styles;
    size_t style_count;
    whaleui_dom_element_t* parent;
    whaleui_dom_element_t* first_child;
    whaleui_dom_element_t* next_sibling;
};

struct whaleui_dom_document
{
    whaleui_dom_element_t* root; /* document element (html) */
};

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_DOM_DOM_H */
