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
 *   The core (app/window/theme/render/fs/font + DOM/CSS) is implemented,
 *   including the DOM extensions (query/traversal/mutation/event/geometry,
 *   CSSStyleDeclaration + getComputedStyle), the full window extension set
 *   with HitTest, Canvas 2D and the WebGPU stub backend. C++ users take
 *   whaleui.hpp (namespace whaleui), the C++ API this C surface is the
 *   export layer of; see doc/internal/more-dom-api.md for the hand-off
 *   checklist of remaining gaps. Signatures are final; do not change them
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

/* --- visual viewport (window.visualViewport subset) ---
 *
 * The engine has no pinch zoom: the visual viewport equals the layout
 * viewport, so scale is 1.0 and the offsets are 0. The struct exists so
 * pages can branch on it (and so the values are right if zoom lands later);
 * width/height follow the window's logical size. `resize` is fired on the
 * document element whenever the window resizes. */
typedef struct whaleui_visual_viewport {
    float width, height;             /* visual viewport size (CSS px) */
    float scale;                     /* 1.0 = no pinch zoom */
    float offset_left, offset_top;   /* visual relative to the layout viewport */
    float page_left, page_top;       /* visual relative to the document */
} whaleui_visual_viewport_t;

int whaleui_window_visual_viewport(const whaleui_window_t* win,
                                   whaleui_visual_viewport_t* out);

/* devicePixelRatio: physical pixels per CSS pixel for this window. */
float whaleui_window_device_pixel_ratio(const whaleui_window_t* win);

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
/* Per-window input overrides (preferred over the app-global callbacks).
 * When set on a window, that window's callback fires instead of the app one. */
int whaleui_window_set_key_callback(whaleui_window_t* win, whaleui_key_cb cb,
                                    void* userdata);
int whaleui_window_set_select_callback(whaleui_window_t* win, whaleui_select_cb cb,
                                       void* userdata);

/* --- hit test (borderless drag/resize; mirrors SDL_HitTestResult) --- */
typedef enum whaleui_hit_test_result {
    WHALEUI_HITTEST_NORMAL = 0,
    WHALEUI_HITTEST_DRAGGABLE = 1,
    WHALEUI_HITTEST_RESIZE_TOPLEFT = 2,
    WHALEUI_HITTEST_RESIZE_TOP = 3,
    WHALEUI_HITTEST_RESIZE_TOPRIGHT = 4,
    WHALEUI_HITTEST_RESIZE_RIGHT = 5,
    WHALEUI_HITTEST_RESIZE_BOTTOMRIGHT = 6,
    WHALEUI_HITTEST_RESIZE_BOTTOM = 7,
    WHALEUI_HITTEST_RESIZE_BOTTOMLEFT = 8,
    WHALEUI_HITTEST_RESIZE_LEFT = 9
} whaleui_hit_test_result_t;
typedef whaleui_hit_test_result_t (*whaleui_hit_test_fn)(whaleui_window_t* win,
                                                          int x, int y, void* userdata);
int whaleui_window_set_hit_test(whaleui_window_t* win, whaleui_hit_test_fn cb,
                                void* userdata);

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

/* --- selection (window.getSelection() subset) ---
 *
 * The engine's text selection lives in the render context (anchor/focus
 * element + byte offsets; set by mouse drag, double/triple click and the
 * caret in editable controls). These accessors expose it per window. */

struct whaleui_dom_element;
typedef struct whaleui_dom_element whaleui_dom_element_t;

/* Anchor / focus endpoints (NULL when nothing is selected). */
whaleui_dom_element_t* whaleui_window_sel_anchor_element(whaleui_window_t* win);
whaleui_dom_element_t* whaleui_window_sel_focus_element(whaleui_window_t* win);
/* Byte offsets into the endpoint element's text. */
int whaleui_window_sel_anchor_offset(whaleui_window_t* win);
int whaleui_window_sel_focus_offset(whaleui_window_t* win);
/* The selected text ("" when none). Returned pointer is owned by the window
 * and valid until the next selection API call on it. */
const char* whaleui_window_selected_text(whaleui_window_t* win);
/* Clear the selection (removeAllRanges). */
void whaleui_window_clear_selection(whaleui_window_t* win);
/* Select the whole text of one element (selectAllChildren); the element
 * must belong to the window's document. */
void whaleui_window_select_all_of(whaleui_window_t* win, whaleui_dom_element_t* el);

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

/* element.append/prepend(node): insert node as first/last child of el.
 * element.before/after(node): insert node as the sibling before/after el
 * (el must be attached). Accept an element or a text node. 0 on success. */
int whaleui_dom_append(whaleui_dom_element_t* el, whaleui_dom_element_t* node);
int whaleui_dom_prepend(whaleui_dom_element_t* el, whaleui_dom_element_t* node);
int whaleui_dom_before(whaleui_dom_element_t* el, whaleui_dom_element_t* node);
int whaleui_dom_after(whaleui_dom_element_t* el, whaleui_dom_element_t* node);

/* element.insertAdjacentHTML(position, html): position is
 * "beforebegin"|"afterbegin"|"beforeend"|"afterend". Parses the fragment and
 * inserts it at that position relative to el. 0 on success. */
int whaleui_dom_insert_adjacent_html(whaleui_dom_element_t* el,
                                     const char* position, const char* html);

/* element.insertAdjacentText(position, text): same positions as above, but
 * the text is inserted as a single text node (no HTML parsing). 0 on success. */
int whaleui_dom_insert_adjacent_text(whaleui_dom_element_t* el,
                                     const char* position, const char* text);

