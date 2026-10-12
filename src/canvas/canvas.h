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

/* element -> context registry (owned here; element destroy drops it) */
whaleui_canvas_context_t* whaleui_canvas_context_for(struct lxb_dom_element* el);
void whaleui_canvas_drop(struct lxb_dom_element* el);
void whaleui_canvas_drop_all(void);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_CANVAS_CANVAS_H */
