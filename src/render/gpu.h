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

/* per-shadow / per-backdrop parameter record for the compute blur passes.
 * The GPU blur is the mipmap approximation: the source (a rounded-rect
 * shape for box-shadow, the painted geometry for backdrop-filter) goes into
 * the low-res mipmapped blur_tex, and a compute shader re-samples several
 * mip levels (gaussian weights) straight into the target - no per-pixel
 * kernel, no graphics-side texture sampling (SDL 3.4 D3D12 graphics SRV is
 * broken: kSolidPS works around it with vertex colors).
 * Layout is 12 floats = 3 float4s as the shaders index them:
 *   {x,y,w,h} {blur,r,g,b} {a,br,bg,bb}
 * (r/g/b = shadow color, a = shadow alpha; br/bg/bb = backdrop body
 * color, a = body alpha when used as a backdrop record). */
struct gpu_blur_param
{
    float x, y, w, h;    /* region, px (framebuffer coords) */
    float blur;          /* blur radius px (selects the mip level) */
    float r, g, b;       /* shadow color (box-shadow) */
    float a;             /* shadow alpha (box-shadow) */
    float br, bg, bb;    /* backdrop body color (blended over the blur) */
};

/* per-window GPU rendering state */
struct whaleui_gpu
{
    SDL_GPUDevice* device;

    /* pipelines */
    SDL_GPUGraphicsPipeline* pipe_solid;
    SDL_GPUGraphicsPipeline* pipe_text;
    SDL_GPUSampler* sampler;
    SDL_GPUSampler* sampler_mip; /* linear min/mag + linear mip filter */
    SDL_GPUComputePipeline* pipe_text_composite; /* text layer -> target */
    SDL_GPUComputePipeline* pipe_shadow_cs;  /* blur_tex -> target (shadows) */
    SDL_GPUComputePipeline* pipe_backdrop_cs; /* blur_tex -> target (backdrop) */
    SDL_GPUComputePipeline* pipe_inset_cs;  /* blur_tex -> target (inset) */
    SDL_GPUGraphicsPipeline* pipe_solid_flat; /* vertex color only (no SDF) */

    /* 1x1 white texture for the solid pipeline */
    SDL_GPUTexture* white_tex;
    SDL_GPUTexture* glyph_atlas; /* 2048x2048 R8 */
    SDL_GPUTexture* target;      /* geometry render target (COLOR_TARGET) */
    SDL_GPUTexture* target_b;    /* second geometry target (scroll ping-pong) */
    SDL_GPUTexture* geom_cur;    /* == target or target_b (draw target) */
    SDL_GPUTexture* target2;     /* composited result (COMPUTE_STORAGE_WRITE);
                                    compute reads target + text_layer into it */
    /* low-res mipmapped texture backing the blur approximation. Each frame
     * it holds either the shadow shapes (white rounded rects) or a copy of
     * the geometry (backdrop-filter), then SDL_GenerateMipmapsForGPUTexture
     * builds the chain and kShadowPS re-samples several levels. */
    SDL_GPUTexture* blur_tex;
    int blur_w, blur_h; /* blur_tex size = fb / kBlurDiv */
    /* CPU-rasterized text (RGBA8) */
    SDL_GPUTexture* text_layer;
    /* last text-layer upload region (pixels_per_row keeps the full-layer
     * row stride; flush copies only this sub-rect into the texture) */
    int layer_rx, layer_ry, layer_rw, layer_rh;

    /* dynamic vertex buffers */
    SDL_GPUBuffer* vb_solid;
    SDL_GPUBuffer* vb_text;
    SDL_GPUTransferBuffer* vb_transfer;
    SDL_GPUTransferBuffer* atlas_transfer; /* full-atlas upload (4MB) */
    SDL_GPUTransferBuffer* layer_transfer; /* text-layer upload */

    /* blur machinery buffers: vb_shapes holds the solid quads drawn into
     * blur_tex (shadow shapes + backdrop body fills); the param storage
     * buffers feed the compute blur passes (one transfer buffer uploads all
     * three) */
    SDL_GPUBuffer* vb_shapes;
    SDL_GPUBuffer* shadow_params_buf;
    SDL_GPUBuffer* backdrop_params_buf;
    SDL_GPUBuffer* inset_params_buf;
    SDL_GPUTransferBuffer* shadow_transfer;

    /* per-frame command lists */
    std::vector<gpu_vert_solid> solids;
    std::vector<gpu_vert_text> texts;
    float fb_w, fb_h; /* render resolution (per-vertex NDC conversion) */

