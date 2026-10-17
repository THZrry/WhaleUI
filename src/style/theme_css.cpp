/* Theme stylesheets: the shared UA baseline, the shared component classes
 * and the per-style control overlay. Tokens come from the reference design
 * systems (fluent-css, Fabric Core 9.6.1, mdui / Material 3, 7.css,
 * adwaita-web, photon, WHATWG UA + temp/Themes/Simple/readme.md).
 *
 * Two levels of fidelity, on purpose:
 *   - browser / Simple is written **inside the engine's current CSS
 *     subset** - it is the reference for "what renders today", so every
 *     rule in it works right now.
 *   - every other style is written the way its reference system actually
 *     is, **including** features the engine does not implement yet. Those
 *     rules carry a `needs:` marker; the engine is expected to grow into
 *     them (backlog: doc/internal/10-theme-gaps.md). Until it does, that
 *     part of the style simply does not show.
 *
 * Engine gaps referenced below. This list is MEASURED, not copied from the
 * docs: temp/gaps.html and temp/gaps2.html render one construct per row and
 * the pixels were sampled (numbers in notes/current.md).
 *
 * STATUS after the engine-gap round (full matrix:
 * doc/internal/16-engine-support.md). The `needs:` markers stay so a reader
 * can tell which reference rules are now actually live:
 *   FIXED multi-shadow   box-shadow lists paint every layer, spread
 *                        included; `0 0 0 3px` focus rings work (blur 0).
 *   FIXED outline        outline / outline-offset painted outside the box;
 *                        dotted/dashed approximated with dashes.
 *   FIXED 4-side-bd      `border-color: a b c d` honours each side.
 *   FIXED bg-size        background-size / -position / -repeat.
 *   FIXED inset          `inset: 0` sizes an absolutely positioned box.
 *   FIXED flex-rel       `inline-flex` + relative shrinks to content;
 *                        `overflow: hidden` no longer blows the height up.
 *   FIXED :checked       :checked / :indeterminate / [attr] match
 *                        (value-less HTML attributes need has_attribute).
 *   FIXED h-margin       four-value shorthand and single-side padding /
 *                        margin both apply.
 *   FIXED bg-anim        background-size / -position animation.
 *   FIXED grad-trans     transition between two gradients (stop-wise).
 *   FIXED backdrop       backdrop-filter works on the GPU path (not CPU).
 *   OPEN  transform      rotate() parses and interpolates, but the painter
 *                        still draws the box unrotated (translate and
 *                        scale do move/size it).
 *
 * Attribute selectors (`[type=text]`) and `:not()` work; so does
 * `display: table*` - see the UA section below. Pseudo elements are not on
 * the gap list: a `::before` with position/left/top/width/height +
 * background paints (its percentage sizing inside a flex box was part of
 * flex-rel and is fixed with it).
 *
 * Colors and metrics are NOT here: they are variables, filled by
 * theme_colors.cpp / the metrics table in theme.cpp. The variable names the
 * renderer consumes (--accent/--field/--border-strong/--card) and the
 * virtual .select-popup/.select-option* classes are unchanged. */

#include "style/theme_internal.h"

#include <cstring>

/* =====================================================================
 * 1. UA baseline - every tag gets a default (WHATWG UA stylesheet, cut
 * down to what the engine can lay out). Shared by ALL styles, so this
 * section stays inside the subset.
 * =================================================================== */
const char* const kWhaleuiUaCss = R"CSS(
/* --- 不渲染 --- */
head, style, script, title, meta, link, base, template, noscript,
source, track, param, area, wbr, dialog, option { display: none; }

/* --- 块级 --- */
div, section, article, nav, aside, header, footer, main, address,
p, hr, pre, blockquote, ol, ul, menu, li, dl, dt, dd, figure,
figcaption, caption, colgroup, col, form, fieldset, details,
picture, map, iframe, embed, object, video, audio, canvas { display: block; }

