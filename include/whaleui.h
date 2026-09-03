#ifndef WHALEUI_WHALEUI_H
#define WHALEUI_WHALEUI_H

/* =========================================================================
 * WhaleUI public C API.
 *
 * C++14 core, exposed as plain C for multi-language/multi-OS bindings.
 * This is the ONLY public header; internal interfaces live in src/ per module.
 *
 * Conventions:
 *   - All functions are `whaleui_` prefixed C functions.
 *   - int returns: 0 = success, non-zero = failure.
 *   - Handle returns: NULL = failure.
 *   - Strings returned are owned by the library and valid until the owning
 *     object is destroyed (do not free, do not keep past the owner's life).
 *   - Arrays / lists returned must be released with the matching destroy
 *     function (e.g. whaleui_dom_list_destroy()).
 *   - All callbacks are invoked on the thread that runs the event loop
 *     (whaleui_app_run) and must not block.
 *
 * DOM model:
 *   The DOM mirrors the JavaScript DOM (querySelector, addEventListener, ...)
 *   so bindings to script engines map 1:1. Elements are live: mutations
 *   (append/remove/attribute/style/text) take effect immediately and are
 *   reflected on the next repaint. Element handles remain valid while the
 *   owning document lives and the element is attached to it; destroying the
 *   document invalidates every handle under it.
 *
 * Implementation status:
 *   The core (app/window/theme/render/fs/font + basic DOM/CSS) is
 *   implemented. The DOM extensions (query/traversal/mutation/event/geometry)
 *   and window extensions declared below are DOCUMENTED but not yet
 *   implemented unless marked otherwise - see doc/more-dom-api.md for the
 *   hand-off checklist. Their signatures are final; do not change them
 *   without updating that document.
 *
 * Virtual file system note:
 *   ALL file operations (HTML/CSS/image/font loading) go through the virtual
 *   file system (whaleui_fs_*). The default loader reads from disk (file://
 *   and bare paths). Users may replace the loader entirely (e.g. HTTP(S) for
 *   CDN-hosted CSS) via whaleui_fs_set_loader(). Font registration also goes
 *   through the VFS - see whaleui_font_register().
 * ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h> /* size_t */

/* build variant macros are defined by the build system:
 *   WHALEUI_BUILD_FULL / WHALEUI_BUILD_LITE / WHALEUI_BUILD_MINIMAL */

#define WHALEUI_VERSION "0.96"

/* ======================= build info ======================= */

/* Returns the library build variant name: "full" | "lite" | "minimal". */
const char* whaleui_variant(void);

/* Returns the compiled-in version string. */
const char* whaleui_version(void);

/* ======================= application ======================= */

typedef struct whaleui_app whaleui_app_t;

/* Create/destroy the application (singleton-ish: one per process is typical).
 * whaleui_app_create() captures the OS color scheme and initializes the
 * platform backend; returns NULL on platform init failure. */
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

/* Set the color scheme; WHALEUI_THEME_SYSTEM follows the OS.
 * Takes effect immediately on all windows (stylesheets reload + repaint). */
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
    WHALEUI_RENDER_VSYNC,        /* int: 1 = vsync on, 0 = off */
    WHALEUI_RENDER_ASYNC_LAYOUT  /* int: 1 = first layout on a worker thread
                                    (window stays responsive while a large
                                    page lays out; opt-in) */
} whaleui_render_option_t;

int whaleui_app_set_render_option(whaleui_app_t* app,
                                  whaleui_render_option_t opt, int value);

/* Global text scale for all windows (font-size multiplier, 1.0 = 100%,
 * e.g. 1.25 = 125%). Relayouts and repaints. Returns 0 on success. */
int whaleui_app_set_text_scale(whaleui_app_t* app, float scale);

/* Set the prefers-reduced-motion preference (1 = reduce). Pages with a
 * "reduce" branch (e.g. reveal-on-scroll content without JS) then show
 * their static content. Reloads the stylesheet. Returns 0 on success. */
int whaleui_app_set_reduced_motion(whaleui_app_t* app, int reduce);

/* ======================= window ======================= */

typedef struct whaleui_window whaleui_window_t;

/* Create a window inside the app. The native window is created lazily on the
 * first show(); until then title/size are cached and applied at creation.
 * Returns NULL on invalid arguments. */
