/* Canvas 2D: public C API implementation (CPU backing store).
 *
 * Browser mapping: HTMLCanvasElement (width/height/getContext) +
 * CanvasRenderingContext2D subset (fillRect/strokeRect/clearRect, path
 * fill/stroke, save/restore/transform, ImageData get/put). Backing store is
 * CPU 0xAARRGGBB; the render paint path blits it like an <img> bitmap.
 * Text (fillText/measureText) and drawImage are follow-ups (need the font
 * raster hook); they return failure cleanly for now. */

#include "canvas/canvas.h"
#include "dom/dom.h"

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::map<lxb_dom_element*, whaleui_canvas_context_t*>& registry()
{
    static std::map<lxb_dom_element*, whaleui_canvas_context_t*> r;
    return r;
}

lxb_dom_element* as_el(whaleui_dom_element_t* el)
{
    return reinterpret_cast<lxb_dom_element*>(el);
}

/* parse "#RRGGBB" / "#RGB" / "rgb(r,g,b)" / named subset -> 0xAARRGGBB */
unsigned int parse_color(const char* s, unsigned int fallback)
{
    if (!s || !*s) return fallback;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '#') {
        ++s;
        size_t n = std::strlen(s);
        unsigned int v = 0;
        if (n == 6 && std::sscanf(s, "%x", &v) == 1)
            return 0xFF000000u | v;
        if (n == 3) {
            unsigned int r = 0, g = 0, b = 0;
            std::sscanf(s, "%1x%1x%1x", &r, &g, &b);
            return 0xFF000000u | (r * 0x11 << 16) | (g * 0x11 << 8) | (b * 0x11);
        }
        if (n == 8 && std::sscanf(s, "%x", &v) == 1) {
            /* #AARRGGBB */
            return v;
        }
        return fallback;
    }
    if (std::strncmp(s, "rgb(", 4) == 0) {
        int r = 0, g = 0, b = 0;
        if (std::sscanf(s + 4, "%d,%d,%d", &r, &g, &b) == 3)
            return 0xFF000000u | ((r & 255) << 16) | ((g & 255) << 8) | (b & 255);
        return fallback;
    }
    static const struct { const char* n; unsigned int c; } kNamed[] = {
        {"black", 0xFF000000u}, {"white", 0xFFFFFFFFu}, {"red", 0xFFFF0000u},
        {"green", 0xFF008000u}, {"lime", 0xFF00FF00u}, {"blue", 0xFF0000FFu},
        {"yellow", 0xFFFFFF00u}, {"cyan", 0xFF00FFFFu}, {"magenta", 0xFFFF00FFu},
        {"gray", 0xFF808080u}, {"grey", 0xFF808080u}, {"transparent", 0x00000000u},
        {nullptr, 0},
    };
    for (int i = 0; kNamed[i].n; ++i) {
        if (std::strcmp(s, kNamed[i].n) == 0) return kNamed[i].c;
    }
    return fallback;
}

int canvas_attr_int(lxb_dom_element* e, const char* name, int fallback)
{
    size_t len = 0;
    const lxb_char_t* v = lxb_dom_element_get_attribute(
        e, reinterpret_cast<const lxb_char_t*>(name), std::strlen(name), &len);
    if (!v || !len) return fallback;
    int out = std::atoi(std::string(reinterpret_cast<const char*>(v), len).c_str());
    return out > 0 ? out : fallback;
}

void canvas_apply_transform(whaleui_canvas_context_t* c, float x, float y, float* ox, float* oy)
{
    *ox = c->m_a * x + c->m_c * y + c->m_e;
    *oy = c->m_b * x + c->m_d * y + c->m_f;
}

