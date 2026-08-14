#ifndef WHALEUI_WHALEUI_H
#define WHALEUI_WHALEUI_H

/* WhaleUI public C API.
 *
 * C++14 core, exposed as plain C for multi-language/multi-OS bindings.
 * This is the ONLY public header; internal interfaces live in src/ per module.
 *
 * Conventions:
 *   - All functions are `whaleui_` prefixed C functions.
 *   - int returns: 0 = success, non-zero = failure.
 *   - Handle returns: NULL = failure.
 *   - Strings returned are owned by the library and valid until the owning
 *     object is destroyed.
 *
 * Virtual file system note:
 *   ALL file operations (HTML/CSS/image/font loading) go through the virtual
 *   file system (whaleui_fs_*). The default loader reads from disk (file://
 *   and bare paths). Users may replace the loader entirely (e.g. HTTP(S) for
 *   CDN-hosted CSS) via whaleui_fs_set_loader(). Font registration also goes
 *   through the VFS - see whaleui_font_register().
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h> /* size_t */

/* build variant macros are defined by the build system:
 *   WHALEUI_BUILD_FULL / WHALEUI_BUILD_LITE / WHALEUI_BUILD_MINIMAL */

#define WHALEUI_VERSION "0.1.0"

/* ======================= build info ======================= */

/* Returns the library build variant name: "full" | "lite" | "minimal". */
const char* whaleui_variant(void);

/* Returns the compiled-in version string. */
const char* whaleui_version(void);

/* ======================= application ======================= */

typedef struct whaleui_app whaleui_app_t;

/* Create/destroy the application (singleton-ish: one per process is typical). */
whaleui_app_t* whaleui_app_create(void);
void whaleui_app_destroy(whaleui_app_t* app);

/* Enter the event loop (blocks). Returns 0 on clean quit. */
int whaleui_app_run(whaleui_app_t* app);

/* Request the event loop to quit. */
void whaleui_app_quit(whaleui_app_t* app);

/* ======================= theme ======================= */

typedef enum whaleui_theme {
    WHALEUI_THEME_SYSTEM = 0, /* follow system (default) */
    WHALEUI_THEME_LIGHT,
    WHALEUI_THEME_DARK
} whaleui_theme_t;

/* Set the color scheme; WHALEUI_THEME_SYSTEM follows the OS. */
int whaleui_app_set_theme(whaleui_app_t* app, whaleui_theme_t theme);
whaleui_theme_t whaleui_app_get_theme(const whaleui_app_t* app);

/* The EFFECTIVE theme: WHALEUI_THEME_SYSTEM resolves to the OS scheme that
 * was detected at app creation (defaults to light when undetectable). */
whaleui_theme_t whaleui_app_resolved_theme(const whaleui_app_t* app);

/* Accent (theme) color, "#RRGGBB" or "#AARRGGBB". */
int whaleui_app_set_accent_color(whaleui_app_t* app, const char* hex);

/* ======================= theme style ======================= */

/* Built-in theme styles: "fluent" (default), "metro", "material",
 * "classic", "aero", "gtk", "macos". The style changes the whole UI
 * (default stylesheet + light/dark variables) for every window, including
 * elements the page does not style itself. */
int whaleui_theme_count(void);
const char* whaleui_theme_name(int index);   /* "fluent", ... */
const char* whaleui_theme_label(int index);  /* "Fluent (Win11)", ... */

int whaleui_app_set_theme_style(whaleui_app_t* app, const char* style);
const char* whaleui_app_get_theme_style(const whaleui_app_t* app);

/* ======================= controls ======================= */

/* Called when a <select> option is chosen (value = the option's value). */
typedef void (*whaleui_select_cb)(whaleui_app_t* app, const char* value,
                                  void* userdata);
int whaleui_app_set_select_callback(whaleui_app_t* app, whaleui_select_cb cb,
                                    void* userdata);

/* Called on every key event. keycode is one of the WHALEUI_KEY_* values
 * below (SDL keycodes passed through; no SDL headers needed to use them);
 * pressed is 1 on key-down, 0 on key-up. Key handling stays in the app
 * (the library only dispatches). */
typedef void (*whaleui_key_cb)(whaleui_app_t* app, int keycode, int pressed,
                               void* userdata);