/* document.createDocumentFragment(): an out-of-tree node that owns appended
 * children; appending it moves its children (not the fragment) into the tree.
 * Destroy with whaleui_dom_element_destroy when done. */
whaleui_dom_element_t* whaleui_dom_create_document_fragment(whaleui_dom_document_t* doc);

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
/* classList.length / item(index) / replace(old, new) / toggle(cls, force).
 * class_item copies the token at index into out (0 on success); replace
 * returns 1 when the old token was present (all occurrences swapped);
 * toggle_force mirrors toggle(cls, force): force >= 0 forces on/off,
 * force < 0 flips; returns resulting presence (-1 error). */
size_t whaleui_dom_class_length(whaleui_dom_element_t* el);
int whaleui_dom_class_item(whaleui_dom_element_t* el, size_t index, char* out, size_t cap);
int whaleui_dom_class_replace(whaleui_dom_element_t* el, const char* old_cls, const char* new_cls);
int whaleui_dom_class_toggle_force(whaleui_dom_element_t* el, const char* cls, int force);

/* --- dataset (mirror element.dataset.*) ---
 * Keys are camelCase and map to data-* attributes ("fooBar" <-> data-foo-bar);
 * keys already containing '-' map verbatim after the prefix. */
int whaleui_dom_dataset_set(whaleui_dom_element_t* el, const char* key, const char* value);
const char* whaleui_dom_dataset_get(whaleui_dom_element_t* el, const char* key);
int whaleui_dom_dataset_has(whaleui_dom_element_t* el, const char* key);
int whaleui_dom_dataset_remove(whaleui_dom_element_t* el, const char* key);

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

/* --- inline style (el.style.xxx; CSSStyleDeclaration C-port) --- */

int whaleui_dom_set_style(whaleui_dom_element_t* el, const char* property, const char* value);
const char* whaleui_dom_get_style(whaleui_dom_element_t* el, const char* property);
/* setProperty/getPropertyValue/removeProperty mirror CSSStyleDeclaration */
int whaleui_dom_style_set_property(whaleui_dom_element_t* el, const char* property, const char* value);
int whaleui_dom_style_get_property(whaleui_dom_element_t* el, const char* property, char* out, size_t cap);
int whaleui_dom_style_remove_property(whaleui_dom_element_t* el, const char* property);
/* cssText: whole inline style text ("a: b; c: d") */
int whaleui_dom_style_get_css_text(whaleui_dom_element_t* el, char* out, size_t cap);
int whaleui_dom_style_set_css_text(whaleui_dom_element_t* el, const char* css_text);
/* length/item: declaration count + property name at index */
size_t whaleui_dom_style_length(whaleui_dom_element_t* el);
int whaleui_dom_style_item(whaleui_dom_element_t* el, size_t index, char* out, size_t cap);

/* --- computed style (getComputedStyle C-port; snapshot, destroy it) --- */
typedef struct whaleui_style_declaration whaleui_style_declaration_t;
whaleui_style_declaration_t* whaleui_dom_get_computed_style(whaleui_dom_element_t* el, const char* pseudo);
void whaleui_style_declaration_destroy(whaleui_style_declaration_t* decl);
size_t whaleui_style_declaration_length(const whaleui_style_declaration_t* decl);
int whaleui_style_declaration_item(const whaleui_style_declaration_t* decl, size_t index, char* out, size_t cap);
int whaleui_style_declaration_get(const whaleui_style_declaration_t* decl, const char* property, char* out, size_t cap);

/* Tag name (lowercase; "input", "div", ...). */
const char* whaleui_dom_tag_name(whaleui_dom_element_t* el);

/* --- focus --- */

/* Focus/blur an element (mirrors el.focus() / el.blur()): moves the
 * document.activeElement mirror and fires the non-bubbling focus/blur pair
 * (blur on the element that was active, focus on the new one). blur() only
 * fires when el is the active element, like the browser. :focus STYLES
 * follow the engine focus (click / Tab) - on a rendered window the two
 * converge on the next frame. */
int whaleui_dom_focus(whaleui_dom_element_t* el);
int whaleui_dom_blur(whaleui_dom_element_t* el);

/* --- geometry (mirrors getBoundingClientRect) --- */

typedef struct whaleui_dom_rect {
    float x, y;        /* top-left, viewport coordinates */
    float width, height;
} whaleui_dom_rect_t;

int whaleui_dom_get_bounding_client_rect(whaleui_dom_element_t* el,
                                         whaleui_dom_rect_t* out);

/* --- Range (document.createRange() subset) ---
 *
 * One range object per handle (create/destroy). The endpoint model matches
 * the engine's selection: a container ELEMENT plus a UTF-8 byte offset into
 * its text (input value / textContent). Text-node containers are accepted
 * and resolved to their parent element, since the engine addresses text by
 * element + byte offset. */

typedef struct whaleui_dom_range whaleui_dom_range_t;

whaleui_dom_range_t* whaleui_dom_create_range(whaleui_dom_document_t* doc);
void whaleui_dom_range_destroy(whaleui_dom_range_t* range);

/* Endpoints. setStart/setEnd take a container (element or text node) and an
 * offset; the stored container is the owning element. 0 on success. */
int whaleui_dom_range_set_start(whaleui_dom_range_t* range,
                                whaleui_dom_element_t* container, int offset);
int whaleui_dom_range_set_end(whaleui_dom_range_t* range,
                              whaleui_dom_element_t* container, int offset);
whaleui_dom_element_t* whaleui_dom_range_start_container(const whaleui_dom_range_t* range);
whaleui_dom_element_t* whaleui_dom_range_end_container(const whaleui_dom_range_t* range);
int whaleui_dom_range_start_offset(const whaleui_dom_range_t* range);
int whaleui_dom_range_end_offset(const whaleui_dom_range_t* range);

