#ifndef WHALEUI_WHALEUI_H
#define WHALEUI_WHALEUI_H

/* WhaleUI public C API.
 * C++14 core, exposed as plain C for multi-language bindings. */

#ifdef __cplusplus
extern "C" {
#endif

/* build variant macros are defined by the build system:
 *   WHALEUI_BUILD_FULL / WHALEUI_BUILD_LITE / WHALEUI_BUILD_MINIMAL */

#define WHALEUI_VERSION "0.1.0"

/* Returns the library build variant name: "full" | "lite" | "minimal". */
const char* whaleui_variant(void);

/* Returns the compiled-in version string. */
const char* whaleui_version(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_WHALEUI_H */
