# Theme CSS gaps (backlog)

The theme stylesheets (`src/style/theme.cpp`) are translations of real,
verified design systems into this engine's CSS subset:

| theme   | source                                                                 |
|---------|------------------------------------------------------------------------|
| browser | hand-written UA default stylesheet (WHATWG HTML standard)              |
| fluent  | https://github.com/aipx-proto/fluent-css                               |
| metro   | Office UI Fabric Core 9.6.1 (Microsoft CDN, `ms-bgColor-*` palette)    |
| classic | hand-written Windows Classic translation                               |
| aero    | https://github.com/khang-nd/7.css                                      |
| material| https://github.com/zdhxiong/mdui (Material 3 tokens)                   |
| gtk     | https://github.com/mclellac/adwaita-web                                |
| macos   | https://github.com/connors/photon                                      |

Structure: a shared UA baseline (every tag gets a default display/typography,
WHATWG-style) + shared component classes (`.card`, `.btn-*`, `.tabs`,
`.toolbar`, `.pane`, `.anim-*` keyframes, opt-in per `class=`) + a per-theme
overlay for the recognizable control chrome (buttons/inputs/links/focus),
all driven by `var(--*)` from the light/dark variable tables. The renderer's
native controls (checkbox/radio/progress/scrollbar) read
`--accent`/`--field`/`--border`/`--card`; the select dropdown popup is drawn
from the virtual `.select-popup`/`.select-option[-hover|-selected]` classes
(`whaleui_style_virtual`), not hard-coded colors.

Reference source archives used during the translation are kept out of git
(`temp/ref/`, gitignored).

Status legend: `[x]` implemented, `[ ]` not yet.

## Engine features the stylesheets assume (marked `needs:` in theme.cpp)

The CSS is written for the *target* engine; rules that need engine work are
kept (commented with `needs:`) so nothing is lost:

- [ ] `pseudo-box` - `::before`/`::after` painted as real boxes
  (position/width/height/inset/background/border-radius + their own
  transition/animation). Today a pseudo-element is only a text run (its
  `content` becomes a run; paint merges text properties). The Aero button
  hover/active gradient overlays, the Fluent/M3 state layers and the input
  focus underline all depend on this.
- [ ] `multi-shadow` - `box-shadow` with more than one shadow / spread.
  Today one shadow (`ox oy blur color`, `inset` ok). Theme shadows like
  `--shadow-lg` carry two layers; the second is dropped.
- [ ] `grad-trans` - transitions between two `background` gradients (the
  reference systems cross-fade hover gradients; today a gradient change
  snaps).
- [ ] `:checked` / `:indeterminate` - pseudo-classes on checkbox/radio
  (switch component, selected tabs without JS class toggling).
- [ ] `outline` / `outline-offset` - focus ring property (focus rings are
  approximated with `box-shadow: 0 0 0 Npx` / inset).
- [ ] `4-side-bd` - per-side `border-color` (7.css inputs use four different
  edge colors; today border is a single color).
- [ ] `backdrop` - `backdrop-filter` (Aero glass window blur).
- [ ] `box-shadow` transition (hover shadow deepening snaps today; the
  transform translateY part does animate).
- [ ] `position: relative` layout bug - an inline-block element with
  `position: relative` (buttons, tabs) collapses to a vertical sliver in a
  flex/block flow. The stylesheets avoid `position: relative` on buttons
  for now (the overlay-layer pattern needs it once pseudo-box lands).
- [ ] `placeholder` attribute on input/textarea - an empty-value control
  shows nothing; placeholder text must be painted by the engine.
- [ ] `transform: rotate` (and other rotation functions) - `spin`-style
  keyframes have no effect; translate/scale work.
- [ ] `[type="..."]` attribute selectors - CSS cannot size style controls by
  input type (checkbox/radio rely on the UA default sizes).
- [ ] inline/flex children with an explicit `height` keep it - a
  checkbox/radio in a line box is sized 32px tall (line-height) instead of
  its 16px; in a flex row it can also shrink to ~11px wide (the UA default
  width is applied after the flex main-size pass). `paint_checkbox` now
  forces 16x16 as a stopgap.
- [ ] hover interaction in a real window - the render-layer hover
  background change works (test_scroll), but mouse-move -> hover state ->
  repaint does not fire in the app event loop (buttons/links/cards show no
  hover feedback).

## Fixed

- [x] `is_editable` accepted only `type="text"` - password/number/email
  values never painted. Now any non-special input type is editable.
- [x] app default accent `#0067c0` overrode every theme's default (material
  should be #6750a4). The app accent now starts empty; each theme's own
  default applies until the user sets one.
- [x] material capsule buttons: `border-radius: 999px` painted no
  background inside a flex container (engine capsule bug). CSS uses
  18px (= min-height/2) for now.
- [x] checkbox/radio paint box: layout 11x32 sliver -> forced 16x16
  (position still from the layout box).

## Fixed in this round

- [x] `::before/::after` selectors no longer leak into the element's own
  style: a rule like `input::after { position:absolute; height:2px }` used
  to apply position/height to every `input` (the `::` suffix was skipped as
  an unknown pseudo-class) and broke layout. `parse_simple` now marks
  `pseudo_el` and the element match fails; pseudo rules still surface for
  paint via `whaleui_style_match_pseudo`. Regression test in test_style.
- [x] select dropdown popup is CSS-driven: `paint_select_list` computes the
  virtual `.select-popup` / `.select-option-hover` / `.select-option-
  selected` classes (`whaleui_style_virtual` in style.cpp) and paints
  background/border/radius/first-shadow/row colors from them. The theme
  stylesheets now style the popup like any other surface.
- [x] `content: ""` pseudo rules produce no layout run (they did insert an
  empty run before the parse fix; verified `input::after{content:""}` alone
  is inert).

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
  `render.cpp`), plus the `inset` variant. GPU: soft shadow = white rounded
  rect mipmap-blurred; inset = the box interior split along the diagonals
  into four alpha-gradient triangles (edges 1 -> center 0) mipmap-blurred
  and blended over the painted background. CPU fallback: concentric
  (inward) rounded rects with fading alpha. Used by `.card` (`--shadow` per
  theme) and the select popup.
  Not yet: multiple shadows, spread.

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