void blend_px(unsigned int* dst, unsigned int src, float alpha)
{
    unsigned int sa = (unsigned int)(((src >> 24) & 255) * alpha + 0.5f);
    if (sa == 0) return;
    unsigned int sr = (src >> 16) & 255, sg = (src >> 8) & 255, sb = src & 255;
    unsigned int d = *dst;
    unsigned int da = (d >> 24) & 255, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    unsigned int oa = sa + da * (255 - sa) / 255;
    unsigned int orr = (sr * sa + dr * da * (255 - sa) / 255) / (oa ? oa : 1);
    unsigned int og = (sg * sa + dg * da * (255 - sa) / 255) / (oa ? oa : 1);
    unsigned int ob = (sb * sa + db * da * (255 - sa) / 255) / (oa ? oa : 1);
    *dst = (oa << 24) | (orr << 16) | (og << 8) | ob;
}

void fill_rect_px(whaleui_canvas_context_t* c, int x, int y, int w, int h, unsigned int color)
{
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w;
    int y1 = y + h > c->h ? c->h : y + h;
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i)
            blend_px(&c->px[(size_t)j * c->w + i], color, c->global_alpha);
}

void stroke_rect_px(whaleui_canvas_context_t* c, int x, int y, int w, int h, unsigned int color)
{
    int lw = (int)(c->line_width + 0.5f);
    if (lw < 1) lw = 1;
    fill_rect_px(c, x, y, w, lw, color);
    fill_rect_px(c, x, y + h - lw, w, lw, color);
    fill_rect_px(c, x, y, lw, h, color);
    fill_rect_px(c, x + w - lw, y, lw, h, color);
}

/* scanline fill of the current path bbox via even-odd on segment list.
 * Path ops: 0=move, 1=line, 2=close, 3=rect, 4=arc(circle). Only line/rect/arc
 * contribute edges; curves are not supported yet. */
void fill_path(whaleui_canvas_context_t* c, unsigned int color)
{
    /* collect polygon points + circles + rects */
    std::vector<std::pair<float,float> > poly;
    struct Circle { float x, y, r; };
    std::vector<Circle> circles;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool open = false;
    for (size_t i = 0; i < c->path.size(); ++i) {
        const whaleui_canvas_context::PathOp& op = c->path[i];
        if (op.kind == 0) { cx = op.x0; cy = op.y0; sx = cx; sy = cy; open = true; }
        else if (op.kind == 1 && open) { poly.push_back(std::make_pair(cx, cy)); poly.push_back(std::make_pair(op.x0, op.y0)); cx = op.x0; cy = op.y0; }
        else if (op.kind == 2 && open) { poly.push_back(std::make_pair(cx, cy)); poly.push_back(std::make_pair(sx, sy)); open = false; }
        else if (op.kind == 3) {
            float x0 = op.x0, y0 = op.y0, x1 = op.x1, y1 = op.y1;
            poly.push_back(std::make_pair(x0, y0)); poly.push_back(std::make_pair(x1, y0));
            poly.push_back(std::make_pair(x1, y0)); poly.push_back(std::make_pair(x1, y1));
            poly.push_back(std::make_pair(x1, y1)); poly.push_back(std::make_pair(x0, y1));
            poly.push_back(std::make_pair(x0, y1)); poly.push_back(std::make_pair(x0, y0));
        }
        else if (op.kind == 4) { Circle cc; cc.x = op.x0; cc.y = op.y0; cc.r = op.x1; circles.push_back(cc); }
    }
    if (poly.empty() && circles.empty()) return;
    /* bounds */
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (size_t i = 0; i < poly.size(); ++i) {
        float tx, ty;
        canvas_apply_transform(c, poly[i].first, poly[i].second, &tx, &ty);
        if (tx < bx0) bx0 = tx; if (tx > bx1) bx1 = tx;
        if (ty < by0) by0 = ty; if (ty > by1) by1 = ty;
    }
    for (size_t i = 0; i < circles.size(); ++i) {
        float tx, ty;
        canvas_apply_transform(c, circles[i].x, circles[i].y, &tx, &ty);
        float r = circles[i].r * (c->m_a > c->m_d ? c->m_a : c->m_d);
        if (tx - r < bx0) bx0 = tx - r; if (tx + r > bx1) bx1 = tx + r;
        if (ty - r < by0) by0 = ty - r; if (ty + r > by1) by1 = ty + r;
    }
    int ix0 = (int)bx0 < 0 ? 0 : (int)bx0;
    int iy0 = (int)by0 < 0 ? 0 : (int)by0;
    int ix1 = (int)(bx1 + 1.0f) > c->w ? c->w : (int)(bx1 + 1.0f);
    int iy1 = (int)(by1 + 1.0f) > c->h ? c->h : (int)(by1 + 1.0f);
    /* transform polygon once */
    std::vector<std::pair<float,float> > tp(poly.size());
    for (size_t i = 0; i < poly.size(); ++i)
        canvas_apply_transform(c, poly[i].first, poly[i].second, &tp[i].first, &tp[i].second);
    for (int j = iy0; j < iy1; ++j) {
        for (int i = ix0; i < ix1; ++i) {
            float px = i + 0.5f, py = j + 0.5f;
            bool inside = false;
            for (size_t k = 0; k + 1 < tp.size(); k += 2) {
                float ax = tp[k].first, ay = tp[k].second;
                float bx = tp[k+1].first, by = tp[k+1].second;
                /* even-odd ray cast per segment */
                if ((ay > py) != (by > py)) {
                    float xi = ax + (py - ay) * (bx - ax) / (by - ay);
                    if (px < xi) inside = !inside;
                }
            }
            if (!inside) {
                for (size_t k = 0; k < circles.size() && !inside; ++k) {
                    float tx, ty;
                    canvas_apply_transform(c, circles[k].x, circles[k].y, &tx, &ty);
                    float r = circles[k].r * (c->m_a > c->m_d ? c->m_a : c->m_d);
                    float dx = px - tx, dy = py - ty;
                    if (dx * dx + dy * dy <= r * r) inside = true;
                }
            }
            if (inside) blend_px(&c->px[(size_t)j * c->w + i], color, c->global_alpha);
        }
    }
}