whaleui_window_t* whaleui_window_create(whaleui_app_t* app,
                                        const char* title, int width, int height);
void whaleui_window_destroy(whaleui_window_t* win);

/* --- display state --- */

/* Show/hide the window. show() creates the native window on first call.
 * Returns 0 on success; the window is destroyed on close(). */
int whaleui_window_show(whaleui_window_t* win);
int whaleui_window_hide(whaleui_window_t* win);

/* Close and tear down the native window + render context. The window handle
 * stays valid (document kept); call whaleui_window_destroy() to free it.
 * This is what the CLOSE window event performs by default. */
int whaleui_window_close(whaleui_window_t* win);

int whaleui_window_is_visible(const whaleui_window_t* win);
int whaleui_window_is_minimized(const whaleui_window_t* win);
int whaleui_window_is_maximized(const whaleui_window_t* win);
int whaleui_window_is_focused(const whaleui_window_t* win);

int whaleui_window_minimize(whaleui_window_t* win);
int whaleui_window_maximize(whaleui_window_t* win);
int whaleui_window_restore(whaleui_window_t* win);

/* Raise the window to the top of the desktop stacking order. */
int whaleui_window_raise(whaleui_window_t* win);

/* --- title / geometry (logical pixels; see get_size_in_pixels) --- */

int whaleui_window_set_title(whaleui_window_t* win, const char* title);
const char* whaleui_window_get_title(const whaleui_window_t* win);

int whaleui_window_set_size(whaleui_window_t* win, int width, int height);
int whaleui_window_get_size(const whaleui_window_t* win, int* w, int* h);

/* Physical (backbuffer) size, differs from get_size() under display
 * scaling / high-DPI. Returns 0 on success. */
int whaleui_window_get_size_in_pixels(const whaleui_window_t* win, int* w, int* h);

/* Position in screen (desktop) coordinates. Returns 0 on success. */
int whaleui_window_set_position(whaleui_window_t* win, int x, int y);
int whaleui_window_get_position(const whaleui_window_t* win, int* x, int* y);

/* Center the window on the display it currently occupies (or the primary
 * display before the first show). Returns 0 on success. */
int whaleui_window_center(whaleui_window_t* win);

/* Minimum/maximum client size constraints (0 = no constraint). */
int whaleui_window_set_min_size(whaleui_window_t* win, int width, int height);
int whaleui_window_get_min_size(const whaleui_window_t* win, int* w, int* h);
int whaleui_window_set_max_size(whaleui_window_t* win, int width, int height);
int whaleui_window_get_max_size(const whaleui_window_t* win, int* w, int* h);

/* --- appearance --- */

/* bordered: 1 = system frame + title bar (default), 0 = borderless. */
int whaleui_window_set_bordered(whaleui_window_t* win, int bordered);

/* resizable: 1 = user can resize (default), 0 = fixed. */
int whaleui_window_set_resizable(whaleui_window_t* win, int resizable);

/* Always-on-top: 1 = window stays above other windows, 0 = normal. */
int whaleui_window_set_always_on_top(whaleui_window_t* win, int on_top);

/* Opacity 0.0 (transparent) .. 1.0 (opaque, default). */
int   whaleui_window_set_opacity(whaleui_window_t* win, float opacity);
float whaleui_window_get_opacity(const whaleui_window_t* win);

/* Set the window icon from raw RGBA pixels (data copied by the library).
 * Returns 0 on success. */
int whaleui_window_set_icon(whaleui_window_t* win,
                            const unsigned char* rgba, int width, int height);

/* Load the icon from an image file through the virtual file system
 * (full: SDL_image; lite: stb_image; minimal: unsupported). */
int whaleui_window_set_icon_uri(whaleui_window_t* win, const char* uri);

/* --- fullscreen --- */

typedef enum whaleui_fullscreen_mode {
    WHALEUI_FULLSCREEN_OFF = 0,
    WHALEUI_FULLSCREEN_DESKTOP, /* fill the desktop resolution (recommended) */
    WHALEUI_FULLSCREEN_REAL     /* exclusive: switch the display mode */
} whaleui_fullscreen_mode_t;

int whaleui_window_set_fullscreen(whaleui_window_t* win,
                                  whaleui_fullscreen_mode_t mode);
whaleui_fullscreen_mode_t whaleui_window_get_fullscreen(const whaleui_window_t* win);

