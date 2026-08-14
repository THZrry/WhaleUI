# Theme CSS gaps (backlog)

The theme stylesheets (`src/style/theme.cpp`) follow real design systems:
fluent-css, MetroUI, mdui (Material 3 tokens), and the GNOME Adwaita theme.
Reference sources are documented inline in `fill_vars()`.

Status legend: `[x]` implemented, `[ ]` not yet.

## Pseudo-classes

Matched via `whaleui_style_state` (hover/focus/pressed, `style.h`). The
window tracks the mouse position (`set_hover`), the last clicked control
(`focus_el`), and the element with the left button held (`pressed_el`,
`set_pressed` in `app.cpp`).

- [x] `:hover` - element under the mouse
- [x] `:active` - element with the button held (pressed button state:
  `--btn-bg-active`, darkened accent)
- [x] `:focus` / `:focus-visible` - last clicked control (focused input
  border: `input:focus { border-color: var(--accent) }`)
- [x] `:disabled` - element with the `disabled` attribute

Note: focus is set on mouse-down only (no Tab traversal yet); `:focus` and
`:focus-visible` are treated identically.

## Box shadow

- [x] `box-shadow` - single shadow `ox oy blur color` (`parse_shadow` in
  `render.cpp`). Soft shadow via concentric rounded rects with fading alpha.
  Used by `.card` (`--shadow` per theme) and the select popup.
  Not yet: multiple shadows, spread, inset.

## Transition

- [x] `transition` on color properties (`background-color`, `border-color`).
  Engine (`anim_color` / `ColorAnim` in `render.cpp`) interpolates RGBA over
  the declared duration on a per-frame clock and keeps repainting while an
  animation runs. Declared on buttons and inputs in `base_css`.
  Not yet: non-color properties, easing functions other than linear.

## Animation (`@keyframes`)

- [ ] `animation` - `@keyframes` blocks are parsed and stored
  (`css.cpp`/`render_set_css`) but never driven: the render loop has no
  keyframe clock. Needs the same per-frame advance as transitions.

## Other

- [ ] `outline` - focus ring (see `:focus-visible` above).
- [ ] `font-weight: 500` - medium weight renders as regular text; `paint_text`
  only treats `>=600` (or `bold`/`bolder`) as bold. M3 title weights use 500.
- [ ] `cursor` - parsed by the stylesheet but not consumed: `cursor: pointer`
  (buttons, `select`, `summary`) and `cursor: not-allowed` (disabled) have no
  effect on the host cursor.
- [x] `rgba()` float alpha (e.g. `rgba(0,0,0,0.12)` for shadows) and the
  legacy 0-255 integer form.

## Already supported (used by the themes, no work needed)

- `border-radius` (including capsule via large px values, clipped to half)
- `:hover` / `:active` / `:focus` background and border-color changes
- `font-weight` 600/700 (bold) and `font-family` fallback
- `opacity` (element-level)
