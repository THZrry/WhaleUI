#ifndef WHALEUI_RENDER_RENDER_H
#define WHALEUI_RENDER_RENDER_H

/* Renderer - internal interface.
 *
 * One render context per window, backed by a SDL3 GPU device (shared from the
 * app). Step 3 fills in: GPU pipeline, shaders (SPIR-V intermediate for
 * Vulkan/D3D11/GL4.6), dirty-rect tracking, occlusion, and texture/font
 * caches. This header fixes the seam the rest of the engine renders into. */

#include "whaleui.h"

/* SDL3 opaque types; never dereferenced in this header. */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_GPUDevice SDL_GPUDevice;

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_render
{
    SDL_GPUDevice* device; /* borrowed from app->gpu */
    SDL_Window* window;    /* borrowed from win->sdl */

    /* dirty-rect accumulation (step 3: min/max tracking + rect list) */
    int dirty_x, dirty_y, dirty_w, dirty_h;
    int has_dirty;

    /* swapchain/viewport cached from the last resize */
    int width, height;

    /* step 3: draw list, texture cache, font atlas, shader pipeline */
    void* pipeline; /* placeholder, owned by render impl */
};

typedef struct whaleui_render whaleui_render_t;

/* Create/destroy a per-window render context (device/window borrowed). */
whaleui_render_t* whaleui_render_create(SDL_GPUDevice* device, SDL_Window* window);
void whaleui_render_destroy(whaleui_render_t* render);

/* Mark the whole viewport (or a rect) dirty -> next frame repaints it. */
void whaleui_render_invalidate(whaleui_render_t* render);
void whaleui_render_invalidate_rect(whaleui_render_t* render, int x, int y, int w, int h);

/* Render one frame for the window. Returns 0 on success. */
int whaleui_render_frame(whaleui_render_t* render);

/* Handle a resize: update cached viewport, invalidate. */
int whaleui_render_resize(whaleui_render_t* render, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_RENDER_RENDER_H */