/* collapsed: 1 when both endpoints are the same (element + offset). */
int whaleui_dom_range_collapsed(const whaleui_dom_range_t* range);
/* The covered text (same rules as the window selection's selected_text). */
const char* whaleui_dom_range_to_string(const whaleui_dom_range_t* range);
/* selectNodeContents(el): whole text of el. 0 on success. */
int whaleui_dom_range_select_node_contents(whaleui_dom_range_t* range,
                                           whaleui_dom_element_t* el);
/* deleteContents(): remove the covered text from the DOM (editable targets:
 * input value / textContent); returns 0 on success. */
int whaleui_dom_range_delete_contents(whaleui_dom_range_t* range);

/* ======================= DOM events ======================= */

/* Event listeners mirror addEventListener(type, listener[, useCapture])
 * with a plain C callback. `type` is a lowercase DOM event name; supported
 * types:
 *
 *   Mouse:    click, dblclick, mousedown, mouseup, mousemove,
 *             mouseenter, mouseleave, contextmenu, wheel
 *   Keyboard: keydown, keyup
 *   Focus:    focus, blur
 *   Form:     change, input, submit
 *   Document: load, scroll, resize
 *
 * Dispatch model: the DOM three-phase model - capture down the ancestor
 * chain (capture listeners), target (capture, then bubble listeners),
 * bubble back up (bubble listeners). mouseenter/mouseleave/focus/blur are
 * non-bubbling (capture + target only), as in the browser.
 * stopPropagation() halts the walk; preventDefault() cancels the engine's
 * default behavior for the event (mousedown cancels the press chain,
 * keydown cancels editing keys, wheel cancels the scroll). The callback
 * receives a transient event object valid for the duration of the call
 * only.
 *
 * Engine wiring: all types above are dispatched by the event loop
 * (mousemove/enter/leave from hover changes, wheel with preventDefault
 * gating the scroll, focus/blur on element focus moves, input/change on
 * edit commits and control toggles, scroll on actual scrolls, load after
 * document loads, resize on window resizes, dblclick/contextmenu from the
 * mouse buttons, submit from Enter inside a <form> - the engine performs
 * no form navigation; preventDefault is the app hook). */

typedef struct whaleui_dom_event whaleui_dom_event_t;

/* listener: (event, userdata). userdata must match at removal. */
typedef void (*whaleui_event_cb)(whaleui_dom_event_t* ev, void* userdata);

/* Register a listener. Returns 0 on success (duplicate type+cb+userdata is
 * a no-op success). Passing a document handle (cast to whaleui_dom_element_t*)
 * registers on the document-level target, whose event path is the
 * documentElement - like document.addEventListener. */
int whaleui_dom_add_event_listener(whaleui_dom_element_t* el, const char* type,
                                   whaleui_event_cb cb, void* userdata);
/* Register a capture-phase listener (fires on the way down the ancestor
 * chain and at the target before bubble listeners). */
int whaleui_dom_add_event_listener_ex(whaleui_dom_element_t* el,
                                      const char* type, whaleui_event_cb cb,
                                      void* userdata, int capture);
int whaleui_dom_remove_event_listener(whaleui_dom_element_t* el, const char* type,
                                      whaleui_event_cb cb, void* userdata);

/* Synchronously dispatch a synthetic event of `type` on el. Returns 0 on
 * success (listeners may call prevent_default). */
int whaleui_dom_dispatch_event(whaleui_dom_element_t* el, const char* type);

/* Full dispatch with input data + default-behavior reporting: runs the
 * three-phase dispatch and reports via out_prevented (may be NULL) whether
 * any listener called prevent_default. The engine's own click/key handlers
 * use this to decide whether their default behavior runs. */
int whaleui_dom_dispatch_phased(whaleui_dom_element_t* el, const char* type,
                                int key_code, int mouse_x, int mouse_y,
                                int mouse_button, float wheel_x,
                                float wheel_y, int* out_prevented);

/* --- event object accessors (valid inside the callback only) --- */

/* The element the event was dispatched on. */
whaleui_dom_element_t* whaleui_dom_event_target(const whaleui_dom_event_t* ev);

/* The node whose listeners are currently running (the ancestor during
 * capture/bubble, the target at the target phase). */
whaleui_dom_element_t* whaleui_dom_event_current_target(
    const whaleui_dom_event_t* ev);

/* The current phase: 1 = capture, 2 = target, 3 = bubble. */
int whaleui_dom_event_phase(const whaleui_dom_event_t* ev);

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

/* --- modifier keys (mirror KeyboardEvent.getModifierState) --- */

enum {
    WHALEUI_MOD_NONE  = 0,
    WHALEUI_MOD_SHIFT = 1,
    WHALEUI_MOD_CTRL  = 2,
    WHALEUI_MOD_ALT   = 4,
    WHALEUI_MOD_META  = 8
};

/* The modifier bitmask (WHALEUI_MOD_*) active when the event fired. */
int whaleui_dom_event_mods(const whaleui_dom_event_t* ev);
int whaleui_dom_event_ctrl_key(const whaleui_dom_event_t* ev);
int whaleui_dom_event_shift_key(const whaleui_dom_event_t* ev);
int whaleui_dom_event_alt_key(const whaleui_dom_event_t* ev);
int whaleui_dom_event_meta_key(const whaleui_dom_event_t* ev);

/* --- richer keyboard / mouse / focus payloads --- */

/* KeyboardEvent.key: the printable name ("a", "A", "Enter", "ArrowLeft",
 * "F1", " "). Owned by the event, valid inside the callback. */