int whaleui_app_set_key_callback(whaleui_app_t* app, whaleui_key_cb cb,
                                 void* userdata);

/* Key codes delivered to the key callback (no SDL dependency). */
enum {
    WHALEUI_KEY_ESCAPE = 27,
    WHALEUI_KEY_ENTER  = 13,
    WHALEUI_KEY_SPACE  = 32,
    WHALEUI_KEY_TAB    = 9,
    WHALEUI_KEY_BACKSPACE = 8,
    WHALEUI_KEY_UP     = 1073741906,
    WHALEUI_KEY_DOWN   = 1073741905,
    WHALEUI_KEY_LEFT   = 1073741904,
    WHALEUI_KEY_RIGHT  = 1073741903,
    WHALEUI_KEY_HOME   = 1073741898,
    WHALEUI_KEY_END    = 1073741901,
    WHALEUI_KEY_DELETE = 127,
    WHALEUI_KEY_A = 'a', WHALEUI_KEY_B = 'b', WHALEUI_KEY_C = 'c',
    WHALEUI_KEY_D = 'd', WHALEUI_KEY_E = 'e', WHALEUI_KEY_F = 'f',
    WHALEUI_KEY_G = 'g', WHALEUI_KEY_H = 'h', WHALEUI_KEY_I = 'i',
    WHALEUI_KEY_J = 'j', WHALEUI_KEY_K = 'k', WHALEUI_KEY_L = 'l',
    WHALEUI_KEY_M = 'm', WHALEUI_KEY_N = 'n', WHALEUI_KEY_O = 'o',
    WHALEUI_KEY_P = 'p', WHALEUI_KEY_Q = 'q', WHALEUI_KEY_R = 'r',
    WHALEUI_KEY_S = 's', WHALEUI_KEY_T = 't', WHALEUI_KEY_U = 'u',
    WHALEUI_KEY_V = 'v', WHALEUI_KEY_W = 'w', WHALEUI_KEY_X = 'x',
    WHALEUI_KEY_Y = 'y', WHALEUI_KEY_Z = 'z',
};

/* ======================= render options ======================= */

typedef enum whaleui_render_option {
    WHALEUI_RENDER_MAX_FPS = 0,  /* int: fps cap, 0 = unlimited */
    WHALEUI_RENDER_BATTERY_SAVER,/* int: 1 = battery saver (default 60fps) */
    WHALEUI_RENDER_VSYNC         /* int: 1 = vsync on, 0 = off */
} whaleui_render_option_t;

int whaleui_app_set_render_option(whaleui_app_t* app,
                                  whaleui_render_option_t opt, int value);

/* Global text scale for all windows (font-size multiplier, 1.0 = 100%,
 * e.g. 1.25 = 125%). Relayouts and repaints. Returns 0 on success. */
int whaleui_app_set_text_scale(whaleui_app_t* app, float scale);

/* ======================= window ======================= */

typedef struct whaleui_window whaleui_window_t;

/* Create a window inside the app. */
whaleui_window_t* whaleui_window_create(whaleui_app_t* app,
                                        const char* title, int width, int height);
void whaleui_window_destroy(whaleui_window_t* win);

int whaleui_window_show(whaleui_window_t* win);
int whaleui_window_hide(whaleui_window_t* win);
int whaleui_window_close(whaleui_window_t* win);

int whaleui_window_set_title(whaleui_window_t* win, const char* title);
const char* whaleui_window_get_title(const whaleui_window_t* win);
int whaleui_window_set_size(whaleui_window_t* win, int width, int height);
int whaleui_window_get_size(const whaleui_window_t* win, int* w, int* h);

/* Load HTML from a string. */
int whaleui_window_load_html(whaleui_window_t* win, const char* html);

/* Load HTML/CSS from a URI through the virtual file system. */
int whaleui_window_load_uri(whaleui_window_t* win, const char* uri);

struct whaleui_dom_document;
typedef struct whaleui_dom_document whaleui_dom_document_t;
whaleui_dom_document_t* whaleui_window_get_document(whaleui_window_t* win);

/* ======================= DOM ======================= */

typedef struct whaleui_dom_element whaleui_dom_element_t;

/* Parse an HTML string into a document. */
whaleui_dom_document_t* whaleui_dom_parse_html(const char* html, size_t len);
void whaleui_dom_document_destroy(whaleui_dom_document_t* doc);