/* --- 表格：引擎有真正的 table 布局，别把 display 覆盖成 block ---
 * 实测（temp/gaps3.html）：display:table / table-row / table-cell 下三个 td
 * 横排成 (0,75)(100,75)(200,75) 各 100px 宽。 */
table { display: table; border-collapse: collapse; }
thead, tbody, tfoot { display: table-row-group; }
tr { display: table-row; }
td, th { display: table-cell; }

/* --- 行内 --- */
a, em, strong, small, s, cite, q, dfn, abbr, data, time, code, var,
samp, kbd, sub, sup, i, b, u, mark, ruby, rt, rp, bdi, bdo, span,
label { display: inline; }

/* --- 行内块 --- */
img, input, button, select, textarea, output, progress, meter,
datalist { display: inline-block; }

/* --- 页面与正文 --- */
/* body 不带外边距：页面默认零留白是既有契约(多处按坐标断言裸页面左上角就是
 * 内容)。Simple 要求的"留白稍大"由组件层承担(.app 的 padding、控件高度与
 * 行距),不动裸页面的盒模型。 */
html { background-color: var(--bg); }
body {
    margin: 0;
    background-color: var(--bg);
    color: var(--fg);
    font-family: var(--font-sans);
    font-size: var(--text-base);
    line-height: var(--line-height);
}

/* --- 标题：字号阶梯由主题给出（Fluent Title / M3 Headline / Adwaita Title） --- */
h1, h2, h3, h4, h5, h6 {
    margin: 0.8em 0 0.4em;
    font-weight: var(--h-weight);
    line-height: 1.3;
    color: var(--heading);
}
h1 { font-size: var(--h1); }
h2 { font-size: var(--h2); }
h3 { font-size: var(--h3); }
h4 { font-size: var(--h4); }
h5 { font-size: var(--h5); }
h6 { font-size: var(--h6); }

/* --- 排版 --- */
p { margin: 0.6em 0; }
strong, b { font-weight: 700; }
em, i, var, cite, dfn, q { font-style: italic; }
small { font-size: 0.85em; }
s { text-decoration: line-through; }
u { text-decoration: underline; }
mark { background-color: var(--mark-bg); color: var(--mark-fg); }
sub, sup { font-size: 0.75em; }
code, kbd, samp, pre { font-family: var(--font-mono); font-size: 0.9em; }
kbd {
    padding: 1px 5px;
    border: 1px solid var(--border);
    border-radius: 3px;
    background-color: var(--panel);
}
pre {
    margin: 0.7em 0; padding: 10px 12px;
    background-color: var(--panel);
    border: 1px solid var(--border);
    border-radius: var(--radius-sm);
    overflow: auto; white-space: pre;
}
blockquote {
    margin: 0.7em 0; padding: 2px 0 2px 14px;
    border-left: 3px solid var(--border);
    color: var(--muted);
}
ul, ol { margin: 0.6em 0; padding-left: 26px; }
li { margin: 3px 0; }
dl { margin: 0.6em 0; }
dt { font-weight: 600; }
dd { margin: 2px 0 6px 26px; }
figure { margin: 0.8em 0; }
figcaption { margin-top: 4px; font-size: var(--text-sm); color: var(--muted); }
summary { padding: 2px 0; cursor: pointer; }
hr { margin: 1em 0; border: none; border-top: 1px solid var(--border); }
img { max-width: 100%; }
table { width: 100%; }
td, th { padding: 7px 10px; border-bottom: 1px solid var(--border); text-align: left; }
th { font-weight: 600; color: var(--heading); }
caption { padding: 4px; font-size: var(--text-sm); color: var(--muted); }
::selection { background-color: var(--selection-bg); color: var(--selection-fg); }
)CSS";

/* =====================================================================
 * 2. Component classes + control baseline, shared by every style - so it
 * stays in the subset too. A style's own overlay (section 3) adds the
 * reference-only chrome on top; class selectors out-rank tag selectors, so
 * a style's tag rules never fight these.
 * =================================================================== */
