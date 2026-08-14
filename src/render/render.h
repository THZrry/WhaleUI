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
#include <string>
#include <vector>

/* SDL3 opaque types; never dereferenced in this header. */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUTexture SDL_GPUTexture;
typedef struct SDL_GPUTransferBuffer SDL_GPUTransferBuffer;
typedef struct SDL_GPUComputePipeline SDL_GPUComputePipeline;
typedef struct TTF_Font TTF_Font;
typedef struct TTF_TextEngine TTF_TextEngine;

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_render
{
    SDL_GPUDevice* device; /* borrowed from app->gpu */
    SDL_Window* window;    /* borrowed from win->sdl */

    int width, height;     /* window size in pixels */
    int has_dirty;

    /* CPU framebuffer (0xAARRGGBB) + GPU offscreen target (B8G8R8A8 so the
     * little-endian framebuffer bytes map 1:1). */
    std::vector<unsigned int> pixels;
    SDL_GPUTexture* offscreen;
    SDL_GPUTransferBuffer* transfer;

    /* owned stylesheet + last layout tree (rebuilt on dirty) */
    whaleui_layout_tree_t* tree;
    whaleui_css_rule_t* rules;
    size_t rule_count;
    whaleui_css_keyframes_t keyframes;
    std::map<std::string, std::string> theme_vars;

    /* font cache: "family|size" -> TTF_Font */
    std::vector<std::pair<std::string, TTF_Font*>> fonts;
    TTF_Font* font_default;
    TTF_TextEngine* text_engine; /* lazy; TTF_Text supports font fallback */

    /* select dropdown interaction state. open_select is the DOM element
     * (stable across layout-tree rebuilds; layout nodes are recreated every
     * frame and would dangle) */
    struct lxb_dom_element* open_select;
    int open_select_hover;
    std::map<struct lxb_dom_element*, int> select_index; /* select -> option */

    /* element under the mouse (for :hover rules) */
    struct lxb_dom_element* hover_el;
    /* last clicked control (:focus) and the element the left button is
     * held down on (:active) */
    struct lxb_dom_element* focus_el;
    struct lxb_dom_element* pressed_el;

    /* vertical scroll per element (overflow:auto/scroll), applied at layout */
    std::map<struct lxb_dom_element*, int> scrolls;

    /* text selection: anchor + focus (element, UTF-8 byte offset). The
     * focus end is the "active" end while dragging. In an editable element
     * the selection doubles as the caret (anchor == focus == caret). */
    struct lxb_dom_element* sel_anchor_el;
    struct lxb_dom_element* sel_focus_el;
    int sel_anchor;
    int sel_focus;

    /* editable element with keyboard focus (input/textarea/contenteditable);
     * NULL when none. Drives SDL_StartTextInput/StopTextInput. */
    struct lxb_dom_element* edit_el;
    /* IME composition text (SDL_EVENT_TEXT_EDITING), drawn at the caret */
    std::string compose;

    /* color transitions (CSS `transition` on color properties): one active
     * animation per element+property, plus the last-drawn color used to
     * detect changes and keep animating */
    struct ColorAnim
    {
        struct lxb_dom_element* el;
        std::string prop;
        unsigned int from, to;
        uint64_t start; /* SDL_GetTicks ms */
        uint32_t dur;
    };
    std::vector<ColorAnim> anims;
    std::map<std::string, unsigned int> anim_last;

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

    /* painted-background color (body background, cached) */
    unsigned int bg_color;
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

/* Mouse wheel: scrolls the nearest scrollable ancestor of the element under
 * (x, y) by dy wheel ticks (x/y may be -1 to reuse the last hover pos). */
void whaleui_render_handle_wheel(whaleui_render_t* r, int x, int y, float dy);

/* Keyboard: editing keys (arrows/backspace/delete/enter) on the focused
 * editable element. mods: SDL_Keymod bitmask (for ctrl shortcuts). */
void whaleui_render_handle_key(whaleui_render_t* r, int keycode, int pressed,
                               int mods);

/* Text input (SDL_EVENT_TEXT_INPUT, UTF-8): insert into the focused editable
 * element, replacing the current selection. */
void whaleui_render_handle_text(whaleui_render_t* r, const char* utf8);

/* IME composition update (SDL_EVENT_TEXT_EDITING); empty text commits. */
void whaleui_render_handle_editing(whaleui_render_t* r, const char* utf8);

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

/* Paint one frame for the window. Returns 0 on success. */
int whaleui_render_frame(whaleui_render_t* render, whaleui_dom_document_t* doc);

/* Handle a resize: recreate offscreen target, invalidate. */
int whaleui_render_resize(whaleui_render_t* render, int width, int height);

/* Parse a CSS color ("#rgb"/"#rrggbb"/"#rrggbbaa"/named/transparent) into
 * 0xAARRGGBB. Returns 0 on success. */
int whaleui_render_parse_color(const char* s, unsigned int* out);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_RENDER_RENDER_H */
