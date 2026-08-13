#ifndef WHALEUI_DOM_DOM_H
#define WHALEUI_DOM_DOM_H

/* DOM - internal interface.
 *
 * Step 3: handles ARE the lexbor objects. A whaleui_dom_document_t* is an
 * lxb_html_document* and a whaleui_dom_element_t* is an lxb_dom_element*,
 * passed through as opaque pointers (C-compatible, stable identity, no
 * wrapper allocation). The structs below are never instantiated; they only
 * give the public API a named type. Internal modules cast back via
 * reinterpret_cast. */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_dom_element { char _opaque; };
struct whaleui_dom_document { char _opaque; };

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_DOM_DOM_H */
