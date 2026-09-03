#ifndef WHALEUI_RENDER_RENDER_H
#define WHALEUI_RENDER_RENDER_H

/* Renderer - internal interface.
 *
 * One render context per window. Frames are painted into a CPU RGBA
 * framebuffer, uploaded to an offscreen GPU texture and blitted to the
 * swapchain with SDL's built-in blit pipeline (no custom shaders yet - the
 * intermediate-shader path is a later step). Text is rasterized with
 * SDL3_ttf from fonts registered through the font module. */

#include "whaleui.h"
#include "layout/layout.h"
#include "style/css.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <string>
#include <tuple>
#include <vector>

#include <atomic>
#include <thread>

/* SDL3 opaque types; never dereferenced in this header. */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUTexture SDL_GPUTexture;
typedef struct SDL_GPUTransferBuffer SDL_GPUTransferBuffer;
typedef struct SDL_GPUComputePipeline SDL_GPUComputePipeline;
typedef struct SDL_Surface SDL_Surface;
typedef struct SDL_Cursor SDL_Cursor;
typedef struct TTF_Font TTF_Font;

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_render
{
    SDL_GPUDevice* device; /* borrowed from app->gpu */
    SDL_Window* window;    /* borrowed from win->sdl */

    int width, height;     /* window size in pixels */
    int has_dirty;
    /* set by whaleui_render_frame: 1 while the frame loop must keep
     * repainting (running animation, blinking caret), 0 when the page is
     * static. The app loop reads it (via the worker's OR over windows) to
     * decide whether to keep requesting frames or park idle. */
    int alive;

    /* CPU framebuffer (0xAARRGGBB) - CPU paint path (controls/selection)
     * only; the GPU path draws geometry + text directly. */
    std::vector<unsigned int> pixels;

    /* owned stylesheet + last layout tree (rebuilt on dirty) */
    whaleui_layout_tree_t* tree;
    whaleui_css_rule_t* rules;
    size_t rule_count;
    whaleui_css_keyframes_t keyframes;
    std::map<std::string, std::string> theme_vars;
    /* any rule selects on :hover/:active/:focus? Interaction-state changes
     * only need a relayout when a rule can react to them; a stylesheet
     * with none (most form/plain pages) makes hover purely a cursor/state
     * update - no tree rebuild per hover element change. */
    int has_interact_rules;

    /* font cache: "family" -> TTF_Font. One font per FAMILY - size and
     * style are set on demand (font_state below) right before use, so a
     * page with N font sizes does not open N copies of the 20MB CJK font
     * (that was the bulk of the ~130MB font memory: every (family,size,
     * style) fallback chain re-opened every registered font). */
    std::vector<std::pair<std::string, TTF_Font*>> fonts;
    /* last-applied (size, style) per font; ensure_font_state skips the
     * SDL3_ttf calls when unchanged */
    std::map<TTF_Font*, std::pair<int, int>> font_state;
    /* primary font -> its fallback chain (rendered glyphs sync the chain's
     * sizes to the current run before rasterizing) */
    std::map<TTF_Font*, std::vector<TTF_Font*>> font_chain;
    /* fallback fonts already opened (lazy: a registered font is only
     * opened when a glyph is actually missing; one TTF_Font per family is
     * kept and size-applied on demand - no per-element font copies) */
    std::vector<TTF_Font*> fallback_open;
    /* (family, codepoint) pairs verified NOT to be provided by that font
     * (checked without keeping the font open): skipped on later misses so
     * a useless font is not reopened per glyph, but the same family can
     * still serve OTHER codepoints (msyh has CJK but not emoji) */
    std::vector<std::pair<std::string, unsigned int>> fallback_tried;
    TTF_Font* font_default;
    /* family of font_default (its registry family name); lazy fallback
     * skips re-opening it */
    std::string default_family;
    /* ASCII glyph-advance cache (layout hot path): rebuilt when the font
     * changes. Full build only; stb measures directly from its font table. */
    TTF_Font* ascii_font;
    int ascii_fs; /* size ascii_font was cached at (SetFontSize reuses it) */
    int ascii_w[128];
    /* non-ASCII glyph-advance cache (full build): measuring CJK/emoji via a
     * per-char TTF_Text was the dominant cost on large CJK pages (every
     * char created + destroyed a TTF_Text); caching per (font, size,
     * codepoint) keeps layout linear. The size is part of the key because
     * render_get_font reuses one TTF_Font per (family,style) and switches
     * its size with TTF_SetFontSize - a size-less key would return a stale
     * advance after a size change (and re-layout the whole page wrong). */
    std::map<std::tuple<TTF_Font*, int, unsigned int>, int> glyph_w_cache;

    /* whole-string width cache for the layout metric hook: the box pass
     * and fix_run_heights re-measure the same stable text every frame
     * (an animation only repositions, it does not change the glyphs), so
     * they are the hot cost. Keyed by a 64-bit FNV hash of the text+size+
     * family+weight so the lookup does not copy the whole UTF-8 string.
     * A stable run hits and returns in O(1). */
    std::unordered_map<uint64_t, float> text_w_cache;

    /* text_size (wrapped measure) cache: the layout pass calls text_size
     * for every text run on every relayout (a width animation re-lays out
     * each frame); the layout_text walk is the per-frame cost. Keyed by an
     * FNV hash of text+size+family+bold+wrap width - content-addressed, so
     * edits invalidate themselves. */
    std::unordered_map<uint64_t, std::pair<int, int> > text_size_cache;

    /* text cache: one rasterized RGBA buffer per element+style, reused
     * across frames. Rebuilding text objects AND re-rasterizing every run
     * per frame was the dominant paint cost on text-heavy pages (~13fps);
     * cached frames are millisecond cheap. The key includes the text so
     * edits recreate the buffer. Bounded (LRU by tc_bytes) so a huge page
     * (qwen 21k, hundreds of runs) cannot grow it unbounded. */
    struct TextCacheEntry
    {
        std::vector<unsigned int> px; /* 0xAARRGGBB,已含前景色/彩色字形色 */
        int w, h;
        uint64_t last_use;            /* LRU tick (r->tc_tick) */
    };
    std::map<std::string, TextCacheEntry> text_cache;
    uint64_t tc_tick;      /* monotonic use counter for LRU */
    size_t tc_bytes;       /* total raster bytes in the cache */

    /* decoded <img> bitmaps by src (owned; freed in destroy). Only local
     * file:// / relative URIs are loadable; remote or missing sources fall
     * back to a placeholder box. */
    std::map<std::string, SDL_Surface*> images;

    /* select dropdown interaction state. open_select is the DOM element
     * (stable across layout-tree rebuilds; layout nodes are recreated every
     * frame and would dangle) */
    struct lxb_dom_element* open_select;
    int open_select_hover;
    std::map<struct lxb_dom_element*, int> select_index; /* select -> option */

    /* element under the mouse (for :hover rules) */
    struct lxb_dom_element* hover_el;
    /* previous hover target, dirtied for a partial repaint on change */
    struct lxb_dom_element* hover_old_el;
    /* an interaction-state change (:hover/:focus/:active on a stylesheet
     * with such rules) is pending: the next frame relayouts only the
     * previous + current state elements through the DOM-dirty incremental
     * path instead of a whole-tree rebuild (a 22k-node page rebuilt every
     * tree on each mouse move - seconds of freeze per hover change). */
    int state_pending;
    /* an edit/caret move happened: the frame's relayout pass should
     * scroll the caret visible ONCE. User scrolls (wheel/drag) never set
     * this, so a focused textarea is not locked to the caret line. */
    int edit_scroll_need;
    /* last clicked control (:focus) and the element the left button is
     * held down on (:active) */
    struct lxb_dom_element* focus_el;
    struct lxb_dom_element* pressed_el;

    /* vertical scroll per element (overflow:auto/scroll), applied at layout */
    std::map<struct lxb_dom_element*, int> scrolls;
    /* horizontal scroll for single-line inputs (content wider than the
     * box): the caret scrolls the text sideways, never the box */
    std::map<struct lxb_dom_element*, int> hscrolls;
    /* last-frame scroll snapshot: used to detect pure scroll deltas so the
     * frame can shift the previous image instead of repainting everything */
    std::map<struct lxb_dom_element*, int> last_scrolls;
    /* wheel scrolling changed scrolls: repaint next frame without relayout */
    int scroll_dirty;
    /* scroll behavior hook (default: clamped; replace for smooth scrolling) */
    int (*scroll_fn)(struct whaleui_render*, struct lxb_dom_element*, int,
                     void*);
    void* scroll_ud;
    /* smooth scrolling (opt-in: set scroll_fn to whaleui_scroll_smooth_fn):
     * wheel deltas accumulate into these TARGET positions and the frame
     * loop eases the live scrolls toward them in small per-frame steps -
     * discrete mouse-wheel notches move like touchpad deltas instead of
     * jumping a full notch per event */
    std::map<struct lxb_dom_element*, int> scroll_tgt;
    /* cached scroll_max for the last wheel-scrolled element (wheel events
     * arrive in bursts) */
    struct lxb_dom_element* scroll_max_el;
    int scroll_max_cache;
    /* last wheel-hit layout node + pointer position: wheel bursts without
     * mouse movement keep scrolling the same container, so hit-testing is
     * skipped on repeated coordinates (invalidated on layout rebuild) */
    struct whaleui_layout_node* wheel_node;
    int wheel_x, wheel_y;
    /* the element the last wheel scroll actually moved (set by the scroll
     * behavior); the frame uses it to repaint only that container's region
     * (embedded overflow boxes), while the page root keeps the image-shift
     * path */
    struct lxb_dom_element* scroll_el;
    /* scrollbar being dragged (element owning the scrollable box) */
    struct lxb_dom_element* drag_scroll_el;
    /* cached layout node of drag_scroll_el (drag frames do not rebuild the
     * layout tree, so the pointer stays valid until the next rebuild) */
    struct whaleui_layout_node* drag_scroll_node;
    /* system cursors (lazy-created; null until first used) */
    SDL_Cursor* cursor_arrow;
    SDL_Cursor* cursor_text;
    SDL_Cursor* cursor_pointer;

    /* text selection: anchor + focus (element, UTF-8 byte offset). The
     * focus end is the "active" end while dragging. In an editable element
     * the selection doubles as the caret (anchor == focus == caret).
     * selecting turns on only after the drag passes a small threshold, so a
     * plain click never creates a selection. */
    struct lxb_dom_element* sel_anchor_el;
    struct lxb_dom_element* sel_focus_el;
    int sel_anchor;
    int sel_focus;
    int selecting;
    int press_x, press_y; /* where the left button went down (drag gate) */
    /* selection extension mode: 0 = character drag, 1 = word (double-click),
     * 2 = line (triple-click). Set on button-down, drives drag continuation
     * and mouse-up retention. */
    int sel_mode;
    /* click count of the current press (1/2/3 from the platform) */
    int press_clicks;
    /* word/line drag-extension anchors (set on double/triple-click): the
     * boundary where the click started. The drag keeps this end fixed and
     * only moves the other end, so dragging away then back collapses the
     * selection to what was originally chosen (standard editor behavior).
     * Stored separately because sel_anchor/sel_focus are the display
     * bounds and may flip while dragging. */
    int sel_drag_anchor;
    int sel_drag_focus;
    /* drag-and-drop of an existing selection: press went down inside the
     * selection (drag_sel), the drag passed the threshold (drag_sel_active),
     * and ctrl was held on release (drag_copy: copy instead of move).
     * press_caret remembers where the press landed so a click (no drag)
     * collapses the selection and moves the caret there. */
    int drag_sel;
    int drag_sel_active;
    int drag_copy;
    int press_caret;
    /* remembered character column for up/down caret moves; -1 = none.
     * Reset whenever the caret moves horizontally or the text changes. */
    int nav_col;

    /* editable element with keyboard focus (input/textarea/contenteditable);
     * NULL when none. Drives SDL_StartTextInput/StopTextInput. */
    struct lxb_dom_element* edit_el;
    /* IME composition text (SDL_EVENT_TEXT_EDITING), drawn at the caret */
    std::string compose;
    /* caret position INSIDE the composition text, in UTF-8 code points
     * (SDL_TextEditingEvent.start); -1 = at the end */
    int compose_caret = -1;
    /* paint-time flow position after the pre-compose value text (x) and
     * its baseline y: the composition text + the value tail continue here */
    int compose_flow_x = -1;
    int compose_flow_y = -1;
    /* undo/redo stack (Ctrl-Z/Y): each entry replaces [a,b) of the target
     * element's value with `ins` (removing `del`). The element pointer is
     * stable across layout-tree rebuilds. */
    struct EditOp
    {
        struct lxb_dom_element* el;
        size_t a, b;
        std::string ins;
        std::string del;
    };
    std::vector<EditOp> undo_stack;
    std::vector<EditOp> redo_stack;
    /* contenteditable line storage (piece-table style): the text is held
     * as lines so edits touch only the affected line instead of rebuilding
     * the whole string. Ordinary input/textarea keep the plain string
     * path. Invalidated (erased) whenever the DOM changes outside
     * edit_replace (undo/redo/reset) and lazily rebuilt from the DOM. */
    std::map<struct lxb_dom_element*, std::vector<std::string>> edit_lines;

    /* CSS animation engine (animate.h): @keyframes + transition. Animations
     * are interpolated into the computed styles during the layout pass;
     * anim_active tells the frame loop to keep repainting. */
    struct whaleui_anim* anim;

    /* FSR 1.0 (GPU compute upscale, shaders in fsr_shaders.h).
     * fsr_mode: 0 = auto (default; enable on 4K display or on battery,
     * unless the scaled render size gets too small), 1 = on, 2 = off.
     * fb_w/fb_h = the actual render resolution (== window when FSR off,
     * window*scale when on); interaction coordinates are scaled to it. */
    int fsr_mode;
    float fsr_scale;
    float fsr_sharpness;
    int fb_w, fb_h;
    int fsr_active;
    SDL_GPUComputePipeline* fsr_easu_pipe;
    SDL_GPUComputePipeline* fsr_rcas_pipe;
    SDL_GPUTexture* offscreen_low;   /* rgba8, render res (upload target) */
    SDL_GPUTexture* fsr_up;          /* rgba8, window res (EASU out) */
    SDL_GPUTexture* fsr_out;         /* rgba8, window res (RCAS out) */
    SDL_GPUTransferBuffer* fsr_transfer; /* low-res upload buffer */

    /* GPU renderer (gpu.h): batched draw commands, no CPU framebuffer */
    struct whaleui_gpu* gpu;

    /* painted-background color (body background, cached) */
    unsigned int bg_color;

    /* global text scale (font-size multiplier, 1.0 = 100%) */
    float text_scale;

    /* dirty-rect repaint: when partial is set, only this region is cleared,
     * painted and uploaded (small animations no longer repaint the whole
     * framebuffer every frame) */
    int partial;
    int dirty_x, dirty_y, dirty_w, dirty_h;

    /* async first layout (WHALEUI_RENDER_ASYNC_LAYOUT): the first full
     * layout (no tree yet) runs on a worker thread so a large page does
     * not freeze the window while it lays out; the frame loop picks up the
     * finished tree. Only the FIRST layout is async - later full rebuilds
     * stay synchronous. The worker reads the DOM/rules/vars copy; the
     * first frame has no user interaction yet, so there is no concurrent
     * DOM write. ponytail: single-window assumption - the worker writes
     * the global text-metric context, a second window laying out
     * simultaneously would race on it. */
    int async_layout;
    std::thread* layout_thread; /* nullptr when no worker is running */
    std::atomic<int> layout_done; /* release/acquire hand-off */
    whaleui_layout_tree_t* layout_pending;
    std::map<std::string, std::string> layout_vars;
    int layout_w, layout_h;
    float layout_text_scale;

    /* subtree paint bounds are computed once per layout pass and reused by
     * every paint (and the selection sequence walk); invalidated whenever
     * the layout tree is rebuilt */
    int bounds_valid;

    /* CPU text layer: text is rasterized here (geometries go to the GPU
     * draw list), uploaded and composited into the GPU target each frame */
    std::vector<unsigned int> text_layer;
};

