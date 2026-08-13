#ifndef WHALEUI_LAYOUT_LAYOUT_H
#define WHALEUI_LAYOUT_LAYOUT_H

/* Layout - internal interface.
 *
 * Lexbor already implements HTML/CSS layout (position/size computation).
 * This module does NOT re-implement any layout algorithm: it only adapts
 * lexbor's computed boxes into the flat whaleui_layout_box_t the renderer
 * consumes, and owns the per-element layout_box in whaleui_dom_element.
 * The whaleui_layout_* functions below are thin adapters over lexbor. */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct whaleui_rect
{
    int x, y, w, h;
} whaleui_rect_t;

typedef struct whaleui_layout_box
{
    whaleui_rect_t border;  /* outer box (border edge) */
    whaleui_rect_t content; /* content box (padding excluded) */
} whaleui_layout_box_t;

/* Compute layout for a document's subtree (step 3: lexbor). */
int whaleui_layout_document(whaleui_dom_document_t* doc, int viewport_w, int viewport_h);

/* Get the layout box of an element, or NULL if not laid out. */
const whaleui_layout_box_t* whaleui_layout_box(whaleui_dom_element_t* el);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_LAYOUT_LAYOUT_H */
