#ifndef WHALEUI_CORE_APP_H
#define WHALEUI_CORE_APP_H

/* Application core - internal interface. */

#include "whaleui.h"

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_app
{
    whaleui_theme_t theme;
    char accent[16];
    int max_fps;
    int battery_saver;
    int vsync;
    int running;
};

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CORE_APP_H */