const char* const kWhaleuiComponentsCss = R"CSS(
/* --- 链接 --- */
a { color: var(--link); text-decoration: none; cursor: pointer; transition: color 0.12s linear; }
a:hover { color: var(--link-hover); text-decoration: underline; }

/* --- 按钮基线 --- */
button {
    display: inline-flex; align-items: center; justify-content: center;
    box-sizing: border-box;
    /* `height` (a DEFAULT), not `min-height`: min-height always wins over an
     * author's `height`, so `<button style="height:19px">` rendered 32px tall
     * and the app's 34x19 toggle track pushed its knob outside. A UA `height`
     * is overridden by any author height, and the layout pass still grows an
     * explicit-height box to its content when the content is taller (the
     * auto-height branch), so multi-line buttons keep working. */
    height: var(--btn-h); padding: 0 var(--btn-px);
    font-family: inherit; font-size: var(--text-base); font-weight: var(--btn-weight);
    line-height: 1; text-align: center;
    color: var(--btn-fg);
    background: var(--btn-bg);
    border: 1px solid var(--btn-border);
    border-radius: var(--radius-btn);
    cursor: pointer;
    transition: background-color 0.1s linear, border-color 0.1s linear,
                color 0.1s linear, box-shadow 0.1s linear;
}
button:hover { background: var(--btn-bg-hover); border-color: var(--btn-border-hover); }
button:active { background: var(--btn-bg-active); }
button:disabled { opacity: var(--disabled-opacity); cursor: default; }

/* --- 输入基线（focus 环按主题补，见第 3 节） --- */
input, select, textarea {
    box-sizing: border-box;
    min-height: var(--in-h); padding: 0 var(--in-px);
    font-family: inherit; font-size: var(--text-base);
    color: var(--field-fg);
    background-color: var(--field);
    border: 1px solid var(--field-border);
    border-radius: var(--radius-field);
    transition: border-color 0.15s linear, box-shadow 0.15s linear;
}
input:hover, select:hover, textarea:hover { border-color: var(--field-border-hover); }
input:focus, select:focus, textarea:focus { border-color: var(--accent); }
input:disabled, select:disabled, textarea:disabled { opacity: var(--disabled-opacity); }
/* checkbox/radio 是 16px 原生控件：不吃输入框的 min-height / padding，
 * 否则布局盒被撑成 32px，而绘制仍是 16x16 —— 命中区与画面不一致
 * （temp/engine-gaps.md P0#2 实测 20x32）。尺寸是 UA 默认（单写点）：
 * 布局不再手写 16px，引擎的绘制盒与命中盒都从这里来。 */
input[type="checkbox"], input[type="radio"] {
    min-height: 0; min-width: 0; padding: 0;
    width: 16px; height: 16px; border: none; background: none;
}
textarea { padding: 6px var(--in-px); overflow: auto; }
/* form-control UA default sizes (primary source - the C++ in layout.cpp
 * mirrors these values as the stylesheet-less fallback only): text input
 * 12em wide, textarea 12em x 60px scrolling, select + input 28px field
 * height. Explicit author width/height win by cascade. */
input[type="text"], input:not([type]) { width: 12em; }
select, input[type="text"], input:not([type]),
input[type="password"], input[type="number"], input[type="email"],
input[type="search"], input[type="tel"], input[type="url"] { height: 28px; }
textarea { width: 12em; height: 60px; }
select { cursor: pointer; }
/* progress/meter share the renderer's track+fills paint: wide track, fixed
 * height. One UA rule for both (the old C++ 16em/6px and the old
 * `progress{100%/4px}` disagreed - this is the single source). */
progress, meter { width: 16em; height: 6px; padding: 0; border: none; }
/* replaced-element intrinsic size (browser default when no src/dimensions) */
img { width: 300px; height: 150px; }