/* --- input focus / mouse capture --- */

/* Grab the mouse into the window (1) or release (0); keyboard grab is a
 * stronger form that also routes keyboard input exclusively to the window. */
int whaleui_window_set_mouse_grab(whaleui_window_t* win, int grab);
int whaleui_window_get_mouse_grab(const whaleui_window_t* win);
int whaleui_window_set_keyboard_grab(whaleui_window_t* win, int grab);
int whaleui_window_get_keyboard_grab(const whaleui_window_t* win);

/* Move the host cursor to (x, y) in window coordinates. */
int whaleui_window_set_mouse_position(whaleui_window_t* win, int x, int y);

/* focusable: 1 = window can receive keyboard focus (default), 0 = never. */
int whaleui_window_set_focusable(whaleui_window_t* win, int focusable);

/* --- parent / modal (platform dependent: Windows/macOS/X11) --- */

/* parent: attach win as a child of parent (NULL clears). */
int whaleui_window_set_parent(whaleui_window_t* win, whaleui_window_t* parent);

/* modal: make win a modal dialog over parent (blocks input to parent). */
int whaleui_window_set_modal(whaleui_window_t* win, whaleui_window_t* parent);

/* --- display info --- */

/* ID of the display the window is on (-1 = not available). */
int whaleui_window_display_id(const whaleui_window_t* win);

/* Logical-to-physical pixel scale of the window's display (1.0 = 100%). */
float whaleui_window_display_scale(const whaleui_window_t* win);

/* --- taskbar notification --- */

typedef enum whaleui_flash {
    WHALEUI_FLASH_CANCEL = 0,        /* stop flashing */
    WHALEUI_FLASH_BRIEFLY,           /* flash a few times, then stop */
    WHALEUI_FLASH_UNTIL_FOCUSED      /* keep flashing until the window gains focus */
} whaleui_flash_t;

/* Request attention for a background window (platform dependent). */
int whaleui_window_flash(whaleui_window_t* win, whaleui_flash_t mode);

/* --- window events --- */

typedef enum whaleui_window_event {
    WHALEUI_WINDOW_EVENT_CLOSE = 0,      /* user clicked the close button; returning
                                            non-zero cancels the default close */
    WHALEUI_WINDOW_EVENT_RESIZED,        /* a = width, b = height (logical px) */
    WHALEUI_WINDOW_EVENT_MOVED,          /* a = x, b = y (screen coords) */
    WHALEUI_WINDOW_EVENT_SHOWN,
    WHALEUI_WINDOW_EVENT_HIDDEN,
    WHALEUI_WINDOW_EVENT_MINIMIZED,
    WHALEUI_WINDOW_EVENT_RESTORED,
    WHALEUI_WINDOW_EVENT_MAXIMIZED,
    WHALEUI_WINDOW_EVENT_FOCUS_GAINED,
    WHALEUI_WINDOW_EVENT_FOCUS_LOST,
    WHALEUI_WINDOW_EVENT_MOUSE_ENTER,
    WHALEUI_WINDOW_EVENT_MOUSE_LEAVE
} whaleui_window_event_t;

/* Window event callback. Return 0 to allow the default behavior (only
 * WHALEUI_WINDOW_EVENT_CLOSE has one: close the window); return non-zero to
 * cancel it. a/b carry per-event payloads (see the enum above). */
typedef int (*whaleui_window_event_cb)(whaleui_window_t* win,
                                       whaleui_window_event_t ev,
                                       int a, int b, void* userdata);
int whaleui_window_set_event_callback(whaleui_window_t* win,
                                      whaleui_window_event_cb cb, void* userdata);

/* --- content --- */

/* forward declaration; full type + API in the DOM section below */
struct whaleui_dom_document;
typedef struct whaleui_dom_document whaleui_dom_document_t;

/* Load HTML from a string (replaces the current document). */
int whaleui_window_load_html(whaleui_window_t* win, const char* html);

/* Load HTML/CSS from a URI through the virtual file system. */
int whaleui_window_load_uri(whaleui_window_t* win, const char* uri);

/* Current document of the window (NULL before any load). The document is
 * owned by the window; destroying the window destroys it. */
whaleui_dom_document_t* whaleui_window_get_document(whaleui_window_t* win);