void stroke_path(whaleui_canvas_context_t* c, unsigned int color)
{
    int lw = (int)(c->line_width + 0.5f);
    if (lw < 1) lw = 1;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool open = false;
    for (size_t i = 0; i < c->path.size(); ++i) {
        const whaleui_canvas_context::PathOp& op = c->path[i];
        if (op.kind == 0) { cx = op.x0; cy = op.y0; sx = cx; sy = cy; open = true; }
        else if (op.kind == 1 && open) {
            float ax, ay, bx, by;
            canvas_apply_transform(c, cx, cy, &ax, &ay);
            canvas_apply_transform(c, op.x0, op.y0, &bx, &by);
            /* thick line: stamp discs along the segment */
            float dx = bx - ax, dy = by - ay;
            float len = std::sqrt(dx * dx + dy * dy);
            int steps = (int)len + 1;
            for (int s = 0; s <= steps; ++s) {
                float t = steps ? (float)s / steps : 0;
                int x = (int)(ax + dx * t), y = (int)(ay + dy * t);
                fill_rect_px(c, x - lw / 2, y - lw / 2, lw, lw, color);
            }
            cx = op.x0; cy = op.y0;
        }
        else if (op.kind == 2 && open) {
            whaleui_canvas_context::PathOp ln; ln.kind = 1; ln.x0 = sx; ln.y0 = sy;
            float ax, ay, bx, by;
            canvas_apply_transform(c, cx, cy, &ax, &ay);
            canvas_apply_transform(c, sx, sy, &bx, &by);
            float dx = bx - ax, dy = by - ay;
            float len = std::sqrt(dx * dx + dy * dy);
            int steps = (int)len + 1;
            for (int s = 0; s <= steps; ++s) {
                float t = steps ? (float)s / steps : 0;
                int x = (int)(ax + dx * t), y = (int)(ay + dy * t);
                fill_rect_px(c, x - lw / 2, y - lw / 2, lw, lw, color);
            }
            open = false;
        }
        else if (op.kind == 3) {
            float ax, ay;
            canvas_apply_transform(c, op.x0, op.y0, &ax, &ay);
            float bx, by;
            canvas_apply_transform(c, op.x1, op.y1, &bx, &by);
            int x = (int)(ax < bx ? ax : bx), y = (int)(ay < by ? ay : by);
            int w = (int)(ax < bx ? bx - ax : ax - bx), h = (int)(ay < by ? by - ay : ay - by);
            stroke_rect_px(c, x, y, w, h, color);
        }
        else if (op.kind == 4) {
            float tx, ty;
            canvas_apply_transform(c, op.x0, op.y0, &tx, &ty);
            float r = op.x1 * (c->m_a > c->m_d ? c->m_a : c->m_d);
            int segs = (int)(r * 6) + 12;
            float px = tx + r, py = ty;
            for (int s = 1; s <= segs; ++s) {
                float a = 6.2831853f * s / segs;
                int x = (int)(tx + r * std::cos(a)), y = (int)(ty + r * std::sin(a));
                float dx = (float)(x - (int)px), dy = (float)(y - (int)py);
                float len = std::sqrt(dx * dx + dy * dy);
                int steps = (int)len + 1;
                for (int k = 0; k <= steps; ++k) {
                    float t = steps ? (float)k / steps : 0;
                    fill_rect_px(c, (int)(px + dx * t) - lw / 2, (int)(py + dy * t) - lw / 2, lw, lw, color);
                }
                px = x; py = y;
            }
        }
    }
}

} // namespace

