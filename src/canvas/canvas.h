#ifndef WHALEUI_CANVAS_CANVAS_H
#define WHALEUI_CANVAS_CANVAS_H

/* Canvas 2D - internal interface (CPU backing store, C-port of
 * HTMLCanvasElement + CanvasRenderingContext2D subset).
 *
 * Backing store is a per-canvas-element CPU RGBA buffer (0xAARRGGBB, same
 * layout as render pixels). The render paint path blits it into the frame
 * like an <img> bitmap. GPU upload reuse comes later; correctness first. */

#include "whaleui.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct lxb_dom_element;

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_canvas_context
{
    lxb_dom_element* el;
    int w;
    int h;
    std::vector<unsigned int> px; /* 0xAARRGGBB */
    /* 2d state */
    unsigned int fill_style;
    unsigned int stroke_style;
    float line_width;
    float global_alpha;
    std::string font;
    std::string text_align;
    std::string text_baseline;
    /* simple path: list of ops for fill/stroke of rects */
    struct PathOp { int kind; float x0, y0, x1, y1; float x2, y2; };
    std::vector<PathOp> path;
    /* transform: 2D matrix a,b,c,d,e,f */
    float m_a, m_b, m_c, m_d, m_e, m_f;
    struct SavedState {
        unsigned int fill_style, stroke_style;
        float line_width, global_alpha;
        float m_a, m_b, m_c, m_d, m_e, m_f;
    };
    std::vector<SavedState> stack;
    unsigned long long seq; /* bumped on every mutation; paint caches on it */
};

struct whaleui_image_data
{
    int w;
    int h;
    std::vector<unsigned int> px;
};

/* --- text backend (canvas_text.cpp) ---
 *
 * Canvas 2D text needs a rasterizer outside the render context (a canvas
 * may be filled from application code with no page text involved). Full
 * builds use SDL3_ttf through the same font registry the page text uses;
 * lite/minimal have no TTF backend yet, so the text APIs report failure
 * there instead of drawing wrong glyphs.
 *
 * font_css is the CanvasRenderingContext2D `font` string ("bold 16px
 * Arial"): the parser reads an optional weight/style prefix, the size in
 * px and the family list. These are internal C++ helpers (std::vector
 * out-param), not part of the C ABI. */

/* Measure UTF-8 text. Returns 0 on success (w/h set). */
int whaleui_canvas_text_measure(const char* utf8, const char* font_css,
                                int* out_w, int* out_h);

/* Rasterize UTF-8 text into straight-alpha 0xAARRGGBB pixels. On success
 * out_px is (out_w x out_h) and (xoff, yoff) is the top-left offset from
 * the text origin (baseline start). Returns 0 on success. */
int whaleui_canvas_text_raster(const char* utf8, const char* font_css,
                               unsigned int color,
                               std::vector<unsigned int>& out_px,
                               int* out_w, int* out_h,
                               int* out_xoff, int* out_yoff);

/* element -> context registry (owned here; element destroy drops it) */
whaleui_canvas_context_t* whaleui_canvas_context_for(struct lxb_dom_element* el);
void whaleui_canvas_drop(struct lxb_dom_element* el);
void whaleui_canvas_drop_all(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CANVAS_CANVAS_H */