typedef struct whaleui_render whaleui_render_t;

/* Hit-test the last layout tree and drive the <select> interaction.
 * Returns 1 when a select option was chosen (out_value set, owned by the
 * library), 0 otherwise (the click may still have toggled a select open). */
int whaleui_render_handle_click(whaleui_render_t* r, int x, int y,
                                const char** out_value);

/* Update the hovered element from a mouse position; repaints when changed. */
void whaleui_render_set_hover(whaleui_render_t* r, int x, int y);

/* Left-button press/release: hit-tests and tracks the pressed element
 * (:active) and the focused element (:focus, set on press). Also starts a
 * text selection / caret placement on editable elements. */
void whaleui_render_set_pressed(whaleui_render_t* r, int x, int y, int down);

/* Press/release with the platform click count (1/2/3; 2 = double-click
 * selects a word, 3 = triple-click selects a line) and key modifiers.
 * Mouse-up passes the release position (x/y) so a dropped selection can be
 * placed; mods on release carries the ctrl flag for copy-drag. */
void whaleui_render_set_pressed_ex(whaleui_render_t* r, int x, int y, int down,
                                   int clicks, int mods);

/* Mouse wheel: scrolls the nearest scrollable ancestor of the element under
 * (x, y) by dy wheel ticks (x/y may be -1 to reuse the last hover pos). */
