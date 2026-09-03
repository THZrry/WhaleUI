#ifndef WHALEUI_RENDER_INTERNAL_H
#define WHALEUI_RENDER_INTERNAL_H

/* Shared helpers between the split render sources: render.cpp (context +
 * frame + interaction) and render_color/paint/text/control/fsr.cpp.
 * Internal only - not part of the public C API. */

#include "render/render.h"
#include "render/gpu.h"

#include <string>
#include <vector>

/* font style bits (values mirror SDL_ttf's TTF_STYLE_*; local constants so
 * lite/minimal builds don't need SDL3_ttf headers) */
enum { kFontBold = 1, kFontItalic = 2 };

/* <select> option row height (px) */
const int kSelectItemH = 26;

/* paint-pass globals (defined in render.cpp, outside any namespace) */
/* per-call wall-clock timing of a hot function (WHALEUI_PERF_LOG only):
 * accumulate across calls, print the average every 60. Independent of any
 * frame-level segment chain, so it cannot hide cost. */
#ifdef WHALEUI_PERF_LOG
#define PERF_FN(name)                                                        \
    static Uint64 pf_##name##_t = 0;                                         \
    static int pf_##name##_n = 0;                                            \
    Uint64 pf_##name##_0 = SDL_GetPerformanceCounter();                      \
    (void)pf_##name##_0
