#ifndef WHALEUI_FONT_FONT_H
#define WHALEUI_FONT_FONT_H

/* Font registry - internal interface.
 *
 * All font file loading goes through the virtual file system
 * (whaleui_fs_load). System-font fallback paths come from platform/. */

#include "whaleui.h"

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_font
{
    char* family;
    /* path for file-registered fonts (owned; the file is read lazily when
     * a TTF_Font is actually opened, so a registered-but-unused font costs
     * only this string - the 20MB+ msyh.ttc data is no longer pinned in
     * memory just by being registered). NULL for memory-registered fonts. */
    char* path;
    /* borrowed for memory-registered fonts; NULL when path is set */
    const unsigned char* data;
    size_t len;
};

typedef struct whaleui_font whaleui_font_t;

/* Registry (global; step 3 may move into WhaleUIApp). */
struct whaleui_font_registry
{
    whaleui_font_t* fonts;
    size_t count;
    char* default_family;
};

whaleui_font_registry* whaleui_font_registry_get(void);

/* Register the platform's common UI fonts (Segoe UI, Segoe UI Emoji, YaHei,
 * SimHei, SimSun, Arial...) so text rendering can fall back across them.
 * Idempotent. Returns the number of fonts newly registered. */
int whaleui_font_register_system_defaults(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_FONT_FONT_H */