const char* whaleui_dom_event_key(const whaleui_dom_event_t* ev);
/* KeyboardEvent.code: the physical key ("KeyA", "Enter", "ArrowLeft",
 * "Space"). Layout independent. */
const char* whaleui_dom_event_code(const whaleui_dom_event_t* ev);
/* KeyboardEvent.repeat: 1 when this is an auto-repeat of a held key. */
int whaleui_dom_event_repeat(const whaleui_dom_event_t* ev);
/* MouseEvent.detail: the click count (2 = dblclick). */
int whaleui_dom_event_detail(const whaleui_dom_event_t* ev);
/* MouseEvent.movementX/Y: pointer delta since the previous event. */
float whaleui_dom_event_movement_x(const whaleui_dom_event_t* ev);
float whaleui_dom_event_movement_y(const whaleui_dom_event_t* ev);
/* mouseenter/mouseleave: the element the pointer came from / went to. */
whaleui_dom_element_t* whaleui_dom_event_related_target(const whaleui_dom_event_t* ev);
/* copy/cut/paste: the clipboard payload. text is NULL when the event
 * carries none. set_clipboard_text supplies the data the default behavior
 * writes on copy/cut, or the text inserted on paste. */
const char* whaleui_dom_event_clipboard_text(const whaleui_dom_event_t* ev);
int whaleui_dom_event_set_clipboard_text(whaleui_dom_event_t* ev, const char* text);

/* --- struct-based dispatch (full payload) ---
 *
 * The scalar whaleui_dom_dispatch_phased above stays for compatibility; this
 * is the entry point the engine uses, carrying modifiers, key names, repeat,
 * click detail, pointer movement, relatedTarget and clipboard text. */
typedef struct whaleui_event_init {
    int key_code;              /* WHALEUI_KEY_* (0 when not a key event) */
    const char* key;           /* KeyboardEvent.key (NULL to omit) */
    const char* code;          /* KeyboardEvent.code (NULL to omit) */
    int repeat;                /* 1 = auto-repeat */
    int mods;                  /* WHALEUI_MOD_* bitmask */
    int mouse_x, mouse_y;      /* viewport coordinates */
    int mouse_button;          /* 0 = left, 1 = middle, 2 = right */
    int detail;                /* click count */
    float wheel_x, wheel_y;    /* wheel deltas */
    float movement_x, movement_y; /* pointer delta since the last event */
    whaleui_dom_element_t* related_target; /* enter/leave counterpart */
    const char* clipboard_text; /* copy/cut/paste payload */
    void* data_transfer;        /* whaleui_data_transfer_t* (drag/drop events) */
} whaleui_event_init_t;

int whaleui_dom_dispatch_input(whaleui_dom_element_t* el, const char* type,
                               const whaleui_event_init_t* init,
                               int* out_prevented);

/* Non-bubbling variant (mouseenter/mouseleave/focus/blur). */
int whaleui_dom_dispatch_input_nonbubble(whaleui_dom_element_t* el,
                                         const char* type,
                                         const whaleui_event_init_t* init,
                                         int* out_prevented);

/* ======================= drag & drop =======================
 *
 * Two sources, one model:
 *
 *  1. OS drags INTO the window (files or text dragged from Explorer/Finder/
 *     another app). SDL delivers SDL_EVENT_DROP_*; the engine hit-tests and
 *     fires dragenter/dragover/drop/dragleave on the element under the
 *     pointer, with a DataTransfer carrying the files (DROP_FILE) or the text
 *     (DROP_TEXT).
 *  2. In-page HTML5 drags: an element with draggable="true" pressed and
 *     dragged past a small threshold starts a session - dragstart on the
 *     source, dragenter/dragover/dragleave on whatever is under the pointer,
 *     drop on the target that called preventDefault in its dragover, and
 *     dragend on the source.
 *
 * The engine performs NO default action for either (a browser would navigate
 * to a dropped file): the page handles `drop`. Calling preventDefault in a
 * `dragover` listener is what makes an element a drop target, exactly as in
 * the browser.
 *
 * DataTransfer mirrors the W3C object's useful subset: typed items
 * ("text/plain", "text/uri-list", anything), a file list, and the
 * effectAllowed/dropEffect negotiation pair. During dragstart the source fills
 * it; during drop the target reads it. The engine pre-fills text/plain with
 * the source element's textContent for in-page drags (the browser uses the
 * selection; textContent is the closest thing we have) unless a dragstart
 * listener replaced it. */

typedef struct whaleui_file whaleui_file_t;

/* File name (basename) / the URI it was dropped as. Owned by the file. */
const char* whaleui_file_name(const whaleui_file_t* f);
const char* whaleui_file_uri(const whaleui_file_t* f);
/* Byte size, or -1 when it could not be read. Reads the file through the VFS
 * on first call and caches the length. */
long long whaleui_file_size(const whaleui_file_t* f);
/* Read the whole file through the VFS (malloc'd; caller frees with free()).
 * Returns 0 on success. */
int whaleui_file_read(const whaleui_file_t* f, char** out, size_t* outlen);

typedef struct whaleui_data_transfer whaleui_data_transfer_t;

/* Create/destroy a transfer. The engine owns the ones it passes to listeners
 * (do not destroy those); create your own to synthesize a drag/drop. */
whaleui_data_transfer_t* whaleui_data_transfer_create(void);
void whaleui_data_transfer_destroy(whaleui_data_transfer_t* dt);

/* Typed items. set replaces an existing item with the same format. */
int whaleui_data_transfer_set_data(whaleui_data_transfer_t* dt, const char* format,
                                   const char* data);