#define PERF_FN_END(name)                                                    \
    pf_##name##_t += SDL_GetPerformanceCounter() - pf_##name##_0;            \
    if (++pf_##name##_n >= 30) {                                             \
        fprintf(stderr, "[pf] %s=%.3f ms\n", #name,                          \
                (double)pf_##name##_t /                                     \
                    (double)SDL_GetPerformanceFrequency() * 1000.0 /         \
                    (double)pf_##name##_n);                                 \
        pf_##name##_t = 0;                                                   \
        pf_##name##_n = 0;                                                   \
    }
#else
#define PERF_FN(name) ((void)0)
#define PERF_FN_END(name) ((void)0)
#endif

extern whaleui_gpu_t* g_gpu;
/* run a function on the SDL window/message thread (main thread). SDL
 * window/IME calls (StartTextInput/Stop/SetTextInputArea) are bound to
 * that thread on Windows (IMM context); the render worker must post them. */
extern "C" void whaleui_sdl_on_main(void (*fn)(void*), void* ud);
extern int g_backdrop_active;
extern whaleui_layout_tree_t* g_last_tree;
extern whaleui_render_t* g_metric_render;

/* --- shared types --- */

struct Clip { int x, y, w, h; };
struct TRect { int x, y, w, h; };

struct GradStop
{
    unsigned int c;
    float pos; /* 0..1 */
};

struct Gradient
{
    int type;                  /* 0=linear, 1=radial */
    float angle_deg;           /* linear direction */
    float rx, ry;              /* radial radii (px) */
    float cx, cy;              /* radial center (fraction of the box) */
    std::vector<GradStop> stops;
};

/* --- color (render_color.cpp) --- */

unsigned int hex_byte(const char* s, int* ok);
unsigned int hex_nib(char c, int* ok);
void split_grad_parts(const std::string& s, std::vector<std::string>& out);
int parse_gradient(const std::string& v, Gradient& out);
unsigned int grad_color(const Gradient& g, float t);
void fill_gradient(std::vector<unsigned int>& fb, int fbw, int fbh,
                   int x, int y, int w, int h, const Gradient& g,
                   const Clip* clip);
unsigned int color_of(const WhaleUIComputedStyle& s, const char* prop,
                      unsigned int def);
unsigned int border_color_of(const WhaleUIComputedStyle& s, unsigned int def);

/* --- paint primitives (render_paint.cpp) --- */

void dirty_rect(int x0, int y0, int x1, int y1, Clip* out);
void clip_rect(int& x0, int& y0, int& x1, int& y1, const Clip* clip);
void fill_rect(std::vector<unsigned int>& fb, int fbw, int fbh,
               int x, int y, int w, int h, unsigned int color,
               const Clip* clip);
bool inside_rounded(int px, int py, int x, int y, int w, int h, int radius);
void fill_round_rect(std::vector<unsigned int>& fb, int fbw, int fbh,
                     int x, int y, int w, int h, int radius,
                     unsigned int color, const Clip* clip);
void fill_round_border(std::vector<unsigned int>& fb, int fbw, int fbh,
                       int x, int y, int w, int h, int radius, int bw,
                       unsigned int color, unsigned int bg, const Clip* clip);
void blend_surface(std::vector<unsigned int>& fb, int fbw, int fbh,
                   const SDL_Surface* surf, int dx, int dy, const Clip* clip,
                   const unsigned int* tint);
std::vector<std::string> split_space2(const std::string& v);
int parse_shadow(const std::string& v, int& ox, int& oy, int& blur,
                 unsigned int& col);
int parse_shadow_any(const std::string& v, int& ox, int& oy, int& blur,
                     unsigned int& col);
void paint_shadow(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
                  int off_y, const Clip* clip);
SDL_Surface* img_surface(whaleui_render_t* r, const std::string& src);
void paint_img(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
               int off_y, const Clip* clip);
void compute_paint_bounds(whaleui_layout_node_t* n);
/* subtree + ancestor-chain bounds refresh (style-only relayout) */
void refresh_paint_bounds_chain(whaleui_layout_node_t* n);
bool paint_cull(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
                int off_y, const Clip* clip);
void paint_node(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
                int off_y, int& seq, int sel_lo, int sel_hi,
                const Clip* clip, bool tc);
/* z-order helpers (render_paint.cpp): an element with z-index>0 or
 * fixed/sticky position paints on its own layer and clears the old text
 * under its subtree before drawing (text_layer composites last). */
bool is_high_root(const whaleui_layout_node_t* n);
void text_layer_clear(whaleui_render_t* r, int x, int y, int w, int h);
void clear_subtree_text(whaleui_render_t* r, const whaleui_layout_node_t* n,
                        int off_x, int off_y);

/* --- text (render_text.cpp) --- */

std::vector<std::string> split_families(const std::string& s);
TTF_Font* render_get_font(whaleui_render_t* r, const std::string& family,
                          int size, int style);
size_t utf8_char_len(unsigned char c);
void apply_text_transform(std::string& s, const std::string& t);
/* wrap_w > 0: measure/hit against a TTF wrap width, so editing math
 * (caret/selection/click) matches what draw_text_at actually paints. 0 =
 * single-line layout (only \n breaks), the historic behavior. */
void text_size(whaleui_render_t* r, const std::string& text, int fs,
               const std::string& family, bool bold, int* tw, int* th,
               int wrap_w = 0);
int text_line_h(whaleui_render_t* r, int fs, const std::string& family,
                bool bold);
/* 字形视觉盒高(ascent+descent/2):文字/光标垂直居中的统一基准。 */
int text_glyph_h(whaleui_render_t* r, int fs, const std::string& family,
                 bool bold);
/* glyph advance via the paint path's fallback chain (.notdef-safe). */
int text_glyph_adv(whaleui_render_t* r, int fs, const std::string& family,
                   bool bold, unsigned int cp);
std::vector<TRect> sel_rects(whaleui_render_t* r, const std::string& text,
                             int fs, const std::string& family, bool bold,
                             size_t a, size_t b, int wrap_w = 0);
void caret_pos(whaleui_render_t* r, const std::string& text, int fs,
               const std::string& family, bool bold, size_t off,
               int* cx, int* cy, int* ch, int wrap_w = 0);
void draw_text_at(whaleui_render_t* r, const std::string& text,
                  int bx, int by, int bw, int bh,
                  int fs, const std::string& family, unsigned int color,
                  int style, int align, lxb_dom_element* ckey,
                  const Clip* clip, int lsp = 0, bool wrap = false,
                  float opacity = 1.0f, bool outline = false);
bool sel_range_for(whaleui_render_t* r, lxb_dom_element* el, size_t len,
                   size_t* a, size_t* b, int seq, int sel_lo, int sel_hi);
void sel_seq(whaleui_render_t* r, int* lo, int* hi, const Clip* clip);
void expand_hl_rects(std::vector<TRect>& rects, int lh);
unsigned int sel_hl_color(whaleui_render_t* r, whaleui_layout_node_t* n,
                          unsigned int alpha);
void paint_text_selection(whaleui_render_t* r, whaleui_layout_node_t* n,
                          int fs, const std::string& family, bool bold,
                          int off_x, int off_y, int seq, int sel_lo,
                          int sel_hi, const Clip* clip);
void paint_caret(whaleui_render_t* r, int tx, int ty, const std::string& text,
                 int fs, const std::string& family, bool bold,
                 size_t off, const Clip* clip, int wrap_w = 0);
void update_ime_area(whaleui_render_t* r, const std::string& val, int fs,
                     const std::string& family, bool bold, size_t caret,
                     int tx, int ty, int wrap_w = 0);
void paint_text(whaleui_render_t* r, whaleui_layout_node_t* n, int off_x,
                int off_y, int seq, int sel_lo, int sel_hi, const Clip* clip);
void text_origin(whaleui_render_t* r, whaleui_layout_node_t* n,
                 const std::string& text, int fs, const std::string& family,
                 bool bold, int* tx, int* ty, int wrap_w = 0);
size_t byte_at_text(whaleui_render_t* r, const std::string& text, int fs,
                    const std::string& family, bool bold, int px, int py,
                    int wrap_w = 0);
/* wrap width a text run paints at (draw_text_at's avail_w): the parent
 * content right edge minus the run's laid-out x; 0 when the run never
 * wraps (white-space: nowrap) */
int run_wrap_w(whaleui_layout_node_t* n);

/* --- controls (render_control.cpp) --- */

void paint_checkbox(whaleui_render_t* r, whaleui_layout_node_t* n,
                    int off_x, int off_y, const Clip* clip);
void paint_progress(whaleui_render_t* r, whaleui_layout_node_t* n,
                    int off_x, int off_y, const Clip* clip);
void paint_editable(whaleui_render_t* r, whaleui_layout_node_t* n,
                    int off_x, int off_y, const Clip* clip);
void paint_scrollbar(whaleui_render_t* r, whaleui_layout_node_t* n,
                     int off_x, int off_y, const Clip* clip);
void select_options(lxb_dom_element* sel, std::vector<std::string>& texts,
                    std::vector<std::string>& values);
bool is_select_node(whaleui_layout_node_t* n);
void paint_select_value(whaleui_render_t* r, whaleui_layout_node_t* n,
                        int off_x, int off_y, const Clip* clip);
void paint_select_list(whaleui_render_t* r, whaleui_layout_node_t* n,
                       int off_y, const Clip* clip);

/* --- interaction helpers (defined in render.cpp) --- */

std::string sget(const WhaleUIComputedStyle& s, const char* k);
bool tag_eq(lxb_dom_element* el, const char* tag);
bool is_check_radio(lxb_dom_element* el);
bool is_editable(lxb_dom_element* el);
bool is_editable_node(whaleui_layout_node_t* n);
std::string edit_value(lxb_dom_element* el);
void edit_set_value(lxb_dom_element* el, const std::string& s);
void edit_replace(whaleui_render_t* r, lxb_dom_element* el, size_t a, size_t b,
                  const std::string& insertion);
size_t utf8_prev(const std::string& s, size_t b);
size_t utf8_next(const std::string& s, size_t b);
int scroll_delta(whaleui_render_t* r, whaleui_layout_node_t* n);
/* font-size -> px (clamp()/min()/max()/vw/vh supported, matching the
 * layout pass). 0 when unparseable. */
int font_size_px(whaleui_render_t* r, const std::string& v);

void node_font(whaleui_render_t* r, whaleui_layout_node_t* n, int* fs,
               std::string* family, bool* bold);
whaleui_layout_node_t* editable_geo(whaleui_layout_node_t* n);

/* --- FSR upscale (render_fsr.cpp) --- */

int render_fsr_create(whaleui_render_t* r);
void render_fsr_destroy(whaleui_render_t* r);
int fsr_want_active(whaleui_render_t* r);

#endif /* WHALEUI_RENDER_INTERNAL_H */