/* --- 布局工具 --- */
.row { display: flex; flex-direction: row; gap: 10px; }
.column { display: flex; flex-direction: column; gap: 8px; }
.center { display: flex; justify-content: center; align-items: center; }
.app { display: flex; flex-direction: column; gap: 16px; padding: 28px 32px; }
.header { display: flex; align-items: center; justify-content: space-between; gap: 16px; }
.wrap { flex-wrap: wrap; }
.grow { flex-grow: 1; }
.space-between { justify-content: space-between; }

/* --- 卡片：表面 + 阴影；hover 阴影加深/描边加重（transform 不位移元素） --- */
.card {
    padding: 16px 18px;
    background-color: var(--card); color: var(--fg);
    border: 1px solid var(--border);
    border-radius: var(--radius-lg);
    box-shadow: var(--shadow-sm);
    transition: box-shadow 0.18s linear, border-color 0.18s linear;
}
.card:hover { border-color: var(--border-strong); box-shadow: var(--shadow-md); }

/* --- 按钮变体 --- */
.btn-primary { background: var(--accent); color: var(--accent-fg); border-color: transparent; }
.btn-primary:hover { background: var(--accent-hover); border-color: transparent; }
.btn-primary:active { background: var(--accent-active); border-color: transparent; }
.btn-secondary { background: transparent; color: var(--accent); border-color: var(--accent); }
.btn-secondary:hover { background: var(--hover); }
.btn-subtle { background: transparent; color: var(--fg); border-color: transparent; }
.btn-subtle:hover { background: var(--hover); border-color: transparent; }
.btn-outline { background: transparent; border-color: var(--border-strong); }
.btn-outline:hover { background: var(--hover); border-color: var(--accent); }
.btn-danger { background: var(--danger); color: var(--danger-fg); border-color: transparent; }
.btn-danger:hover { background: var(--danger-hover); border-color: transparent; }

/* --- 标签页：下划线跟随选中项（.selected 由宿主应用切换） --- */
.tabs { display: flex; flex-direction: row; gap: 2px; border-bottom: 1px solid var(--border); }
.tab {
    padding: 8px 14px;
    border: none; border-radius: 0;
    background: transparent; color: var(--muted); font-weight: 400;
}
.tab:hover { background: var(--hover); color: var(--fg); }
.tab.selected { color: var(--heading); font-weight: 600; box-shadow: inset 0 -2px 0 var(--accent); }

/* --- 工具条 / 窗格 / 侧栏 --- */
.toolbar {
    display: flex; align-items: center; gap: 8px; padding: 8px 12px;
    background-color: var(--panel);
    border-bottom: 1px solid var(--border);
}
.pane { flex: 1; overflow: auto; padding: 12px; }
.sidebar { background-color: var(--sidebar); border-right: 1px solid var(--border); }

/* --- 文本 / 徽标 / 尺寸 --- */
.muted { color: var(--muted); }
.small { font-size: var(--text-sm); }
.text-left { text-align: left; }
.text-center { text-align: center; }
.text-right { text-align: right; }
.hidden { display: none; }
.disabled { opacity: var(--disabled-opacity); }
.badge {
    display: inline-block; padding: 2px 10px;
    background-color: var(--accent); color: var(--accent-fg);
    border-radius: 10px; font-size: var(--text-sm); font-weight: 600;
}
.size-sm { min-height: 26px; font-size: var(--text-sm); }
.size-lg { min-height: 40px; font-size: var(--text-lg); }

/* --- select 弹层：渲染器按这些虚拟类取外观（whaleui_style_virtual） --- */
.select-popup {
    background-color: var(--card); color: var(--fg);
    border: 1px solid var(--border-strong);
    border-radius: var(--radius-md);
    box-shadow: var(--shadow-lg);
}
.select-option { color: var(--fg); }
.select-option-hover { background-color: var(--hover); color: var(--heading); }
.select-option-selected { background-color: var(--accent); color: var(--accent-fg); }