extern "C" whaleui_canvas_context_t* whaleui_canvas_context_for(struct lxb_dom_element* el)
{
    if (!el) return nullptr;
    std::map<lxb_dom_element*, whaleui_canvas_context_t*>::iterator it = registry().find(el);
    if (it != registry().end()) return it->second;
    whaleui_canvas_context_t* c = new whaleui_canvas_context_t;
    c->el = el;
    c->w = canvas_attr_int(el, "width", 300);
    c->h = canvas_attr_int(el, "height", 150);
    c->px.assign((size_t)c->w * c->h, 0x00000000u);
    c->fill_style = 0xFF000000u;
    c->stroke_style = 0xFF000000u;
    c->line_width = 1.0f;
    c->global_alpha = 1.0f;
    c->font = "10px sans-serif";
    c->text_align = "start";
    c->text_baseline = "alphabetic";
    c->m_a = 1; c->m_b = 0; c->m_c = 0; c->m_d = 1; c->m_e = 0; c->m_f = 0;
    c->seq = 1;
    registry()[el] = c;
    return c;
}

extern "C" void whaleui_canvas_drop(struct lxb_dom_element* el)
{
    std::map<lxb_dom_element*, whaleui_canvas_context_t*>::iterator it = registry().find(el);
    if (it != registry().end()) { delete it->second; registry().erase(it); }
}

extern "C" void whaleui_canvas_drop_all(void)
{
    for (std::map<lxb_dom_element*, whaleui_canvas_context_t*>::iterator it = registry().begin();
         it != registry().end(); ++it) delete it->second;
    registry().clear();
}

/* ======================= public C API ======================= */

extern "C" whaleui_canvas_context_t* whaleui_canvas_get_context(whaleui_dom_element_t* el, const char* type)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !type) return nullptr;
    if (std::strcmp(type, "2d") != 0) return nullptr;
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(e, &len);
    if (!name || len != 6 || std::memcmp(name, "canvas", 6) != 0) return nullptr;
    return whaleui_canvas_context_for(e);
}