void whaleui_render_handle_wheel(whaleui_render_t* r, int x, int y, float dy);

/* Page-internal anchor: scroll the document so the element with this id is
 * visible near the top (leaving room for a fixed header). No-op when the id
 * is missing. Returns 0 on success. */
int whaleui_render_scroll_to_id(whaleui_render_t* r,
                                whaleui_dom_document_t* doc,
                                const char* id);

/* Scroll behavior hook: called for every wheel delta applied to an element.
 * delta is the pixel amount to ADD to the element's scroll position (may be
 * negative). Return 1 when the scroll position actually changed (a repaint
 * is scheduled). The default clamps to [0, scroll_max]; a custom hook can
 * implement velocity/acceleration-based smooth scrolling later. */
typedef int (*whaleui_scroll_behavior_fn)(whaleui_render_t* r,
                                          struct lxb_dom_element* el,
                                          int delta, void* userdata);
int whaleui_render_set_scroll_behavior(whaleui_render_t* r,
                                       whaleui_scroll_behavior_fn fn,
                                       void* userdata);

/* Built-in smooth scroll behavior for whaleui_render_set_scroll_behavior:
 * the wheel delta accumulates into the element's target position and the
 * frame loop eases the live position toward it (opt-in; the default
 * behavior keeps the immediate wheel -> position contract). */