const char* whaleui_data_transfer_get_data(const whaleui_data_transfer_t* dt,
                                           const char* format);
int whaleui_data_transfer_has_format(const whaleui_data_transfer_t* dt,
                                     const char* format);
int whaleui_data_transfer_clear_data(whaleui_data_transfer_t* dt);
size_t whaleui_data_transfer_type_count(const whaleui_data_transfer_t* dt);
const char* whaleui_data_transfer_type_item(const whaleui_data_transfer_t* dt, size_t index);

/* text/plain convenience (the common case). */
int whaleui_data_transfer_set_text(whaleui_data_transfer_t* dt, const char* text);
const char* whaleui_data_transfer_get_text(const whaleui_data_transfer_t* dt);

/* File list (populated by an OS file drop). */
size_t whaleui_data_transfer_file_count(const whaleui_data_transfer_t* dt);
const whaleui_file_t* whaleui_data_transfer_file_item(const whaleui_data_transfer_t* dt,
                                                      size_t index);
/* Append a file (uri resolved through the VFS when read). */
int whaleui_data_transfer_add_file(whaleui_data_transfer_t* dt, const char* name,
                                   const char* uri);

/* effectAllowed / dropEffect: 0 = none, 1 = copy, 2 = move, 3 = link. */
int  whaleui_data_transfer_effect_allowed(const whaleui_data_transfer_t* dt);
void whaleui_data_transfer_set_effect_allowed(whaleui_data_transfer_t* dt, int effect);
int  whaleui_data_transfer_drop_effect(const whaleui_data_transfer_t* dt);
void whaleui_data_transfer_set_drop_effect(whaleui_data_transfer_t* dt, int effect);

/* The transfer attached to a drag/drop event (NULL for other event types).
 * Borrowed: valid for the duration of the dispatch. */
whaleui_data_transfer_t* whaleui_dom_event_data_transfer(const whaleui_dom_event_t* ev);

/* --- drag state (per window) --- */

/* 1 while an in-page drag session is running on this window. */
int whaleui_window_drag_active(const whaleui_window_t* win);
/* The drag source element of the running session (NULL when none). */
whaleui_dom_element_t* whaleui_window_drag_source(const whaleui_window_t* win);

/* ======================= Web 平台补充 API =======================
 *
 * matchMedia / CSS.supports / screen / navigator / Web Storage /
 * queueMicrotask / requestIdleCallback. See doc/external/09-api-web.md. */

/* --- matchMedia (window.matchMedia) --- */

typedef struct whaleui_media_query_list whaleui_media_query_list_t;
/* Called when `matches` flips (window resize / theme change). */
typedef void (*whaleui_media_query_cb)(whaleui_media_query_list_t* list, void* userdata);

whaleui_media_query_list_t* whaleui_window_match_media(const whaleui_window_t* win,
                                                       const char* query);
void whaleui_media_query_list_destroy(whaleui_media_query_list_t* list);
int whaleui_media_query_list_matches(const whaleui_media_query_list_t* list);
const char* whaleui_media_query_list_media(const whaleui_media_query_list_t* list);
int whaleui_media_query_list_add_listener(whaleui_media_query_list_t* list,
                                          whaleui_media_query_cb cb, void* userdata);
int whaleui_media_query_list_remove_listener(whaleui_media_query_list_t* list,
                                             whaleui_media_query_cb cb, void* userdata);

/* --- CSS.supports --- */

/* 1 when the engine's CSS parser keeps `property: value` (the parser IS the
 * supported subset, so this answers the real question). */
int whaleui_css_supports(const char* property, const char* value);

/* --- screen / navigator --- */

int whaleui_window_screen_size(const whaleui_window_t* win, int* w, int* h);
/* screen.availLeft/Top/Width/Height (work area, taskbar excluded). */
int whaleui_window_screen_work_area(const whaleui_window_t* win, int* x, int* y,
                                    int* w, int* h);
const char* whaleui_navigator_user_agent(void);
const char* whaleui_navigator_platform(void);
const char* whaleui_navigator_language(void);

/* --- Web Storage (localStorage / sessionStorage) ---
 *
 * One global store (the engine has no origins). session = 1 selects
 * sessionStorage (memory only); session = 0 selects localStorage, persisted
 * under the platform's user-data dir. get_item returns NULL when absent. */

int whaleui_storage_length(int session);
const char* whaleui_storage_key(int session, int index);      /* NULL past the end */
const char* whaleui_storage_get_item(int session, const char* key);
int whaleui_storage_set_item(int session, const char* key, const char* value);
int whaleui_storage_remove_item(int session, const char* key);
int whaleui_storage_clear(int session);

/* --- microtasks / idle callbacks --- */

typedef void (*whaleui_web_task_cb)(void* userdata);

/* queueMicrotask: runs before the next timer, after the current task. */
int whaleui_queue_microtask(whaleui_web_task_cb cb, void* userdata);
/* requestIdleCallback: runs on an otherwise-idle loop iteration. */
int whaleui_request_idle_callback(whaleui_web_task_cb cb, void* userdata);

/* --- MutationObserver --- */

typedef struct whaleui_mutation_observer whaleui_mutation_observer_t;

/* Mutation type ids passed to the callback / returned by take_records. */
enum {
    WHALEUI_MUTATION_ATTRIBUTES = 0,
    WHALEUI_MUTATION_CHARACTER_DATA = 1,
    WHALEUI_MUTATION_CHILD_LIST = 2
};

/* Called once per delivered record (batched per loop iteration, not inline).
 * attr is the attribute name for attribute records (NULL otherwise);
 * added/removed say whether childList gained/lost nodes. */
