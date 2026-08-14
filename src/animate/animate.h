#ifndef WHALEUI_ANIMATE_ANIMATE_H
#define WHALEUI_ANIMATE_ANIMATE_H

/* Animation engine - internal interface.
 *
 * Implements CSS `transition` and `@keyframes`/`animation` on top of the
 * computed-style map. One whaleui_anim context lives per render context and
 * is fed every element's freshly computed style (in the layout pass, right
 * after whaleui_style_compute); it interpolates and writes the animated
 * values back into the style map, so layout (width/height/opacity) and paint
 * (colors) both see them with no extra plumbing.
 *
 * Two mechanisms:
 *  - `animation: name dur [timing] [delay] [iter] [forwards]`: plays the
 *    named @keyframes block, interpolating frame values between adjacent
 *    percentage frames. fill-mode forwards keeps the last frame after the
 *    run; infinite loops forever.
 *  - `transition: [props] dur [timing]`: when a property value changes vs
 *    the previous frame, interpolates from the old value to the new one.
 *    Colors and numeric lengths interpolate; anything else snaps.
 *
 * State is keyed by "prop@element-address" so concurrent per-element
 * animations don't collide. The clock is passed in (whaleui_anim_now wraps
 * SDL_GetTicks) so unit tests can drive time deterministically. */

#include "style/style.h"
#include "style/css.h"

#include <cstdint>
#include <map>
#include <string>

/* lexbor element type */
struct lxb_dom_element;

#ifdef __cplusplus
extern "C" {
#endif

struct whaleui_anim;
typedef struct whaleui_anim whaleui_anim_t;

whaleui_anim_t* whaleui_anim_create(void);
void whaleui_anim_destroy(whaleui_anim_t* a);

/* Attach the @keyframes registry (borrowed; stable for the render context's
 * lifetime). Call again after the stylesheet changes. */
void whaleui_anim_set_keyframes(whaleui_anim_t* a,
                                const whaleui_css_keyframes_t* keyframes);

/* Drop all animation state (new document / stylesheet swap). */
void whaleui_anim_reset(whaleui_anim_t* a);

/* Evaluate keyframes + transition for one element's computed style, writing
 * animated values back into `style`. Called by the layout pass; advances the
 * animation state. Returns 1 while any animation is active. */
int whaleui_anim_apply(whaleui_anim_t* a, struct lxb_dom_element* el,
                       WhaleUIComputedStyle& style, uint64_t now_ms);

/* Advance all running animations without touching any layout styles and
 * record the current interpolated values (whaleui_anim_apply_ov reads them
 * back). The frame loop calls this every frame; a paint-only animation (no
 * layout-affecting property active) can then repaint without relaying out. */
int whaleui_anim_tick(whaleui_anim_t* a, uint64_t now_ms);

/* 1 if the last tick/apply left an animation running. */
int whaleui_anim_active(const whaleui_anim_t* a);

/* 1 if the running animations touch any layout-affecting property (width,
 * margin, font-size, ...) and the layout tree must be rebuilt every frame;
 * 0 means paint-only (opacity/transform/colors) - repaint is enough. */
int whaleui_anim_needs_layout(const whaleui_anim_t* a);

/* 1 if `el` has any running animation (for the paint fast path). */
int whaleui_anim_has_el(const whaleui_anim_t* a, struct lxb_dom_element* el);

/* Overwrite `style` with the current animated values for `el` (recorded by
 * whaleui_anim_tick). Used by the repaint-only fast path. */
void whaleui_anim_apply_ov(const whaleui_anim_t* a, struct lxb_dom_element* el,
                           WhaleUIComputedStyle& style);

/* Milliseconds clock (SDL_GetTicks). */
uint64_t whaleui_anim_now(void);

/* Evaluated transform: translation in pixels + uniform/axis scale.
 * scale applies around the element's center (default transform-origin). */
struct whaleui_transform
{
    float tx, ty; /* translate result, px */
    float sx, sy; /* scale result */
};
typedef struct whaleui_transform whaleui_transform_t;

/* Resolve a `transform` value ("none", "translateY(16px)",
 * "scale(1.09) translate(1.5%,-1%)") for an element of self_w x self_h.
 * Percentages are relative to the element's own size. Returns 0 on success;
 * unsupported functions return -1 and leave out untouched. */
int whaleui_transform_eval(const char* value, float self_w, float self_h,
                           whaleui_transform_t* out);

#ifdef __cplusplus
}
#endif

#endif /* WHALEUI_ANIMATE_ANIMATE_H */