/* Queries (mirror document.* / element.*). */
whaleui_dom_element_t* whaleui_dom_get_element_by_id(whaleui_dom_document_t* doc,
                                                     const char* id);
whaleui_dom_element_t* whaleui_dom_query_selector(whaleui_dom_document_t* doc,
                                                  const char* selector);
whaleui_dom_element_t* whaleui_dom_document_element(whaleui_dom_document_t* doc);
whaleui_dom_element_t* whaleui_dom_parent(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_first_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_next_sibling(whaleui_dom_element_t* el);

/* Mutation. */
whaleui_dom_element_t* whaleui_dom_create_element(whaleui_dom_document_t* doc,
                                                  const char* tag);
int whaleui_dom_append_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);
int whaleui_dom_remove_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);
int whaleui_dom_element_destroy(whaleui_dom_element_t* el);

/* Attributes. */
int whaleui_dom_set_attribute(whaleui_dom_element_t* el, const char* name, const char* value);
const char* whaleui_dom_get_attribute(whaleui_dom_element_t* el, const char* name);

/* Text content. */
int whaleui_dom_set_text(whaleui_dom_element_t* el, const char* text);
const char* whaleui_dom_get_text(whaleui_dom_element_t* el);

/* Inline style (el.style.xxx). */
int whaleui_dom_set_style(whaleui_dom_element_t* el, const char* property, const char* value);
const char* whaleui_dom_get_style(whaleui_dom_element_t* el, const char* property);

/* Tag name (lowercase). */
const char* whaleui_dom_tag_name(whaleui_dom_element_t* el);

/* ======================= CSS ======================= */

typedef struct whaleui_css_rule whaleui_css_rule_t;

/* Parse CSS from a string into an array of rules. */
int whaleui_css_parse(whaleui_css_rule_t** rules, size_t* count,
                      const char* css, size_t len);

/* Load and parse CSS from a URI through the virtual file system. */
int whaleui_css_load(whaleui_css_rule_t** rules, size_t* count, const char* uri);

void whaleui_css_rules_destroy(whaleui_css_rule_t* rules, size_t count);

const char* whaleui_css_selector(const whaleui_css_rule_t* rule);
const char* whaleui_css_get_property(const whaleui_css_rule_t* rule, const char* name);
int         whaleui_css_has_property(const whaleui_css_rule_t* rule, const char* name);

/* Apply a set of rules to a document (selector matching). */
int whaleui_css_apply(whaleui_dom_document_t* doc,
                      const whaleui_css_rule_t* rules, size_t count);

/* ======================= virtual file system ======================= */

/* All file operations go through the VFS. Default loader reads from disk
 * (file:// and bare paths). Replace the loader to support other protocols
 * (e.g. HTTP(S) for CDN-hosted resources). */

typedef void* (*whaleui_fs_open_fn)(const char* uri, void* userdata);
typedef size_t (*whaleui_fs_read_fn)(void* handle, char* buf, size_t size, void* userdata);
typedef void  (*whaleui_fs_close_fn)(void* handle, void* userdata);

/* Replace the global loader. Passing NULL restores the default disk loader. */
int whaleui_fs_set_loader(whaleui_fs_open_fn open, whaleui_fs_read_fn read,
                          whaleui_fs_close_fn close, void* userdata);

/* Load the whole uri into memory (malloc'd, caller frees). */
int whaleui_fs_load(const char* uri, char** out, size_t* outlen);

/* ======================= font ======================= */

/* Font loading goes through the VFS (see whaleui_fs_*). Defaults to a
 * platform system-font fallback; register custom fonts explicitly. */

/* Register a font file from a URI (through the VFS). Returns the family name
 * (owned by the library), or NULL on failure. */
const char* whaleui_font_register(const char* uri);

/* Register a font from memory (data NOT copied; caller must keep it alive).
 * Returns the family name, or NULL on failure. */
const char* whaleui_font_register_memory(const unsigned char* data, size_t len);

/* Set the default font family used when CSS specifies none. */
int whaleui_font_set_default(const char* family);
const char* whaleui_font_get_default(void);

/* Comma-separated list of registered families (for debugging/enumeration). */
const char* whaleui_font_list(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_WHALEUI_H */
