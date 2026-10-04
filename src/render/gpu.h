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
 * (r/g/b = shadow color, a = shadow alpha; for a backdrop record r = the
 * corner radius px (SDF clip in kBackdropCS), a = body alpha - which is 0
 * since the body paints in the late pass - and br/bg/bb = body color). */
struct gpu_blur_param
{
    float x, y, w, h;    /* region, px (framebuffer coords) */
    float blur;          /* blur radius px (selects the mip level) */
    float r, g, b;       /* shadow color (box-shadow) */
    float a;             /* shadow alpha (box-shadow) */
    float br, bg, bb;    /* backdrop body color (blended over the blur) */
};

/* per-inset-shadow record: 16 floats = 4 float4s (kInsetCS reads the *4
 * stride). The shadow = 1 - blurred_mask(px - offset) of the spread-shrunk
 * box mask, clipped to the element's rounded box. blur 0 is legal (crisp
 * ring: the CS reads the unblurred level-0 mask). */
struct gpu_inset_param
{
    float x, y, w, h;    /* element box, px (framebuffer coords) */
    float blur;          /* blur radius px */
    float r, g, b;       /* shadow color */
    float a;             /* shadow alpha */
    float ox, oy;        /* offset px (positive ox lights the LEFT edge) */
    float spread;        /* spread px (>= 0, shrinks the mask source) */
    float radius;        /* corner radius px (rounded clip) */
    float pad0, pad1, pad2;
};

/* per-filter record: 24 floats = 6 float4s (kFilterCS reads the *6 stride).
 * The filter applies to the element's own painted content (geometry + text,
 * i.e. after the text composite): optional mip blur of the composited frame
 * plus a 4x5 color matrix. blur 0 skips the blur tap. Each matrix row is
 * one float4 [c0,c1,c2,bias]. */
struct gpu_filter_param
{
    float x, y, w, h;    /* element box, px (framebuffer coords) */
    float blur;          /* blur radius px (0 = color matrix only) */
    float radius;        /* corner radius px (rounded clip) */
    float pad0, pad1;
    float r0[4];         /* matrix rows, bias in [3] */
    float r1[4];
    float r2[4];
    float r3[4];
};

/* per-mix-blend record: 12 floats = 3 float4s (kMixBlendCS *3 stride).
 * src = the element body's own color (solid/gradient corner average);
 * dst = the painted frame behind it. Subtree content does not participate
 * (documented approximation). */
struct gpu_mixblend_param
{
    float x, y, w, h;    /* element box, px (framebuffer coords) */
    float mode;          /* blend_channel id (0..15) */
    float radius;        /* corner radius px (rounded clip) */
    float pad0, pad1;
    float sr, sg, sb, sa; /* body source color */
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
    SDL_GPUComputePipeline* pipe_filter_cs; /* blur_tex -> target2 (filter) */
    SDL_GPUComputePipeline* pipe_mixblend_cs; /* frame -> target2 (mix-blend) */

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
    SDL_GPUBuffer* vb_solid_late; /* backdrop bodies (late draw list) */
    SDL_GPUTransferBuffer* vb_transfer;
    SDL_GPUTransferBuffer* atlas_transfer; /* full-atlas upload (4MB) */
    SDL_GPUTransferBuffer* layer_transfer[2]; /* text-layer upload, ping-pong:
        two buffers so the next frame's Map does not wait for the GPU to
        finish reading the previous upload (a 2k strip upload takes ~0.5ms
        of GPU time; a single buffer serialized the CPU on it) */
    int layer_cur; /* index of the buffer used by the current upload */

    /* blur machinery buffers: vb_shapes holds the solid quads drawn into
     * blur_tex (shadow shapes + inset masks, two vertex runs); the param
     * storage buffers feed the compute blur passes (one transfer buffer
     * uploads all three) */
    SDL_GPUBuffer* vb_shapes;
    SDL_GPUBuffer* shadow_params_buf;
    SDL_GPUBuffer* backdrop_params_buf;
    SDL_GPUBuffer* inset_params_buf;
    SDL_GPUBuffer* filter_params_buf;
    SDL_GPUBuffer* mixblend_params_buf;
    SDL_GPUTransferBuffer* shadow_transfer;

    /* per-frame command lists */
    std::vector<gpu_vert_solid> solids;
    std::vector<gpu_vert_text> texts;
    float fb_w, fb_h; /* render resolution (per-vertex NDC conversion) */

