/* Layout: thin adapter over lexbor's computed layout.
 * Step 2: contract stub - returns not-implemented. Step 3 calls lexbor to lay
 * out the document and copies its boxes into whaleui_layout_box_t. No layout
 * algorithm is implemented in this project. */

#include "layout/layout.h"

extern "C" int whaleui_layout_document(whaleui_dom_document_t* doc, int viewport_w, int viewport_h)
{
    (void)doc;
    (void)viewport_w;
    (void)viewport_h;
    /* Step 2: contract only. */
    return -1;
}

extern "C" const whaleui_layout_box_t* whaleui_layout_box(whaleui_dom_element_t* el)
{
    (void)el;
    return nullptr;
}