    /* box-shadow / backdrop-filter records (fed to the compute passes) */
    std::vector<gpu_blur_param> shadows;
    std::vector<gpu_blur_param> backdrops;
    /* inset box-shadow records (blended over the geometry after paint) */
    std::vector<gpu_blur_param> insets;
    /* shadow shapes (white rounded rects) painted into blur_tex (pass A) */
    std::vector<gpu_vert_solid> shapes;
    /* inset gradient triangles painted into blur_tex (pass A2) */
    std::vector<gpu_vert_solid> inset_shapes;

    /* glyph atlas CPU-side occupancy (R8) */
    std::vector<unsigned char> atlas;
    int atlas_cx, atlas_cy; /* next free position */
    int atlas_row_h;
    uint32_t atlas_w, atlas_h;
    int atlas_dirty;
    int layer_dirty; /* text layer uploaded + needs compositing */
};

typedef struct whaleui_gpu whaleui_gpu_t;

/* blur texture scale: blur_tex is fb / kBlurDiv on each axis */
enum { kBlurDiv = 2, kBlurLevels = 8 };

/* create/destroy GPU rendering state (device borrowed). target format is
 * the offscreen texture the swapchain blits from. */
whaleui_gpu_t* whaleui_gpu_create(SDL_GPUDevice* device, int w, int h);
void whaleui_gpu_destroy(whaleui_gpu_t* g);

/* push one solid quad (px coords). clip: NULL or a rectangle to
 * intersect (rounded corners are approximated when clipped). */
void whaleui_gpu_rect(whaleui_gpu_t* g, float x, float y, float w, float h,
                      float radius, unsigned int color, const int* clip);

/* push a quad whose four corners carry distinct colors (c0=TL, c1=TR,
 * c2=BL, c3=BR); the solid pipeline interpolates them across the quad, so
 * linear CSS gradients fall out of the rasterizer for free. */
void whaleui_gpu_gradient_rect(whaleui_gpu_t* g, float x, float y, float w,
                               float h, unsigned int c0, unsigned int c1,
                               unsigned int c2, unsigned int c3,
                               const int* clip);

/* push a soft box-shadow: a rounded-rect shape (radius, px, framebuffer
 * coords) blurred by `blur` px and drawn with `color` (0xAARRGGBB) under
 * the element. The blur is the mipmap approximation: the shape is painted
 * into the low-res blur_tex, mipmapped, and a compute pass re-samples
 * several levels back into the target. */
void whaleui_gpu_shadow(whaleui_gpu_t* g, float x, float y, float w, float h,
                        float radius, float blur, unsigned int color);

/* push an inset box-shadow: the box interior is split along the two
 * diagonals into four triangles whose vertex alpha ramps from 1 (edges)
 * to 0 (center); the linear gradient is rasterized into blur_tex, mipmap
 * blurred (same pyramid as the outer shadow) and a compute pass blends it
 * over the painted geometry AFTER the geometry pass (inset shadows sit on
 * top of the element background). radius is ignored (angular approximation:
 * the diagonal split covers the corners). */
void whaleui_gpu_inset(whaleui_gpu_t* g, float x, float y, float w, float h,
                       float radius, float blur, unsigned int color);

/* push a backdrop-filter region: after the geometry pass, the target is
 * copied into blur_tex, mipmapped, and a compute pass replaces the region
 * with the blurred geometry. body_color (may be 0) is blended on top
 * afterwards (the element's own background). */
void whaleui_gpu_backdrop(whaleui_gpu_t* g, float x, float y, float w,
                          float h, float radius, float blur,
                          unsigned int body_color);

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
 * target during the next flush. Call right before whaleui_gpu_flush. Only
 * the (rx, ry, rw, rh) region is uploaded - partial repaints (dirty-rect
 * animations) upload just the painted area instead of the whole layer. */
void whaleui_gpu_text_layer(whaleui_gpu_t* g, const unsigned int* pixels,
                            int w, int h, int rx, int ry, int rw, int rh);

/* submit the collected commands: upload vertices, render to the offscreen
 * target, return the (not yet submitted) command buffer so the caller can
 * blit to the swapchain and submit. scroll_dy != 0 shifts the previous
 * frame's geometry via a ping-pong blit (the caller repaints the exposed
 * strip). load_only skips the clear (dirty-rect repaints). Returns NULL
 * on failure. */
SDL_GPUCommandBuffer* whaleui_gpu_flush(whaleui_gpu_t* g, int fb_w, int fb_h,
                                        unsigned int clear_color, int scroll_dy,
                                        int load_only);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_RENDER_GPU_H */
