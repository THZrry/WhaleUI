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

#include <map>
#include <string>
#include <vector>

/* SDL3 opaque types; never dereferenced in this header. */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUTexture SDL_GPUTexture;
typedef struct SDL_GPUTransferBuffer SDL_GPUTransferBuffer;
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

    /* select dropdown interaction state */
    whaleui_layout_node_t* open_select;  /* currently expanded <select>, or NULL */
    int open_select_hover;               /* hovered option index in the list */
    std::map<whaleui_layout_node_t*, int> select_index; /* select -> chosen option */

    /* painted-background color (body background, cached) */
    unsigned int bg_color;
};

typedef struct whaleui_render whaleui_render_t;

/* Hit-test the last layout tree and drive the <select> interaction.
 * Returns 1 when a select option was chosen (out_value set, owned by the
 * library), 0 otherwise (the click may still have toggled a select open). */
int whaleui_render_handle_click(whaleui_render_t* r, int x, int y,
                                const char** out_value);

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
