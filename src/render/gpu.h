#ifndef WHALEUI_RENDER_GPU_H
#define WHALEUI_RENDER_GPU_H

/* GPU renderer: SDL_GPU with intermediate shaders compiled at runtime
 * (HLSL -> DXIL via D3DCompile for the D3D12 backend; the same sources
 * cross-compile to SPIR-V/MSL on other backends). The layout pass stays
 * CPU-side; painting becomes a batch of vertex commands submitted a few
 * times per frame (no per-element GPU round-trips, no CPU framebuffer). */

#include "whaleui.h"

#include <cstdint>
#include <vector>

/* SDL3 opaque types */
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUTexture SDL_GPUTexture;
typedef struct SDL_GPUBuffer SDL_GPUBuffer;
typedef struct SDL_GPUTransferBuffer SDL_GPUTransferBuffer;
typedef struct SDL_GPUGraphicsPipeline SDL_GPUGraphicsPipeline;
typedef struct SDL_GPUSampler SDL_GPUSampler;
typedef struct SDL_GPUCommandBuffer SDL_GPUCommandBuffer;
typedef struct SDL_GPUComputePipeline SDL_GPUComputePipeline;

#ifdef __cplusplus
extern "C" {
#endif

/* solid (rounded rect, vertex-color gradient) quad, px coordinates */
struct gpu_vert_solid
{
    float x, y;        /* pixel position (top-left of the quad) */
    float u, v;        /* 0..1 inside the quad */
    float r, g, b, a;  /* vertex color */
    float size_x, size_y; /* quad size in px */
    float radius;      /* corner radius px */
    float fb_w, fb_h;  /* framebuffer size in px (per-vertex: SDL 3.4 D3D12
                          DXIL vertex uniform crashes pipeline creation) */
};

/* text sprite quad, atlas UVs */
struct gpu_vert_text
{
    float x, y;        /* pixel position */
    float u, v;        /* atlas UV (top-left) */
    float r, g, b, a;  /* tint */
    float u2, v2;      /* atlas UV (bottom-right) */
};

/* per-window GPU rendering state */
struct whaleui_gpu
{
    SDL_GPUDevice* device;

    /* pipelines */
    SDL_GPUGraphicsPipeline* pipe_solid;
    SDL_GPUGraphicsPipeline* pipe_text;
    SDL_GPUSampler* sampler;
    SDL_GPUComputePipeline* pipe_text_composite; /* text layer -> target */

    /* 1x1 white texture for the solid pipeline */
    SDL_GPUTexture* white_tex;
    SDL_GPUTexture* glyph_atlas; /* 2048x2048 R8 */
    SDL_GPUTexture* target;      /* geometry render target (COLOR_TARGET) */
    SDL_GPUTexture* target_b;    /* second geometry target (scroll ping-pong) */
    SDL_GPUTexture* geom_cur;    /* == target or target_b (draw target) */
    SDL_GPUTexture* target2;     /* composited result (COMPUTE_STORAGE_WRITE);
                                    compute reads target + text_layer into it */
    SDL_GPUTexture* text_layer;  /* CPU-rasterized text (RGBA8) */

    /* dynamic vertex buffers */
    SDL_GPUBuffer* vb_solid;
    SDL_GPUBuffer* vb_text;
    SDL_GPUTransferBuffer* vb_transfer;
    SDL_GPUTransferBuffer* atlas_transfer; /* full-atlas upload (4MB) */
    SDL_GPUTransferBuffer* layer_transfer; /* text-layer upload */

    /* per-frame command lists */
    std::vector<gpu_vert_solid> solids;
    std::vector<gpu_vert_text> texts;
    float fb_w, fb_h; /* render resolution (per-vertex NDC conversion) */

    /* glyph atlas CPU-side occupancy (R8) */
    std::vector<unsigned char> atlas;
    int atlas_cx, atlas_cy; /* next free position */
    int atlas_row_h;
    uint32_t atlas_w, atlas_h;
    int atlas_dirty;
    int layer_dirty; /* text layer uploaded + needs compositing */
};

typedef struct whaleui_gpu whaleui_gpu_t;

/* create/destroy GPU rendering state (device borrowed). target format is
 * the offscreen texture the swapchain blits from. */
whaleui_gpu_t* whaleui_gpu_create(SDL_GPUDevice* device, int w, int h);
void whaleui_gpu_destroy(whaleui_gpu_t* g);

/* push one solid quad (px coords). clip: NULL or a rectangle to
 * intersect (rounded corners are approximated when clipped). */
void whaleui_gpu_rect(whaleui_gpu_t* g, float x, float y, float w, float h,
                      float radius, unsigned int color, const int* clip);

/* push a text sprite; atlas uv comes from the glyph atlas manager */
void whaleui_gpu_text(whaleui_gpu_t* g, float x, float y, float w, float h,
                      float u, float v, float u2, float v2,
                      unsigned int color);

/* reserve a rect in the glyph atlas (returns 0 and sets *x/*y on success;
 * -1 when full). Caller then copies glyph pixels into g->atlas and marks
 * it dirty with whaleui_gpu_atlas_dirty. */
int whaleui_gpu_atlas_alloc(whaleui_gpu_t* g, int w, int h, int* x, int* y);

/* mark the atlas CPU buffer as changed: the next flush uploads it */
void whaleui_gpu_atlas_dirty(whaleui_gpu_t* g);

/* upload the CPU text layer (RGBA8, fb_w x fb_h) and composite it into the
 * target during the next flush. Call right before whaleui_gpu_flush. */
void whaleui_gpu_text_layer(whaleui_gpu_t* g, const unsigned int* pixels,
                            int w, int h);

/* submit the collected commands: upload vertices, render to the offscreen
 * target, return the (not yet submitted) command buffer so the caller can
 * blit to the swapchain and submit. scroll_dy != 0 shifts the previous
 * frame's geometry via a ping-pong blit (the caller repaints the exposed
 * strip). Returns NULL on failure. */
SDL_GPUCommandBuffer* whaleui_gpu_flush(whaleui_gpu_t* g, int fb_w, int fb_h,
                                        unsigned int clear_color, int scroll_dy);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_RENDER_GPU_H */