extern "C" int whaleui_canvas_set_size(whaleui_dom_element_t* el, int w, int h)
{
    lxb_dom_element* e = as_el(el);
    if (!e || w <= 0 || h <= 0) return -1;
    char tmp[32];
    std::snprintf(tmp, sizeof(tmp), "%d", w);
    lxb_dom_element_set_attribute(e, (const lxb_char_t*)"width", 5,
        reinterpret_cast<const lxb_char_t*>(tmp), std::strlen(tmp));
    std::snprintf(tmp, sizeof(tmp), "%d", h);
    lxb_dom_element_set_attribute(e, (const lxb_char_t*)"height", 6,
        reinterpret_cast<const lxb_char_t*>(tmp), std::strlen(tmp));
    whaleui_canvas_context_t* c = whaleui_canvas_context_for(e);
    c->w = w; c->h = h;
    c->px.assign((size_t)w * h, 0x00000000u);
    c->seq++;
    whaleui_dom_mark_dirty_struct(e);
    return 0;
}

extern "C" int whaleui_canvas_get_size(whaleui_dom_element_t* el, int* w, int* h)
{
    lxb_dom_element* e = as_el(el);
    if (!e || !w || !h) return -1;
    *w = canvas_attr_int(e, "width", 300);
    *h = canvas_attr_int(e, "height", 150);
    return 0;
}

extern "C" int whaleui_canvas_set_fill_style(whaleui_canvas_context_t* ctx, const char* style)
{
    if (!ctx || !style) return -1;
    ctx->fill_style = parse_color(style, ctx->fill_style);
    return 0;
}

extern "C" int whaleui_canvas_set_stroke_style(whaleui_canvas_context_t* ctx, const char* style)
{
    if (!ctx || !style) return -1;
    ctx->stroke_style = parse_color(style, ctx->stroke_style);
    return 0;
}

extern "C" int whaleui_canvas_set_line_width(whaleui_canvas_context_t* ctx, float w)
{
    if (!ctx || w <= 0) return -1;
    ctx->line_width = w;
    return 0;
}

extern "C" int whaleui_canvas_set_global_alpha(whaleui_canvas_context_t* ctx, float a)
{
    if (!ctx || a < 0.0f || a > 1.0f) return -1;
    ctx->global_alpha = a;
    return 0;
}

extern "C" int whaleui_canvas_fill_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h)
{
    if (!ctx) return -1;
    float ax, ay, bx, by;
    canvas_apply_transform(ctx, x, y, &ax, &ay);
    canvas_apply_transform(ctx, x + w, y + h, &bx, &by);
    int ix = (int)(ax < bx ? ax : bx), iy = (int)(ay < by ? ay : by);
    int iw = (int)(ax < bx ? bx - ax : ax - bx), ih = (int)(ay < by ? by - ay : ay - by);
    fill_rect_px(ctx, ix, iy, iw, ih, ctx->fill_style);
    ctx->seq++;
    whaleui_dom_mark_dirty(ctx->el);
    return 0;
}

extern "C" int whaleui_canvas_clear_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h)
{
    if (!ctx) return -1;
    int ix = (int)x, iy = (int)y, iw = (int)w, ih = (int)h;
    int x0 = ix < 0 ? 0 : ix, y0 = iy < 0 ? 0 : iy;
    int x1 = ix + iw > ctx->w ? ctx->w : ix + iw;
    int y1 = iy + ih > ctx->h ? ctx->h : iy + ih;
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i)
            ctx->px[(size_t)j * ctx->w + i] = 0x00000000u;
    ctx->seq++;
    whaleui_dom_mark_dirty(ctx->el);
    return 0;
}

extern "C" int whaleui_canvas_stroke_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h)
{
    if (!ctx) return -1;
    float ax, ay, bx, by;
    canvas_apply_transform(ctx, x, y, &ax, &ay);
    canvas_apply_transform(ctx, x + w, y + h, &bx, &by);
    int ix = (int)(ax < bx ? ax : bx), iy = (int)(ay < by ? ay : by);
    int iw = (int)(ax < bx ? bx - ax : ax - bx), ih = (int)(ay < by ? by - ay : ay - by);
    stroke_rect_px(ctx, ix, iy, iw, ih, ctx->stroke_style);
    ctx->seq++;
    whaleui_dom_mark_dirty(ctx->el);
    return 0;
}

extern "C" int whaleui_canvas_begin_path(whaleui_canvas_context_t* ctx)
{
    if (!ctx) return -1;
    ctx->path.clear();
    return 0;
}

