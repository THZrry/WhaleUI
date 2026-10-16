#ifndef WHALEUI_DOM_DATA_TRANSFER_H
#define WHALEUI_DOM_DATA_TRANSFER_H

/* DataTransfer / File - internal interface (see data_transfer.cpp).
 *
 * The public C API exposes these as opaque handles; the internal structs live
 * here so the render side (in-page drag sessions) and app.cpp (OS drops) can
 * build and inspect them. */

#include "whaleui.h"

#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_file
{
    std::string name;  /* basename shown to the page */
    std::string uri;   /* VFS-readable uri the bytes come from */
    long long size;    /* -1 until measured (measured lazily, then cached) */
    int measured;
};

struct whaleui_data_transfer
{
    /* typed items, in insertion order (types_item walks this) */
    std::vector<std::pair<std::string, std::string>> items;
    std::vector<whaleui_file_t*> files;
    int effect_allowed; /* 0 none, 1 copy, 2 move, 3 link */
    int drop_effect;
    /* scratch buffer for get_data/get_text returns when the item is absent:
     * the C contract promises a pointer valid until the next call */
    std::string scratch;
};

/* Attach a transfer to an event under construction (used by the dispatch
 * entry points; the pointer is borrowed, not owned). */
void whaleui_dom_event_set_data_transfer(whaleui_dom_event_t* ev,
                                         whaleui_data_transfer_t* dt);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_DOM_DATA_TRANSFER_H */