/* --- 动画：@keyframes 由引擎动画系统驱动 --- */
@keyframes wui-fade-in { from { opacity: 0; } to { opacity: 1; } }
@keyframes wui-rise {
    from { opacity: 0; transform: translateY(12px); }
    to   { opacity: 1; transform: translateY(0); }
}
@keyframes wui-pulse {
    0%   { opacity: 0.5; }
    50%  { opacity: 1; }
    100% { opacity: 0.5; }
}
@keyframes wui-spin {
    from { transform: rotate(0deg); }
    to   { transform: rotate(360deg); }
}
.anim-fade-in { animation: wui-fade-in 0.3s ease-out both; }
.anim-rise { animation: wui-rise 0.4s ease-out both; }
.anim-pulse { animation: wui-pulse 1.6s ease-in-out infinite; }
.anim-spin { animation: wui-spin 1.2s linear infinite; }
)CSS";

/* =====================================================================
 * 3. Per-style overlay.
 * =================================================================== */

/* ---------- browser / Simple（手写：勾线边框 + 稍大留白 + 黑白基调） ----------
 * 本主题刻意只用引擎当前已支持的写法（Simple readme：样式最简，只做勾线
 * 边框、简单图像）。它是"现在就能正确渲染"的基准。 */
const char* const kBrowserCss = R"CSS(
button { transition: background-color 0.16s ease-out, border-color 0.16s ease-out; }
button:focus { border-color: var(--accent); box-shadow: 0 0 0 2px var(--focus-ring); }
input:focus, select:focus, textarea:focus { box-shadow: 0 0 0 2px var(--focus-ring); }
.tab { border-radius: var(--radius-btn); }
)CSS";

/* ---------- fluent（Win11 Fluent 2 / fluent-css） ----------
 * fixed: pseudo-box + bg-anim —— 输入框 focus 下划线在参考里是
 *        background-size 0%→100% 的展开；这里用 ::after + scaleX 表达。
 * fixed: multi-shadow —— elevation 是"环境层 + 键层"两层（变量表按参考给）。
 * fixed: [type=] / :checked —— 勾选框与开关样式可写；checkbox/radio 仍由
 *        原生绘制（读 --accent），所以主题里的 :checked 规则用不上的部分
 *        保持原样即可。
 * 按钮状态层：fluent-css 直接替换 background-color（不是 ::before 叠层），
 * 这一半引擎今天就能渲染；输入框的 position: relative 现在是安全的。 */
const char* const kFluentCss = R"CSS(
input, select, textarea { position: relative; border-bottom-color: var(--border-strong); }
input::after, select::after, textarea::after {
    content: ""; position: absolute; left: 0; right: 0; bottom: 0; height: 2px;
    background-color: var(--accent);
    transform: scaleX(0); transition: transform 0.15s ease-out;
}
input:focus::after, select:focus::after, textarea:focus::after { transform: scaleX(1); }
input:focus, select:focus, textarea:focus { border-bottom-color: var(--accent); }
button:focus { border-color: var(--accent); }
input[type="checkbox"]:checked { background-color: var(--accent); border-color: var(--accent); }
input[type="checkbox"]:checked::after { content: "\2713"; color: var(--accent-fg); }
input[type="radio"] { border-radius: 999px; }
input[type="radio"]:checked { border-color: var(--accent); }
)CSS";

/* ---------- metro（Fabric Core 9.6.1 / Win8） ----------
 * fixed: outline —— Fabric 的 focus 是 1px 实线描边（outline-offset: -1px）。
 * fixed: :checked —— 勾选框整块 accent 填充（Fabric 原生观感）。
 * Metro 无圆角无阴影，状态靠 hover/active 色块 —— 这些引擎今天就能渲染。 */