extern "C" int whaleui_canvas_move_to(whaleui_canvas_context_t* ctx, float x, float y)
{
    if (!ctx) return -1;
    whaleui_canvas_context::PathOp op; op.kind = 0; op.x0 = x; op.y0 = y;
    ctx->path.push_back(op);
    return 0;
}

extern "C" int whaleui_canvas_line_to(whaleui_canvas_context_t* ctx, float x, float y)
{
    if (!ctx) return -1;
    whaleui_canvas_context::PathOp op; op.kind = 1; op.x0 = x; op.y0 = y;
    ctx->path.push_back(op);
    return 0;
}

extern "C" int whaleui_canvas_close_path(whaleui_canvas_context_t* ctx)
{
    if (!ctx) return -1;
    whaleui_canvas_context::PathOp op; op.kind = 2;
    ctx->path.push_back(op);
    return 0;
}

extern "C" int whaleui_canvas_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h)
{
    if (!ctx) return -1;
    whaleui_canvas_context::PathOp op; op.kind = 3; op.x0 = x; op.y0 = y; op.x1 = x + w; op.y1 = y + h;
    ctx->path.push_back(op);
    return 0;
}

extern "C" int whaleui_canvas_arc(whaleui_canvas_context_t* ctx, float x, float y, float r, float a0, float a1)
{
    if (!ctx || r <= 0) return -1;
    (void)a0; (void)a1; /* full circle for now; arc range is a follow-up */
    whaleui_canvas_context::PathOp op; op.kind = 4; op.x0 = x; op.y0 = y; op.x1 = r;
    ctx->path.push_back(op);
    return 0;
}

extern "C" int whaleui_canvas_fill(whaleui_canvas_context_t* ctx)
{
    if (!ctx) return -1;
    fill_path(ctx, ctx->fill_style);
    ctx->seq++;
    whaleui_dom_mark_dirty(ctx->el);
    return 0;
}

extern "C" int whaleui_canvas_stroke(whaleui_canvas_context_t* ctx)
{
    if (!ctx) return -1;
    stroke_path(ctx, ctx->stroke_style);
    ctx->seq++;
    whaleui_dom_mark_dirty(ctx->el);
    return 0;
}

extern "C" int whaleui_canvas_save(whaleui_canvas_context_t* ctx)
{
    if (!ctx) return -1;
    whaleui_canvas_context::SavedState s;
    s.fill_style = ctx->fill_style; s.stroke_style = ctx->stroke_style;
    s.line_width = ctx->line_width; s.global_alpha = ctx->global_alpha;
    s.m_a = ctx->m_a; s.m_b = ctx->m_b; s.m_c = ctx->m_c;
    s.m_d = ctx->m_d; s.m_e = ctx->m_e; s.m_f = ctx->m_f;
    ctx->stack.push_back(s);
    return 0;
}

extern "C" int whaleui_canvas_restore(whaleui_canvas_context_t* ctx)
{
    if (!ctx || ctx->stack.empty()) return -1;
    whaleui_canvas_context::SavedState s = ctx->stack.back();
    ctx->stack.pop_back();
    ctx->fill_style = s.fill_style; ctx->stroke_style = s.stroke_style;
    ctx->line_width = s.line_width; ctx->global_alpha = s.global_alpha;
    ctx->m_a = s.m_a; ctx->m_b = s.m_b; ctx->m_c = s.m_c;
    ctx->m_d = s.m_d; ctx->m_e = s.m_e; ctx->m_f = s.m_f;
    return 0;
}

extern "C" int whaleui_canvas_set_transform(whaleui_canvas_context_t* ctx, float a, float b, float c, float d, float e, float f)
{
    if (!ctx) return -1;
    ctx->m_a = a; ctx->m_b = b; ctx->m_c = c; ctx->m_d = d; ctx->m_e = e; ctx->m_f = f;
    return 0;
}