typedef void (*whaleui_mutation_cb)(whaleui_dom_element_t* target, int type,
                                    const char* attr, int added, int removed,
                                    void* userdata);

whaleui_mutation_observer_t* whaleui_dom_create_mutation_observer(whaleui_dom_document_t* doc);
void whaleui_dom_mutation_observer_destroy(whaleui_mutation_observer_t* obs);
/* el NULL watches the whole document; otherwise el's subtree. */
int whaleui_dom_mutation_observer_observe(whaleui_mutation_observer_t* obs,
                                          whaleui_dom_element_t* el,
                                          whaleui_mutation_cb cb, void* userdata);
int whaleui_dom_mutation_observer_disconnect(whaleui_mutation_observer_t* obs);
/* Number of records queued for this observer but not yet delivered (normally
 * 0: the pump delivers as it goes). */
int whaleui_dom_mutation_observer_take_records(whaleui_mutation_observer_t* obs);

/* --- ResizeObserver ---
 *
 * Sampled once per frame: the element's laid-out border box is compared with
 * the size last delivered, and a change fires the callback (the initial size
 * is delivered on the first frame after observe()). Elements not laid out
 * (display:none, detached) report nothing. */

typedef struct whaleui_resize_observer whaleui_resize_observer_t;
typedef void (*whaleui_resize_cb)(whaleui_dom_element_t* el, float width,
                                  float height, void* userdata);

whaleui_resize_observer_t* whaleui_dom_create_resize_observer(whaleui_dom_document_t* doc);
void whaleui_dom_resize_observer_destroy(whaleui_resize_observer_t* obs);
int whaleui_dom_resize_observer_observe(whaleui_resize_observer_t* obs,
                                        whaleui_dom_element_t* el,
                                        whaleui_resize_cb cb, void* userdata);
int whaleui_dom_resize_observer_unobserve(whaleui_resize_observer_t* obs,
                                          whaleui_dom_element_t* el);
int whaleui_dom_resize_observer_disconnect(whaleui_resize_observer_t* obs);

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

/* ======================= clipboard (navigator.clipboard subset) ======================= */

/* The system clipboard. SDL backs this (SDL_Get/SetClipboardText), so the
 * content is shared with the rest of the desktop. The browser's async
 * navigator.clipboard API is mapped to a sync pair plus callback forms that
 * resolve on the event loop (the engine has no promise objects). */

/* 1 when the clipboard currently holds text. */
int whaleui_clipboard_has_text(void);

/* The clipboard text ("" when empty). The returned pointer is owned by the
 * library and valid until the next clipboard call. */
const char* whaleui_clipboard_get_text(void);

/* Replace the clipboard contents. 0 on success. */
int whaleui_clipboard_set_text(const char* text);

/* Async (promise-like) forms: the callback runs on the event loop thread
 * after the operation completes; text is NULL for a failed read, and for a
 * write it echoes the text that was written. */
typedef void (*whaleui_clipboard_text_cb)(const char* text, void* userdata);
int whaleui_clipboard_read_text_async(whaleui_clipboard_text_cb cb, void* userdata);
int whaleui_clipboard_write_text_async(const char* text,
                                       whaleui_clipboard_text_cb cb,
                                       void* userdata);

/* ======================= timers / animation frames ======================= */

/* setTimeout/setInterval/requestAnimationFrame. All callbacks run on the
 * event loop thread (the same thread that renders and dispatches DOM events),
 * so they may touch the DOM directly. Timers keep the loop awake: the worker
 * sleeps only until the next due timer. */

/* Milliseconds since the application started (performance.now()-like). */
double whaleui_now(void);

/* callback(timestamp_ms, userdata): timestamp is whaleui_now() at fire time. */
typedef void (*whaleui_timer_cb)(double timestamp_ms, void* userdata);

/* setTimeout/setInterval. Returns a positive id, or 0 on failure. delay_ms
 * is clamped to >= 0; an interval with 0 runs once per loop iteration. */
long long whaleui_set_timeout(whaleui_timer_cb cb, void* userdata, double delay_ms);
long long whaleui_set_interval(whaleui_timer_cb cb, void* userdata, double interval_ms);
/* clearTimeout/clearInterval accept either kind of id (like the browser). */
void whaleui_clear_timeout(long long id);
void whaleui_clear_interval(long long id);
/* Number of live timers (diagnostics / tests). */
int whaleui_timer_count(void);

/* requestAnimationFrame: the callback runs once per rendered frame (the
 * engine's frame clock, not the display's), and keeps the loop alive until it
 * fires. Returns a positive id, or 0 on failure. */
typedef void (*whaleui_frame_cb)(double timestamp_ms, void* userdata);
long long whaleui_request_animation_frame(whaleui_frame_cb cb, void* userdata);
void whaleui_cancel_animation_frame(long long id);
int whaleui_animation_frame_count(void);

/* ======================= Web Audio (placeholder) ======================= */

/* Audio output is deferred (see rules/goal.md: audio/video are out of scope
 * for now). These functions exist so bindings and pages can feature-detect
 * and so the shapes are frozen; everything except state/clock reports
 * unsupported and creates no device. */

typedef struct whaleui_audio_context whaleui_audio_context_t;

typedef enum whaleui_audio_state {
    WHALEUI_AUDIO_SUSPENDED = 0,
    WHALEUI_AUDIO_RUNNING,
    WHALEUI_AUDIO_CLOSED
} whaleui_audio_state_t;

/* 0 while audio is unimplemented (callers should feature-detect). */
int whaleui_audio_is_supported(void);