/* ======================= DOM ======================= */

typedef struct whaleui_dom_element whaleui_dom_element_t;

/* Parse an HTML string into a document. len = byte length (0 = NUL-terminated).
 * The document owns all elements under it. */
whaleui_dom_document_t* whaleui_dom_parse_html(const char* html, size_t len);
void whaleui_dom_document_destroy(whaleui_dom_document_t* doc);

/* --- element collections (document.querySelectorAll etc.) --- */

/* A live-until-destroyed element list. Destroy with whaleui_dom_list_destroy().
 * Lists are snapshots: they do not update as the DOM mutates. An empty result
 * is a valid non-NULL list of length 0. */
typedef struct whaleui_dom_list whaleui_dom_list_t;

size_t whaleui_dom_list_length(const whaleui_dom_list_t* list);
/* Element at index (NULL if out of range). */
whaleui_dom_element_t* whaleui_dom_list_item(const whaleui_dom_list_t* list,
                                             size_t index);
void whaleui_dom_list_destroy(whaleui_dom_list_t* list);

/* --- document-level queries (mirror document.*) --- */

whaleui_dom_element_t* whaleui_dom_get_element_by_id(whaleui_dom_document_t* doc,
                                                     const char* id);
whaleui_dom_element_t* whaleui_dom_query_selector(whaleui_dom_document_t* doc,
                                                  const char* selector);
whaleui_dom_list_t* whaleui_dom_query_selector_all(whaleui_dom_document_t* doc,
                                                   const char* selector);
whaleui_dom_list_t* whaleui_dom_get_elements_by_class_name(whaleui_dom_document_t* doc,
                                                           const char* class_name);
whaleui_dom_list_t* whaleui_dom_get_elements_by_tag_name(whaleui_dom_document_t* doc,
                                                         const char* tag);

/* document.documentElement / body / head (NULL when absent). */
whaleui_dom_element_t* whaleui_dom_document_element(whaleui_dom_document_t* doc);
whaleui_dom_element_t* whaleui_dom_body(whaleui_dom_document_t* doc);
whaleui_dom_element_t* whaleui_dom_head(whaleui_dom_document_t* doc);

/* document.title (the <title> text; set creates <title> in <head> if missing). */
int whaleui_dom_set_title(whaleui_dom_document_t* doc, const char* title);
const char* whaleui_dom_get_title(whaleui_dom_document_t* doc);

/* document.activeElement (the focused element, NULL if none). */
whaleui_dom_element_t* whaleui_dom_active_element(whaleui_dom_document_t* doc);

/* --- element-level queries (mirror element.*) --- */

whaleui_dom_element_t* whaleui_dom_element_query_selector(whaleui_dom_element_t* el,
                                                          const char* selector);
whaleui_dom_list_t* whaleui_dom_element_query_selector_all(whaleui_dom_element_t* el,
                                                           const char* selector);
whaleui_dom_list_t* whaleui_dom_element_get_elements_by_class_name(whaleui_dom_element_t* el,
                                                                   const char* class_name);
whaleui_dom_list_t* whaleui_dom_element_get_elements_by_tag_name(whaleui_dom_element_t* el,
                                                                 const char* tag);

/* element.closest(selector): nearest ancestor (incl. self) matching; NULL. */
whaleui_dom_element_t* whaleui_dom_closest(whaleui_dom_element_t* el,
                                           const char* selector);

/* element.matches(selector): 1 if el itself matches, 0 otherwise. */
int whaleui_dom_matches(whaleui_dom_element_t* el, const char* selector);

/* --- tree traversal (mirror element.*) --- */

whaleui_dom_element_t* whaleui_dom_parent(whaleui_dom_element_t* el);        /* parentNode */
whaleui_dom_element_t* whaleui_dom_parent_element(whaleui_dom_element_t* el);/* parentElement */
whaleui_dom_element_t* whaleui_dom_first_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_last_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_next_sibling(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_previous_sibling(whaleui_dom_element_t* el);

/* Element-only variants (skip text/comment nodes). */
whaleui_dom_element_t* whaleui_dom_first_element_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_last_element_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_next_element_sibling(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_previous_element_sibling(whaleui_dom_element_t* el);

/* element.children: element children only. */
whaleui_dom_list_t* whaleui_dom_children(whaleui_dom_element_t* el);
size_t whaleui_dom_child_element_count(whaleui_dom_element_t* el);

/* --- mutation (mirror element.appendChild etc.) --- */

whaleui_dom_element_t* whaleui_dom_create_element(whaleui_dom_document_t* doc,
                                                  const char* tag);
/* document.createTextNode (append with whaleui_dom_append_child). */
whaleui_dom_element_t* whaleui_dom_create_text_node(whaleui_dom_document_t* doc,
                                                    const char* text);

int whaleui_dom_append_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);
int whaleui_dom_remove_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);
int whaleui_dom_element_destroy(whaleui_dom_element_t* el);