extern "C" int whaleui_canvas_transform(whaleui_canvas_context_t* ctx, float a, float b, float c, float d, float e, float f)
{
    if (!ctx) return -1;
    float na = ctx->m_a * a + ctx->m_c * b;
    float nb = ctx->m_b * a + ctx->m_d * b;
    float nc = ctx->m_a * c + ctx->m_c * d;
    float nd = ctx->m_b * c + ctx->m_d * d;
    float ne = ctx->m_a * e + ctx->m_c * f + ctx->m_e;
    float nf = ctx->m_b * e + ctx->m_d * f + ctx->m_f;
    ctx->m_a = na; ctx->m_b = nb; ctx->m_c = nc; ctx->m_d = nd; ctx->m_e = ne; ctx->m_f = nf;
    return 0;
}

extern "C" int whaleui_canvas_translate(whaleui_canvas_context_t* ctx, float x, float y)
{
    return whaleui_canvas_transform(ctx, 1, 0, 0, 1, x, y);
}

extern "C" int whaleui_canvas_scale(whaleui_canvas_context_t* ctx, float x, float y)
{
    return whaleui_canvas_transform(ctx, x, 0, 0, y, 0, 0);
}

extern "C" int whaleui_canvas_rotate(whaleui_canvas_context_t* ctx, float angle)
{
    float s = std::sin(angle), co = std::cos(angle);
    return whaleui_canvas_transform(ctx, co, s, -s, co, 0, 0);
}

extern "C" whaleui_image_data_t* whaleui_canvas_create_image_data(whaleui_canvas_context_t* ctx, int w, int h)
{
    (void)ctx;
    if (w <= 0 || h <= 0) return nullptr;
    whaleui_image_data_t* d = new whaleui_image_data_t;
    d->w = w; d->h = h;
    d->px.assign((size_t)w * h, 0x00000000u);
    return d;
}

extern "C" void whaleui_image_data_destroy(whaleui_image_data_t* d)
{
    delete d;
}

extern "C" int whaleui_image_data_size(const whaleui_image_data_t* d, int* w, int* h)
{
    if (!d || !w || !h) return -1;
    *w = d->w; *h = d->h;
    return 0;
}

extern "C" unsigned int whaleui_image_data_get(const whaleui_image_data_t* d, int x, int y)
{
    if (!d || x < 0 || y < 0 || x >= d->w || y >= d->h) return 0;
    return d->px[(size_t)y * d->w + x];
}

extern "C" int whaleui_image_data_set(whaleui_image_data_t* d, int x, int y, unsigned int argb)
{
    if (!d || x < 0 || y < 0 || x >= d->w || y >= d->h) return -1;
    d->px[(size_t)y * d->w + x] = argb;
    return 0;
}

extern "C" whaleui_image_data_t* whaleui_canvas_get_image_data(whaleui_canvas_context_t* ctx, int x, int y, int w, int h)
{
    if (!ctx || w <= 0 || h <= 0) return nullptr;
    whaleui_image_data_t* d = new whaleui_image_data_t;
    d->w = w; d->h = h;
    d->px.assign((size_t)w * h, 0x00000000u);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            int sx = x + i, sy = y + j;
            if (sx >= 0 && sy >= 0 && sx < ctx->w && sy < ctx->h)
                d->px[(size_t)j * w + i] = ctx->px[(size_t)sy * ctx->w + sx];
        }
    return d;
}

extern "C" int whaleui_canvas_put_image_data(whaleui_canvas_context_t* ctx, whaleui_image_data_t* d, int x, int y)
{
    if (!ctx || !d) return -1;
    for (int j = 0; j < d->h; ++j)
        for (int i = 0; i < d->w; ++i) {
            int dx = x + i, dy = y + j;
            if (dx >= 0 && dy >= 0 && dx < ctx->w && dy < ctx->h)
                ctx->px[(size_t)dy * ctx->w + dx] = d->px[(size_t)j * d->w + i];
        }
    ctx->seq++;
    whaleui_dom_mark_dirty(ctx->el);
    return 0;
}