const char* const kMetroCss = R"CSS(
input, select, textarea {
    background-color: transparent;
    border: 1px solid var(--field-border);
    border-radius: 0;
    padding: 0 8px;
}
input:hover, select:hover, textarea:hover { border-color: var(--field-border-hover); }
input:focus, select:focus, textarea:focus {
    border-color: var(--accent);
    outline: 1px solid var(--accent);
    outline-offset: -1px;
}
button:focus { outline: 1px solid var(--btn-fg); outline-offset: -1px; }
input[type="checkbox"]:checked, input[type="radio"]:checked {
    background-color: var(--accent);
    border-color: var(--accent);
}
progress { height: 6px; border-radius: 0; }
)CSS";

/* ---------- aero（7.css / Win7） ----------
 * fixed: pseudo-box —— hover / active 是整块渐变覆盖层（::before / ::after
 *        的 opacity 0→1），不是直接换背景。
 * fixed: flex-rel —— 覆盖层要求按钮可定位；inline-flex + relative 现在
 *        正常收缩到内容宽，按钮照参考写 inline-flex + flex 居中。
 * fixed: grad-trans —— 进 0.3s / 出 1s linear 的渐显渐隐。
 * fixed: 4-side-bd —— 输入框四边不同色（上深、下亮）。
 * fixed: outline —— focus 的 1px 点线框。
 * fixed: :not() —— 参考用 :not(:hover) 写"离开"过渡（:not() 已实测支持）。
 * 按钮体三段渐变 + hover 边框变蓝 + 内白边 —— 引擎今天就能渲染。 */