/* NULL while unsupported. */
whaleui_audio_context_t* whaleui_audio_context_create(void);
void whaleui_audio_context_destroy(whaleui_audio_context_t* ctx);
whaleui_audio_state_t whaleui_audio_context_state(const whaleui_audio_context_t* ctx);
int whaleui_audio_context_resume(whaleui_audio_context_t* ctx);
int whaleui_audio_context_suspend(whaleui_audio_context_t* ctx);
int whaleui_audio_context_close(whaleui_audio_context_t* ctx);
/* AudioContext.sampleRate / currentTime. */
float whaleui_audio_context_sample_rate(const whaleui_audio_context_t* ctx);
double whaleui_audio_context_current_time(const whaleui_audio_context_t* ctx);

/* ======================= Gamepad (navigator.getGamepads subset) ======================= */

/* SDL3 provides full gamepad support; the engine exposes a browser-shaped
 * subset. Indices are dense (0..count-1) and stable while a pad stays
 * connected; the W3C Gamepad API's sparse-index behavior is not mirrored.
 * Connect/disconnect also fires "gamepadconnected"/"gamepaddisconnected" on
 * the document element, with the pad index in the event's detail field. */

/* Number of currently connected gamepads. */
int whaleui_gamepad_count(void);
/* 1 when the pad at `index` is connected. */
int whaleui_gamepad_connected(int index);
/* Gamepad.id: the device name ("" when unknown). Owned by the library. */
const char* whaleui_gamepad_name(int index);

/* Gamepad axes/buttons snapshot. axes are -1.0..1.0; buttons is a bitmask of
 * currently-pressed buttons (W3C order: 0 = A/south, 1 = B/east, ...). */
#define WHALEUI_GAMEPAD_MAX_AXES 8
#define WHALEUI_GAMEPAD_MAX_BUTTONS 32
typedef struct whaleui_gamepad_state {
    int connected;
    int axis_count;
    float axes[WHALEUI_GAMEPAD_MAX_AXES];
    int button_count;
    unsigned int buttons;   /* bit i set = button i pressed */
    float trigger_left;     /* 0..1 (L2) */
    float trigger_right;    /* 0..1 (R2) */
} whaleui_gamepad_state_t;

/* Poll the pad. 0 on success; non-zero when the index is out of range or the
 * pad is not connected. */
int whaleui_gamepad_poll(int index, whaleui_gamepad_state_t* out);
/* rumble (0..1 each, duration in ms); 0 when the pad has no rumble. */
int whaleui_gamepad_rumble(int index, float low_freq, float high_freq,
                           unsigned int duration_ms);

/* ======================= Canvas 2D (HTMLCanvasElement C-port) ======================= */

typedef struct whaleui_canvas_context whaleui_canvas_context_t;
typedef struct whaleui_image_data whaleui_image_data_t;

/* getContext("2d"): NULL unless el is a <canvas> and type == "2d". */
whaleui_canvas_context_t* whaleui_canvas_get_context(whaleui_dom_element_t* el,
                                                     const char* type);
/* width/height attributes (backing store reset, transparent black). */
int whaleui_canvas_set_size(whaleui_dom_element_t* el, int w, int h);
int whaleui_canvas_get_size(whaleui_dom_element_t* el, int* w, int* h);

/* state */
int whaleui_canvas_set_fill_style(whaleui_canvas_context_t* ctx, const char* style);
int whaleui_canvas_set_stroke_style(whaleui_canvas_context_t* ctx, const char* style);
int whaleui_canvas_set_line_width(whaleui_canvas_context_t* ctx, float w);
int whaleui_canvas_set_global_alpha(whaleui_canvas_context_t* ctx, float a);
/* font shorthand ("bold 16px Arial, sans-serif"); text_align is
 * start|end|left|right|center; text_baseline is
 * alphabetic|top|middle|bottom|hanging|ideographic. */
int whaleui_canvas_set_font(whaleui_canvas_context_t* ctx, const char* font);
int whaleui_canvas_set_text_align(whaleui_canvas_context_t* ctx, const char* align);
int whaleui_canvas_set_text_baseline(whaleui_canvas_context_t* ctx, const char* baseline);

/* text (fillText/strokeText/measureText). Text is positioned by the current
 * transform + textAlign/textBaseline; x/y is the baseline origin for the
 * default alphabetic baseline. measureText fills out_w/out_h (advance width
 * and line height). */
int whaleui_canvas_fill_text(whaleui_canvas_context_t* ctx, const char* text,
                             float x, float y);
int whaleui_canvas_stroke_text(whaleui_canvas_context_t* ctx, const char* text,
                               float x, float y);
int whaleui_canvas_measure_text(whaleui_canvas_context_t* ctx, const char* text,
                                int* out_w, int* out_h);

/* rects */
int whaleui_canvas_fill_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h);
int whaleui_canvas_stroke_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h);
int whaleui_canvas_clear_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h);

/* path */
int whaleui_canvas_begin_path(whaleui_canvas_context_t* ctx);
int whaleui_canvas_move_to(whaleui_canvas_context_t* ctx, float x, float y);
int whaleui_canvas_line_to(whaleui_canvas_context_t* ctx, float x, float y);
int whaleui_canvas_close_path(whaleui_canvas_context_t* ctx);
int whaleui_canvas_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h);
int whaleui_canvas_arc(whaleui_canvas_context_t* ctx, float x, float y, float r, float a0, float a1);
int whaleui_canvas_fill(whaleui_canvas_context_t* ctx);
int whaleui_canvas_stroke(whaleui_canvas_context_t* ctx);