/* parent.insertBefore(new_child, ref_child); ref_child NULL appends. */
int whaleui_dom_insert_before(whaleui_dom_element_t* parent,
                              whaleui_dom_element_t* new_child,
                              whaleui_dom_element_t* ref_child);

/* parent.replaceChild(new_child, old_child). */
int whaleui_dom_replace_child(whaleui_dom_element_t* parent,
                              whaleui_dom_element_t* new_child,
                              whaleui_dom_element_t* old_child);

/* el.remove(): detach el from its parent (the element itself stays alive
 * and can be re-appended). Returns 0 on success, non-zero if unattached. */
int whaleui_dom_remove(whaleui_dom_element_t* el);

/* element.cloneNode(deep): 1 = clone subtree, 0 = shallow. Detached clone. */
whaleui_dom_element_t* whaleui_dom_clone(whaleui_dom_element_t* el, int deep);

/* parent.contains(child): 1 if child is el or a descendant. */
int whaleui_dom_contains(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);

/* hasChildNodes: 1 if el has any child node (incl. text). */
int whaleui_dom_has_child_nodes(whaleui_dom_element_t* el);

/* isConnected: 1 if el is attached to a document. */
int whaleui_dom_is_connected(whaleui_dom_element_t* el);

/* --- attributes (mirror setAttribute / getAttribute etc.) --- */

int whaleui_dom_set_attribute(whaleui_dom_element_t* el, const char* name, const char* value);
const char* whaleui_dom_get_attribute(whaleui_dom_element_t* el, const char* name);
int whaleui_dom_has_attribute(whaleui_dom_element_t* el, const char* name);
int whaleui_dom_remove_attribute(whaleui_dom_element_t* el, const char* name);
/* element.toggleAttribute(name, force): force >= 0 forces on (1) / off (0);
 * force < 0 toggles. Returns the resulting presence (1/0), -1 on error. */
int whaleui_dom_toggle_attribute(whaleui_dom_element_t* el, const char* name, int force);

/* --- class list (mirror element.classList.*) --- */

/* Returns 1 on success / 0 on failure (or, for *_contains, presence). */
int whaleui_dom_class_add(whaleui_dom_element_t* el, const char* cls);
int whaleui_dom_class_remove(whaleui_dom_element_t* el, const char* cls);
int whaleui_dom_class_toggle(whaleui_dom_element_t* el, const char* cls); /* returns resulting presence */
int whaleui_dom_class_contains(whaleui_dom_element_t* el, const char* cls);

/* --- text / html content --- */

/* element.textContent. */
int whaleui_dom_set_text(whaleui_dom_element_t* el, const char* text);
const char* whaleui_dom_get_text(whaleui_dom_element_t* el);

/* element.innerHTML: get serializes children; set parses and replaces
 * children (fragment parsing, mirrors JS). */
int whaleui_dom_set_inner_html(whaleui_dom_element_t* el, const char* html);
const char* whaleui_dom_get_inner_html(whaleui_dom_element_t* el);

/* element.outerHTML: get serializes el incl. itself; set replaces el. */
int whaleui_dom_set_outer_html(whaleui_dom_element_t* el, const char* html);
const char* whaleui_dom_get_outer_html(whaleui_dom_element_t* el);

/* --- form value (input/textarea/select) --- */

/* The current control value, mirrors the JS `.value` property (not the value
 * attribute; for <select> the chosen option's value). Non-form elements
 * return failure/NULL. */
int whaleui_dom_set_value(whaleui_dom_element_t* el, const char* value);
const char* whaleui_dom_get_value(whaleui_dom_element_t* el);

/* --- inline style (el.style.xxx) --- */