int whaleui_scroll_smooth_fn(whaleui_render_t* r,
                             struct lxb_dom_element* el, int delta,
                             void* userdata);

/* Keyboard: editing keys (arrows/backspace/delete/enter) on the focused
 * editable element. mods: SDL_Keymod bitmask (for ctrl shortcuts). */
void whaleui_render_handle_key(whaleui_render_t* r, int keycode, int pressed,
                               int mods);
/* Tab / Shift+Tab focus navigation between editable controls (dir +-1);
 * disabled controls are skipped and never take the edit focus. */
void whaleui_render_focus_editable(whaleui_render_t* r, int dir);

/* Text input (SDL_EVENT_TEXT_INPUT, UTF-8): insert into the focused editable
 * element, replacing the current selection. */
void whaleui_render_handle_text(whaleui_render_t* r, const char* utf8);

/* IME composition update (SDL_EVENT_TEXT_EDITING); empty text commits. */
void whaleui_render_handle_editing(whaleui_render_t* r, const char* utf8,
                                   int caret);

/* FSR 1.0 upscaling. mode: 0 = auto (4K display or battery -> on, unless the
 * scaled render size would be too small), 1 = force on, 2 = off. scale e.g.
 * 0.5 (render at half resolution, EASU upscales), sharpness in [0,1]. */