/* state stack + transform */
int whaleui_canvas_save(whaleui_canvas_context_t* ctx);
int whaleui_canvas_restore(whaleui_canvas_context_t* ctx);
int whaleui_canvas_set_transform(whaleui_canvas_context_t* ctx, float a, float b, float c, float d, float e, float f);
int whaleui_canvas_transform(whaleui_canvas_context_t* ctx, float a, float b, float c, float d, float e, float f);
int whaleui_canvas_translate(whaleui_canvas_context_t* ctx, float x, float y);
int whaleui_canvas_scale(whaleui_canvas_context_t* ctx, float x, float y);
int whaleui_canvas_rotate(whaleui_canvas_context_t* ctx, float angle);

/* ImageData (0xAARRGGBB pixels) */
whaleui_image_data_t* whaleui_canvas_create_image_data(whaleui_canvas_context_t* ctx, int w, int h);
void whaleui_image_data_destroy(whaleui_image_data_t* d);
int whaleui_image_data_size(const whaleui_image_data_t* d, int* w, int* h);
unsigned int whaleui_image_data_get(const whaleui_image_data_t* d, int x, int y);
int whaleui_image_data_set(whaleui_image_data_t* d, int x, int y, unsigned int argb);
whaleui_image_data_t* whaleui_canvas_get_image_data(whaleui_canvas_context_t* ctx, int x, int y, int w, int h);
int whaleui_canvas_put_image_data(whaleui_canvas_context_t* ctx, whaleui_image_data_t* d, int x, int y);

/* drawImage (browser 9-arg signature): source is another canvas context
 * (whaleui_canvas_draw_image) or an ImageData (whaleui_canvas_draw_image_data).
 * sw/sh <= 0 selects the whole remainder of the source; dw/dh <= 0 keeps the
 * source size. Sampling respects the current transform (affine inverse,
 * nearest-neighbour) and globalAlpha. Unlike put_image_data (raw copy at
 * device pixels), drawImage composites like the browser. Decoding <img>/
 * data-URI sources is a follow-up; use get_image_data + draw_image_data. */
int whaleui_canvas_draw_image(whaleui_canvas_context_t* ctx,
                              const whaleui_canvas_context_t* src,
                              float sx, float sy, float sw, float sh,
                              float dx, float dy, float dw, float dh);
int whaleui_canvas_draw_image_data(whaleui_canvas_context_t* ctx,
                                   const whaleui_image_data_t* img,
                                   float sx, float sy, float sw, float sh,
                                   float dx, float dy, float dw, float dh);

/* ======================= WebGPU (navigator.gpu C-port, stub backend) ======================= */

typedef struct whaleui_gpu_adapter whaleui_gpu_adapter_t;
typedef struct whaleui_gpu_device whaleui_gpu_device_t;
typedef struct whaleui_gpu_queue whaleui_gpu_queue_t;
typedef struct whaleui_gpu_buffer whaleui_gpu_buffer_t;
typedef struct whaleui_gpu_texture whaleui_gpu_texture_t;
typedef struct whaleui_gpu_canvas_context whaleui_gpu_canvas_context_t;

typedef enum whaleui_gpu_backend {
    WHALEUI_GPU_BACKEND_NONE = 0,
    WHALEUI_GPU_BACKEND_SDL_GPU,
    WHALEUI_GPU_BACKEND_WGPU_NATIVE
} whaleui_gpu_backend_t;

typedef struct whaleui_gpu_limits {
    unsigned int max_texture_dimension_2d;
    unsigned int max_buffer_size;
} whaleui_gpu_limits_t;

/* environment: which backend the engine was built with / can provide */
whaleui_gpu_backend_t whaleui_gpu_backend(void);
int whaleui_gpu_is_supported(void); /* 1 when an adapter can be provided */

/* adapter/device/queue (synchronous C-port of the async WebGPU calls) */
int whaleui_gpu_request_adapter(whaleui_gpu_adapter_t** out);
void whaleui_gpu_adapter_destroy(whaleui_gpu_adapter_t* a);
int whaleui_gpu_adapter_limits(const whaleui_gpu_adapter_t* a, whaleui_gpu_limits_t* out);
int whaleui_gpu_adapter_request_device(whaleui_gpu_adapter_t* a, whaleui_gpu_device_t** out);
void whaleui_gpu_device_destroy(whaleui_gpu_device_t* d);
whaleui_gpu_queue_t* whaleui_gpu_device_queue(whaleui_gpu_device_t* d);

/* buffers/textures (opaque handles; data upload is explicit) */
whaleui_gpu_buffer_t* whaleui_gpu_device_create_buffer(whaleui_gpu_device_t* d,
                                                       unsigned long long size, unsigned int usage);
void whaleui_gpu_buffer_destroy(whaleui_gpu_buffer_t* b);
int whaleui_gpu_queue_write_buffer(whaleui_gpu_queue_t* q, whaleui_gpu_buffer_t* b,
                                   unsigned long long offset, const void* data, size_t size);
whaleui_gpu_texture_t* whaleui_gpu_device_create_texture_2d(whaleui_gpu_device_t* d,
                                                            unsigned int w, unsigned int h, unsigned int format);
void whaleui_gpu_texture_destroy(whaleui_gpu_texture_t* t);

/* canvas interop: configure a <canvas> for GPU presentation */
whaleui_gpu_canvas_context_t* whaleui_gpu_canvas_get_context(whaleui_dom_element_t* el);
int whaleui_gpu_canvas_configure(whaleui_gpu_canvas_context_t* c, whaleui_gpu_device_t* d,
                                 unsigned int format, unsigned int alpha_mode);
void whaleui_gpu_canvas_context_destroy(whaleui_gpu_canvas_context_t* c);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_WHALEUI_H */

