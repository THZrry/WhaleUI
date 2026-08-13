#ifndef WHALEUI_CORE_WINDOW_H
#define WHALEUI_CORE_WINDOW_H

/* Window core - internal interface. */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_window
{
    whaleui_app_t* app;
    char* title;
    int width;
    int height;
    int visible;
    whaleui_dom_document_t* document;
};

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_WINDOW_H */