void whaleui_render_set_fsr(whaleui_render_t* r, int mode, float scale,
                            float sharpness);

/* Create/destroy a per-window render context (device/window borrowed).
 * The window must already be claimed for the GPU device. */
whaleui_render_t* whaleui_render_create(SDL_GPUDevice* device, SDL_Window* window,
                                        int width, int height);
void whaleui_render_destroy(whaleui_render_t* render);

/* Attach the document's stylesheet (rules are copied; keyframes copied).
 * Call after load_html/load_uri and before the first frame. */
void whaleui_render_set_css(whaleui_render_t* render,
                            const whaleui_css_rule_t* rules, size_t count,
                            const whaleui_css_keyframes_t* keyframes,
                            const std::map<std::string, std::string>* theme_vars);

/* Mark the whole viewport dirty -> next frame repaints + re-lays-out. */
void whaleui_render_invalidate(whaleui_render_t* render);
void whaleui_render_invalidate_rect(whaleui_render_t* render, int x, int y, int w, int h);

/* Drop every cache keyed on DOM elements (text rasters, decoded images,
 * select/scroll/edit state, hover/focus). Call when the document is
 * replaced (window load_html) - stale element pointers must not leak
 * TTF_Text/SDL_Surface objects or dangle across documents. */
void whaleui_render_reset_dom(whaleui_render_t* render);

/* Paint one frame for the window. Returns 0 on success. */
int whaleui_render_frame(whaleui_render_t* render, whaleui_dom_document_t* doc);

/* The layout tree of the most recent frame (for DOM geometry queries like
 * getBoundingClientRect); NULL when nothing was rendered yet. The tree is
 * owned by the render context and must not be modified. */
whaleui_layout_tree_t* whaleui_render_last_tree(void);

/* DOM element under (x, y) in the current layout tree (hit test), or NULL.
 * Used by the app loop to dispatch DOM mouse events. */
whaleui_dom_element_t* whaleui_render_hit_element(whaleui_render_t* r, int x, int y);

/* The currently focused element (last clicked control), or NULL. Used by
 * the app loop to dispatch DOM keyboard events. */
whaleui_dom_element_t* whaleui_render_focus_element(whaleui_render_t* r);

/* Handle a resize: recreate offscreen target, invalidate. */
int whaleui_render_resize(whaleui_render_t* render, int width, int height);

/* Parse a CSS color ("#rgb"/"#rrggbb"/"#rrggbbaa"/named/transparent) into
 * 0xAARRGGBB. Returns 0 on success. */
int whaleui_render_parse_color(const char* s, unsigned int* out);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_RENDER_RENDER_H */