    /* box-shadow / backdrop-filter records (fed to the compute passes) */
    std::vector<gpu_blur_param> shadows;
    std::vector<gpu_blur_param> backdrops;
    /* inset box-shadow records (blended over the geometry after paint) */
    std::vector<gpu_inset_param> insets;
    /* filter records (applied to the composited frame after text) */
    std::vector<gpu_filter_param> filters;
    /* mix-blend records (body color over the painted frame) */
    std::vector<gpu_mixblend_param> mixblends;
    /* shadow shapes (white rounded rects) painted into blur_tex (pass A) */
    std::vector<gpu_vert_solid> shapes;
    /* inset mask shapes (spread-shrunk white rounded rects) painted into
     * blur_tex (pass A2; a separate vertex run of vb_shapes) */
    std::vector<gpu_vert_solid> inset_shapes;
    /* late draw list: the body of a backdrop-filter element (solid +
     * gradient + border), routed here so the backdrop compute pass - which
     * REPLACES its region with the blurred geometry - cannot clobber it.
     * Drawn after all compute passes of the frame (whaleui_gpu_late_begin/
     * late_end bracket the body paint in render_paint.cpp). */
    std::vector<gpu_vert_solid> solids_late;
    int late_mode; /* routing flag: fill_* quads go to solids_late while set */

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

/* resize: rebuild only the size-bound textures + text-layer transfer,
 * keeping the compiled shaders, pipelines, samplers, atlas and vertex
 * buffers (none depend on size). A full create/destroy per resize event
 * recompiles every shader (DXC) - that is what froze windows while
 * dragging the edge. Returns 0 on success. */
int whaleui_gpu_resize(whaleui_gpu_t* g, int w, int h);

/* push one solid quad (px coords). clip: NULL or a rectangle to
 * intersect (rounded corners are approximated when clipped). */
void whaleui_gpu_rect(whaleui_gpu_t* g, float x, float y, float w, float h,
                      float radius, unsigned int color, const int* clip);

/* push a solid quad rotated by rot_deg degrees (clockwise, CSS rotate
 * direction) around the (ax, ay) framebuffer anchor (from
 * whaleui_transform_origin_eval; default the box center), with optional
 * skew (degrees, sheared around the same anchor before rotation). The four
 * corner positions are transformed, the per-vertex u/v/size/radius stay in
 * the box's local frame - the SDF rounding then interpolates exactly as
 * unrotated (affine invariance), so rounded corners and pills stay correct
 * under any angle. Zero extra texture/draw cost. */
void whaleui_gpu_rect_rot(whaleui_gpu_t* g, float x, float y, float w,
                          float h, float radius, unsigned int color,
                          float rot_deg, const int* clip,
                          float ax = -1.0f, float ay = -1.0f,
                          float skx_deg = 0.0f, float sky_deg = 0.0f);

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

/* push an inset box-shadow: the shadow is 1 - blurred_mask(px - offset) of
 * the spread-shrunk rounded box, blended over the painted geometry inside
 * the element's rounded box (kInsetCS). blur 0 draws a crisp ring. ox/oy
 * shift the sample point (CSS direction: positive ox lights the LEFT
 * edge), spread shrinks the shadow region. */
void whaleui_gpu_inset(whaleui_gpu_t* g, float x, float y, float w, float h,
                       float radius, float blur, float ox, float oy,
                       float spread, unsigned int color);

/* bracket a backdrop-filter element's own body paint (background, gradient,
 * border): while set, fill_* quads go to solids_late instead of solids, so
 * the backdrop compute pass (which replaces the region with the blurred
 * geometry) cannot clobber the body - it draws on top, after the blur. */
void whaleui_gpu_late_begin(whaleui_gpu_t* g);
void whaleui_gpu_late_end(whaleui_gpu_t* g);

/* push a backdrop-filter region: after the geometry pass, the target is
 * copied into blur_tex, mipmapped, and a compute pass replaces the region
 * with the blurred geometry (clipped to `radius` when set). The element's
 * body paints in the LATE pass on top of the blur
 * (whaleui_gpu_late_begin); body_color is accepted for API stability and
 * ignored. */
void whaleui_gpu_backdrop(whaleui_gpu_t* g, float x, float y, float w,
                          float h, float radius, float blur,
                          unsigned int body_color);

/* push an element filter region: after the text composite (target2 holds
 * the full frame), an optional mip blur of the composited frame plus a 4x5
 * color matrix applies to the element's own painted content (kFilterCS).
 * m is the merged matrix from parse_filter; blur 0 = matrix only. */
void whaleui_gpu_filter(whaleui_gpu_t* g, float x, float y, float w,
                        float h, float radius, float blur,
                        const float m[4][5]);

/* push an element mix-blend region: the body's own color (srgb + alpha)
 * blends over the painted frame with the mode formula (kMixBlendCS).
 * Runs with the filter pass (after the text composite). Subtree content
 * does not participate (documented approximation). */
void whaleui_gpu_mixblend(whaleui_gpu_t* g, float x, float y, float w,
                           float h, float radius, int mode,
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
