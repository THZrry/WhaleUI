/* Renderer: public seam implementation.
 * Step 2: contract stub - state tracking only, no GPU work yet. Step 3 fills
 * in the SDL3 GPU pipeline (shaders, draw list, caches). */

#include "render/render.h"

extern "C" whaleui_render_t* whaleui_render_create(SDL_GPUDevice* device, SDL_Window* window)
{
    whaleui_render_t* render = new whaleui_render_t;
    render->device = device;
    render->window = window;
    render->dirty_x = render->dirty_y = 0;
    render->dirty_w = render->dirty_h = 0;
    render->has_dirty = 0;
    render->width = render->height = 0;
    render->pipeline = nullptr;
    return render;
}

extern "C" void whaleui_render_destroy(whaleui_render_t* render)
{
    delete render;
}

extern "C" void whaleui_render_invalidate(whaleui_render_t* render)
{
    if (!render) {
        return;
    }
    render->dirty_x = 0;
    render->dirty_y = 0;
    render->dirty_w = render->width;
    render->dirty_h = render->height;
    render->has_dirty = 1;
}

extern "C" void whaleui_render_invalidate_rect(whaleui_render_t* render, int x, int y, int w, int h)
{
    if (!render || w <= 0 || h <= 0) {
        return;
    }
    if (!render->has_dirty) {
        render->dirty_x = x;
        render->dirty_y = y;
        render->dirty_w = w;
        render->dirty_h = h;
        render->has_dirty = 1;
        return;
    }
    /* union with existing dirty rect */
    int x2 = render->dirty_x + render->dirty_w;
    int y2 = render->dirty_y + render->dirty_h;
    int nx2 = x + w, ny2 = y + h;
    render->dirty_x = render->dirty_x < x ? render->dirty_x : x;
    render->dirty_y = render->dirty_y < y ? render->dirty_y : y;
    render->dirty_w = (x2 > nx2 ? x2 : nx2) - render->dirty_x;
    render->dirty_h = (y2 > ny2 ? y2 : ny2) - render->dirty_y;
}

extern "C" int whaleui_render_frame(whaleui_render_t* render)
{
    if (!render) {
        return -1;
    }
    /* Step 2: no GPU yet. Consume the dirty flag. */
    render->has_dirty = 0;
    return 0;
}

extern "C" int whaleui_render_resize(whaleui_render_t* render, int width, int height)
{
    if (!render || width <= 0 || height <= 0) {
        return -1;
    }
    render->width = width;
    render->height = height;
    whaleui_render_invalidate(render);
    return 0;
}