int whaleui_dom_set_style(whaleui_dom_element_t* el, const char* property, const char* value);
const char* whaleui_dom_get_style(whaleui_dom_element_t* el, const char* property);

/* Tag name (lowercase; "input", "div", ...). */
const char* whaleui_dom_tag_name(whaleui_dom_element_t* el);

/* --- focus --- */

/* Focus/blur an element (mirrors el.focus() / el.blur()). */
int whaleui_dom_focus(whaleui_dom_element_t* el);
int whaleui_dom_blur(whaleui_dom_element_t* el);

/* --- geometry (mirrors getBoundingClientRect) --- */

typedef struct whaleui_dom_rect {
    float x, y;        /* top-left, viewport coordinates */
    float width, height;
} whaleui_dom_rect_t;

int whaleui_dom_get_bounding_client_rect(whaleui_dom_element_t* el,
                                         whaleui_dom_rect_t* out);

/* ======================= DOM events ======================= */

/* Event listeners mirror addEventListener(type, listener) with a plain C
 * callback. `type` is a lowercase DOM event name; supported types:
 *
 *   Mouse:    click, dblclick, mousedown, mouseup, mousemove,
 *             mouseenter, mouseleave, contextmenu, wheel
 *   Keyboard: keydown, keyup
 *   Focus:    focus, blur
 *   Form:     change, input, submit
 *   Document: load, scroll, resize
 *
 * Dispatch model: listeners fire on the target element only (no bubbling /
 * capture phase yet - see doc/more-dom-api.md). The callback receives a
 * transient event object valid for the duration of the call only. */

typedef struct whaleui_dom_event whaleui_dom_event_t;

/* listener: (event, userdata). userdata must match at removal. */
typedef void (*whaleui_event_cb)(whaleui_dom_event_t* ev, void* userdata);

/* Register a listener. Returns 0 on success (duplicate type+cb+userdata is
 * a no-op success). */
int whaleui_dom_add_event_listener(whaleui_dom_element_t* el, const char* type,
                                   whaleui_event_cb cb, void* userdata);
int whaleui_dom_remove_event_listener(whaleui_dom_element_t* el, const char* type,
                                      whaleui_event_cb cb, void* userdata);

/* Synchronously dispatch a synthetic event of `type` on el. Returns 0 on
 * success (listeners may call prevent_default). */
int whaleui_dom_dispatch_event(whaleui_dom_element_t* el, const char* type);

/* --- event object accessors (valid inside the callback only) --- */

/* The element the event was dispatched on. */
whaleui_dom_element_t* whaleui_dom_event_target(const whaleui_dom_event_t* ev);

/* The event type string ("click", ...). */
const char* whaleui_dom_event_type(const whaleui_dom_event_t* ev);

/* Mark the event handled / stop it reaching the default behavior. */
int whaleui_dom_event_prevent_default(whaleui_dom_event_t* ev);
int whaleui_dom_event_stop_propagation(whaleui_dom_event_t* ev);
int whaleui_dom_event_default_prevented(const whaleui_dom_event_t* ev);

/* Keyboard: WHALEUI_KEY_* value. */
int whaleui_dom_event_key_code(const whaleui_dom_event_t* ev);

/* Mouse: viewport coordinates / button (0 = left, 1 = middle, 2 = right). */
int whaleui_dom_event_mouse_x(const whaleui_dom_event_t* ev);
int whaleui_dom_event_mouse_y(const whaleui_dom_event_t* ev);
int whaleui_dom_event_mouse_button(const whaleui_dom_event_t* ev);

/* Wheel: scroll delta (1 = one notch; pixels on high-resolution wheels). */
float whaleui_dom_event_wheel_delta_x(const whaleui_dom_event_t* ev);
float whaleui_dom_event_wheel_delta_y(const whaleui_dom_event_t* ev);

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

/* One-shot: register the current platform's common UI fonts (Latin, CJK,
 * emoji candidates) as a lazy fallback chain. Only the file PATHS are
 * recorded - no font data is loaded here and nothing is opened until a
 * page actually renders glyphs the default font cannot provide (see
 * render_text). Missing files are skipped, so this works on any system
 * (Win8 without Segoe UI still gets Arial/SimSun). Idempotent; returns
 * the number of fonts newly registered. */
int whaleui_font_register_system_defaults(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_WHALEUI_H */