const char* const kAeroCss = R"CSS(
button {
    display: inline-flex; align-items: center; justify-content: center;
    position: relative; min-width: 75px;
}
button::before, button::after {
    content: ""; position: absolute; left: 0; top: 0; width: 100%; height: 100%;
    border-radius: var(--radius-btn);
    opacity: 0; transition: opacity 0.3s linear;
}
button::before { background: var(--btn-bg-hover); }
button::after { background: var(--btn-bg-active); }
button:hover::before { opacity: 1; }
button:active::after { opacity: 1; }
button:not(:hover)::before { transition: opacity 1s linear; }
button:focus-visible {
    box-shadow: inset 0 0 0 2px #98d1ef;
    outline: 1px dotted var(--fg); outline-offset: -4px;
}
input, select, textarea {
    border-radius: 2px;
    border-color: #abadb3 #dbdfe6 #e3e9ef #e2e3ea;
    transition: border-color 0.5s linear;
}
input:hover, select:hover, textarea:hover { border-color: #5794bf #b7d5ea #c7e2f1 #c5daed; }
input:focus, select:focus, textarea:focus { border-color: #3d7bad #a4c9e3 #b7d9ed #b5cfe7; }
@keyframes wui-aero-pulse {
    from { box-shadow: inset 0 0 3px 1px #34deffdd; }
    to   { box-shadow: inset 0 0 1px 1px #34deffdd; }
}
button.default, button:focus { animation: wui-aero-pulse 1s ease infinite alternate; }
)CSS";

/* ---------- material（mdui / Material Design 3） ----------
 * fixed: pseudo-box —— M3 state layer 是 ::before 叠色（hover 8% / pressed 12%）。
 * fixed: multi-shadow —— elevation level1..5 是双层阴影（变量表按 mdui 给）。
 * fixed: flex-rel —— state layer 要求按钮可定位；inline-flex + relative
 *        现在正常收缩到内容宽，按钮照参考写 inline-flex + flex 居中。
 * fixed: overflow-clip —— ripple 的 overflow: hidden 现在能写，但**按钮基础
 *        样式不加**：overflow≠visible 的元素在 flex 行里 min-content 归零
 *        （规范行为），拥挤的一行会把按钮压到 1px；ripple 自己按需加即可。
 * fixed: :checked —— 勾选框的 accent 填充与勾。
 * fixed: bg-anim —— 不确定进度条的 background-position 动画。
 * 胶囊形状、tonal/outlined/text 变体、颜色 token —— 引擎今天就能渲染。 */
const char* const kMaterialCss = R"CSS(
button {
    display: inline-flex; align-items: center; justify-content: center;
    position: relative;
    box-shadow: var(--shadow-sm);
}
button::before {
    content: ""; position: absolute; left: 0; top: 0; width: 100%; height: 100%;
    border-radius: var(--radius-btn);
    background-color: var(--accent-fg);
    opacity: 0; transition: opacity 0.2s cubic-bezier(0.2, 0, 0, 1);
}
button:hover::before { opacity: 0.08; }
button:active::before { opacity: 0.12; }
button:hover { box-shadow: var(--shadow-md); }
button:active { box-shadow: none; }
button:disabled::before { opacity: 0; }
button:focus { box-shadow: 0 0 0 2px var(--focus-ring); }
input:focus, select:focus, textarea:focus {
    border-color: var(--accent);
    box-shadow: 0 0 0 1px var(--accent);
}
.btn-secondary, .btn-subtle { box-shadow: none; }
.btn-secondary:hover, .btn-subtle:hover { box-shadow: none; }
.btn-outline { box-shadow: none; border-color: var(--border-strong); }
.btn-outline:hover { box-shadow: none; }
input[type="checkbox"]:checked, input[type="radio"]:checked {
    background-color: var(--accent);
    border-color: var(--accent);
}
)CSS";

/* ---------- gtk（adwaita-web / GNOME Adwaita） ----------
 * fixed: multi-shadow —— 输入框是"inset 内阴影 + 2px focus 环"两层。
 * fixed: :checked —— 勾选框/开关的 accent 填充。
 * 按钮 1px 边框 + 2px focus 环（单阴影）—— 引擎今天就能渲染。 */
const char* const kGtkCss = R"CSS(
button:focus { box-shadow: 0 0 0 2px var(--focus-ring); }
input, select, textarea { box-shadow: inset 0 1px 1px var(--shade); }
input:focus, select:focus, textarea:focus {
    border-color: var(--accent);
    box-shadow: inset 0 1px 1px var(--shade), 0 0 0 2px var(--focus-ring);
}
input[type="checkbox"]:checked, input[type="radio"]:checked {
    background-color: var(--accent);
    border-color: var(--accent);
}
)CSS";

/* ---------- macos（photon） ----------
 * fixed: 4-side-bd —— 按钮底边比其余三边深（photon 的立体感：参考写的是
 *        border-bottom-color: darken(border)），当前退化成单色边框。
 * fixed: multi-shadow —— focus 外发光环与边框叠加。
 * 渐变按钮体 + active 变平 —— 引擎今天就能渲染。 */
const char* const kMacosCss = R"CSS(
button {
    border-color: var(--border-strong) var(--border-strong)
                  var(--btn-border-hover) var(--border-strong);
}
button:active { border-color: var(--border-strong); }
button:focus { box-shadow: 0 0 0 3px var(--focus-ring); }
input, select, textarea { border-bottom-color: var(--btn-border-hover); }
input:focus, select:focus, textarea:focus {
    border-color: var(--accent);
    box-shadow: 0 0 0 3px var(--focus-ring);
}
)CSS";

const char* whaleui_theme_overlay_css(const char* style)
{
    const char* s = style ? style : "fluent";
    if (std::strcmp(s, "browser") == 0) {
        return kBrowserCss;
    }
    if (std::strcmp(s, "fluent") == 0) {
        return kFluentCss;
    }
    if (std::strcmp(s, "metro") == 0) {
        return kMetroCss;
    }
    if (std::strcmp(s, "material") == 0) {
        return kMaterialCss;
    }
    if (std::strcmp(s, "aero") == 0) {
        return kAeroCss;
    }
    if (std::strcmp(s, "gtk") == 0) {
        return kGtkCss;
    }
    return kMacosCss;
}
