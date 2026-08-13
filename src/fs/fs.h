#ifndef WHALEUI_FS_FS_H
#define WHALEUI_FS_FS_H

/* Virtual file system - internal interface.
 *
 * ALL file operations in the project (HTML/CSS/image/font) go through here.
 * Default loader reads from disk; users may replace it via the public API
 * whaleui_fs_set_loader() to support other protocols (e.g. HTTP(S)). */

#include <stddef.h>
#include "whaleui.h" /* for whaleui_fs_open_fn/read_fn/close_fn */

#ifdef __cplusplus
extern "C" {
#endif

/* Global loader, set by whaleui_fs_set_loader (NULL = default disk). */
void whaleui_fs_set_loader_impl(whaleui_fs_open_fn open, whaleui_fs_read_fn read,
                                whaleui_fs_close_fn close, void* userdata);

/* Load the whole uri into memory (malloc'd, caller frees).
 * 0 on success, non-zero on failure. */
int whaleui_fs_load(const char* uri, char** out, size_t* outlen);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_FS_FS_H */
