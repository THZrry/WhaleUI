/* Theme styles: browser / Fluent / Metro / Classic / Aero / Material / GTK /
 * macOS. Each style = shared UA baseline + shared component classes +
 * per-style control theme, plus a light/dark variable table.
 *
 * These stylesheets are TRANSLATIONS of existing, verified design systems.
 * Where the engine's CSS subset is missing a feature the reference relies
 * on, the rule is still written (marked "needs:") so the engine can be
 * extended to match - the list lives in doc/internal/10-theme-gaps.md:
 *   needs: pseudo-box   ::before/::after paint as real boxes (position/
 *                         width/height/inset/background/border-radius)
 *   needs: multi-shadow box-shadow with more than one shadow / spread
 *   needs: grad-trans   transitions between two background gradients
 *   needs: :checked     :checked/:indeterminate pseudo-classes (switch)
 *   needs: outline      outline / outline-offset
 *   needs: 4-side-bd    per-side border-color (top/right/bottom/left)
 *   needs: backdrop     backdrop-filter (Aero glass)
 *
 * Reference sources, marked inline:
 *   - browser : hand-written UA default stylesheet (WHATWG HTML standard)
 *   - fluent  : https://github.com/aipx-proto/fluent-css
 *   - metro   : Office UI Fabric Core 9.6.1
 *   - classic : hand-written Windows Classic translation
 *   - aero    : https://github.com/khang-nd/7.css
 *   - material: https://github.com/zdhxiong/mdui (Material Design 3)
 *   - gtk     : https://github.com/mclellac/adwaita-web
 *   - macos   : https://github.com/connors/photon
 *
 * Renderer-native controls (checkbox/radio/progress/scrollbar) read
 * --accent/--field/--border/--card; the select popup is drawn from the
 * .select-popup/.select-option virtual classes. */

#include "style/theme.h"

#include <cstdio>
#include <cstring>

namespace {

struct ThemeDef
{
    const char* name;
    const char* label;
    int btn_radius;   /* button border-radius px (999 = capsule) */
    int in_radius;    /* input border-radius px */
    int card_radius;  /* card border-radius px */
};

const ThemeDef kThemes[] = {
    {"fluent",   "Fluent (Win11)",   4,   4,   8},
    {"metro",    "Metro (Win8)",     0,   0,   0},
    {"material", "Material Design",  999, 4,   12},
    {"classic",  "Windows Classic",  2,   0,   0},
    {"aero",     "Windows 7 Aero",   3,   2,   4},
    {"gtk",      "GTK (Adwaita)",    6,   6,   6},
    {"macos",    "macOS",            4,   4,   6},
    {"browser",  "Browser (UA)",     4,   4,   8},
};
const int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));

const ThemeDef* find_def(const char* name)
{
    for (int i = 0; i < kThemeCount; ++i) {
        if (std::strcmp(kThemes[i].name, name) == 0) {
            return &kThemes[i];
        }
    }
    return &kThemes[0];
}

/* =====================================================================
 * 1. UA baseline - every tag gets a default (WHATWG HTML UA stylesheet).
 * =================================================================== */
const char* kUaCss = R"(
/* --- UA 默认样式：每个 tag 一个默认外观（WHATWG HTML 标准） --- */
head, style, script, title, meta, link, base, template, noscript,
source, track, param, area, wbr { display: none; }
div, section, article, nav, aside, header, footer, main, address,
p, hr, pre, blockquote, ol, ul, menu, li, dl, dt, dd, figure,
figcaption, caption, colgroup, col, form, fieldset, details,
dialog, picture, map, iframe, embed, object, video, audio, canvas {
    display: block;
}
table { display: table; border-collapse: collapse; }
thead, tbody, tfoot { display: table-row-group; }
tr { display: table-row; }
td, th { display: table-cell; }
a, em, strong, small, s, cite, q, dfn, abbr, data, time, code, var,
samp, kbd, sub, sup, i, b, u, mark, ruby, rt, rp, bdi, bdo, span,
label { display: inline; }
img, input, button, select, textarea, output, progress, meter,
datalist { display: inline-block; }
html { background-color: var(--bg); }
body {
    background-color: var(--bg); color: var(--fg);
    font-family: var(--font-sans); font-size: var(--text-base);
    line-height: 1.5;
}
h1 { font-size: 24px; margin: 0.7em 0 0.4em; font-weight: 600; color: var(--heading); }
h2 { font-size: 20px; margin: 0.7em 0 0.4em; font-weight: 600; color: var(--heading); }
h3 { font-size: 17px; margin: 0.7em 0 0.4em; font-weight: 600; color: var(--heading); }
h4 { font-size: 15px; margin: 0.7em 0 0.4em; font-weight: 600; color: var(--heading); }
h5 { font-size: 13px; margin: 0.7em 0 0.4em; font-weight: 600; color: var(--heading); }
h6 { font-size: 12px; margin: 0.7em 0 0.4em; font-weight: 600; color: var(--heading); }
p { margin: 0.5em 0; }
strong, b { font-weight: 700; }
em, i, var, cite, dfn { font-style: italic; }
small { font-size: 0.85em; }
s { text-decoration: line-through; }
u { text-decoration: underline; }
mark { background-color: var(--selection-bg); color: var(--selection-fg); }
sub, sup { font-size: 0.75em; }
code, kbd, samp, pre { font-family: var(--font-mono); font-size: 0.9em; }
kbd {
    border: 1px solid var(--border); border-radius: 3px;
    padding: 1px 4px; background: var(--card);
    box-shadow: 0 1px 0 var(--border);
}
pre { margin: 0.5em 0; padding: 8px 10px; background: var(--card);
      border: 1px solid var(--border); border-radius: var(--radius-sm);
      overflow: auto; white-space: pre; }
blockquote {
    margin: 0.5em 0; padding: 2px 0 2px 14px;
    border-left: 3px solid var(--border); color: var(--muted);
}
ul, ol { margin: 0.5em 0; padding-left: 26px; }
li { margin: 3px 0; }
dl { margin: 0.5em 0; }
dt { font-weight: 600; }
dd { margin: 2px 0 6px 26px; }
figure { margin: 0.8em 0; }
figcaption { font-size: 12px; color: var(--muted); margin-top: 4px; }
summary { cursor: pointer; }
dialog { display: none; }
option { display: none; }
hr { border: none; border-top: 1px solid var(--border); margin: 0.9em 0; }
img { max-width: 100%; }
td, th { padding: 4px 8px; border: 1px solid var(--border); }
th { font-weight: 600; background: var(--card); }
caption { caption-side: top; padding: 4px; color: var(--muted); font-size: 12px; }
::selection { background: var(--selection-bg); color: var(--selection-fg); }
)";

/* =====================================================================
 * 2. Component classes + keyframes - shared by ALL themes. Class
 * selectors out-rank tag selectors, so a theme's tag rules never fight
 * them.
 *
 * The button/input interaction chrome is built on an overlay layer
 * (::before = hover, ::after = active) with an opacity transition - the
 * modern (Win11 / M3 / Adwaita) "state layer" pattern. needs: pseudo-box
 * =================================================================== */
const char* kComponentsCss = R"(
/* --- 布局工具 --- */
.row { display: flex; flex-direction: row; gap: 12px; }
.column { display: flex; flex-direction: column; gap: 8px; }
.center { display: flex; justify-content: center; align-items: center; }
.app { display: flex; flex-direction: column; gap: 14px; padding: 24px 28px; }
.header { display: flex; justify-content: space-between; align-items: center; }
.wrap { flex-wrap: wrap; }
.grow { flex-grow: 1; }
.no-shrink { flex-shrink: 0; }
.space-between { justify-content: space-between; }

/* --- 卡片：表面 + 阴影 + hover 上浮（transform 可过渡） --- */
.card {
    background: var(--card); border: 1px solid var(--border);
    border-radius: var(--radius-lg); padding: 16px 18px;
    box-shadow: var(--shadow);
    transition: transform 0.18s, box-shadow 0.18s; /* needs: shadow transition */
}
.card:hover { transform: translateY(-2px); box-shadow: var(--shadow-lg); }

/* --- 按钮：状态覆盖层 + 按压下沉。按钮默认外观由主题定义，class 只改色 --- */
/* note: 不加 position:relative - 引擎 relative 布局 bug 会把 inline-block
 * 按钮压成竖线（needs: relative 布局修复后，覆盖层 ::before/::after
 * 才能用 position:absolute 铺满） */
button::before, button::after {
    content: ""; position: absolute; inset: 0; pointer-events: none;
    opacity: 0; transition: opacity 0.15s; /* needs: pseudo-box */
}
button:hover::before { opacity: 1; }
button:active::before { opacity: 0; }
button:active::after { opacity: 1; }
.btn-primary { background: var(--accent); color: var(--accent-fg); border-color: transparent; }
.btn-primary:hover { background: var(--accent-hover); }
.btn-primary:active { background: var(--accent-active); }
.btn-secondary { background: transparent; color: var(--accent); }
.btn-secondary:hover { background: var(--btn-bg-hover); }
.btn-subtle { background: transparent; border-color: transparent; color: var(--fg); }
.btn-subtle:hover { background: var(--btn-bg-hover); }
.btn-outline { background: transparent; }
.btn-outline:hover { background: var(--btn-bg-hover); }
.btn-danger { background: var(--danger); color: #ffffff; border-color: transparent; }
.btn-danger:hover { background: var(--btn-bg-active); }

/* --- 输入框 focus 下划线（::after scaleX 展开，needs: pseudo-box +
 * transition on transform 非元素自身） --- */
input::after, select::after, textarea::after {
    content: ""; position: absolute; left: 0; right: 0; bottom: 0; height: 2px;
    background: var(--accent); transform: scaleX(0); transition: transform 0.2s;
} /* needs: pseudo-box */
input:focus::after, select:focus::after, textarea:focus::after { transform: scaleX(1); }

/* --- 文本工具 --- */
.muted { color: var(--muted); }
.text-left { text-align: left; }
.text-center { text-align: center; }
.text-right { text-align: right; }
.small { font-size: 12px; }
.hidden { display: none; }
.disabled { opacity: var(--disabled-opacity); }
.badge {
    display: inline-block; padding: 2px 10px;
    background: var(--accent); color: var(--accent-fg);
    border-radius: 999px; font-size: 11px; font-weight: 600;
    box-shadow: 0 1px 2px rgba(0,0,0,0.15);
}

/* --- 标签页：下划线跟随选中项（.selected 由宿主应用切换） --- */
.tabs { display: flex; flex-direction: row; gap: 2px; border-bottom: 1px solid var(--border); }
.tab {
    padding: 8px 14px; cursor: pointer; background: transparent;
    border: none; border-radius: 0; color: var(--fg); font-weight: 400;
    transition: color 0.12s, background-color 0.12s;
}
.tab:hover { background: var(--btn-bg-hover); }
.tab.selected {
    color: var(--heading); font-weight: 600;
    box-shadow: inset 0 -2px 0 var(--accent);
}

/* --- 工具条 / 窗格 / 侧栏 --- */
.toolbar {
    display: flex; align-items: center; gap: 8px; padding: 6px 10px;
    background: var(--panel); border-bottom: 1px solid var(--border);
}
.pane { flex: 1; overflow: auto; border-left: 1px solid var(--border); padding: 12px; }
.pane:first-child { border-left: none; }
.sidebar { background: var(--sidebar); border-right: 1px solid var(--border); }

/* --- 尺寸 --- */
.size-sm { font-size: var(--text-sm); min-height: 24px; }
.size-lg { font-size: var(--text-lg); min-height: 40px; }

/* --- select 弹出层（渲染器从这些虚拟 class 计算外观，needs: virtual
 * element style - style.cpp whaleui_style_virtual） --- */
.select-popup {
    background: var(--card); color: var(--fg);
    border: 1px solid var(--border-strong);
    border-radius: var(--radius-md);
    box-shadow: var(--shadow-lg);
}
.select-option {
    color: var(--fg);
}
.select-option-hover {
    background: var(--btn-bg-hover);
    color: var(--heading);
}
.select-option-selected {
    background: var(--accent);
    color: var(--accent-fg);
}

/* --- 动画 keyframes + 工具类 --- */
@keyframes wui-fade-in { from { opacity: 0; } to { opacity: 1; } }
@keyframes wui-rise {
    from { opacity: 0; transform: translateY(14px); }
    to   { opacity: 1; transform: none; }
}
@keyframes wui-pulse {
    0% { opacity: 0.45; }
    50% { opacity: 1; }
    100% { opacity: 0.45; }
}
@keyframes wui-spin {
    from { transform: rotate(0deg); }
    to   { transform: rotate(360deg); }
}
@keyframes wui-shine {
    from { transform: translateX(-100%); }
    to   { transform: translateX(200%); }
} /* needs: pseudo-box（按钮流光） */
.anim-fade-in { animation: wui-fade-in 0.3s ease-out both; }
.anim-rise    { animation: wui-rise 0.45s cubic-bezier(0.22,1,0.36,1) both; }
.anim-pulse   { animation: wui-pulse 1.6s ease-in-out infinite; }
.anim-spin    { animation: wui-spin 1.2s linear infinite; }
)";

/* =====================================================================
 * 3. Per-theme control themes. Each theme keeps the overlay layer from
 * kComponentsCss and paints its own recognizable chrome: gradient
 * bodies, borders, focus rings, the whole state machine.
 * =================================================================== */

/* --- browser: 浏览器默认 UA 控件（Chrome 观感，手写） --- */
const char* kBrowserCss = R"(
/* browser：Chrome 系默认控件（手写翻译） */
a { color: var(--link); }
a:hover { text-decoration: underline; }
button {
    background: linear-gradient(to bottom, #fafafa, #f1f1f1);
    color: var(--btn-fg); border: 1px solid #dadce0;
    border-radius: var(--radius-btn); min-height: 28px;
    padding: 0 14px; font-size: 13px; cursor: pointer;
    box-shadow: 0 1px 1px rgba(0,0,0,0.08);
    transition: background-color 0.12s, border-color 0.12s, box-shadow 0.12s;
}
button:hover {
    background: linear-gradient(to bottom, #f8f8f8, #ebebeb);
    border-color: #c6c9ce;
}
button:active {
    background: linear-gradient(to bottom, #e8e8e8, #f0f0f0);
    box-shadow: inset 0 1px 2px rgba(0,0,0,0.15);
    transform: translateY(1px);
}
button:disabled { opacity: var(--disabled-opacity); }
button::before { background: rgba(0,0,0,0.06); }
button::after { background: rgba(0,0,0,0.12); }
.btn-primary { background: linear-gradient(to bottom, #4d90fe, #357ae8); color: #fff; border-color: #2f5bb7; }
.btn-primary:hover { background: linear-gradient(to bottom, #5093ff, #3a7df0); }
.btn-primary:active { background: linear-gradient(to bottom, #3a7df0, #2f5bb7); }
.btn-primary::before { background: rgba(255,255,255,0.12); }
.btn-primary::after { background: rgba(0,0,0,0.15); }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid #dadce0; border-radius: var(--radius-btn);
    min-height: 28px; padding: 0 8px; font-size: 13px;
    box-shadow: inset 0 1px 2px rgba(0,0,0,0.06);
    transition: border-color 0.12s, box-shadow 0.12s;
}
input:hover, select:hover, textarea:hover { border-color: #b9bdc4; }
input:focus, select:focus, textarea:focus {
    border-color: var(--accent);
    box-shadow: inset 0 1px 2px rgba(0,0,0,0.06), 0 0 0 2px var(--focus-ring);
} /* needs: multi-shadow */
textarea { padding: 6px 8px; overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 14px; height: 14px; }
progress { width: 160px; height: 12px; }
)";

/* --- fluent: Win11 Fluent（fluent-css） --- */
const char* kFluentCss = R"(
/* fluent（来源：github.com/aipx-proto/fluent-css）：白底 + 细边框 +
 * hover 状态层覆盖 + focus 下划线 */
a { color: var(--link); text-decoration: none; transition: color 0.1s; }
a:hover { text-decoration: underline; }
button {
    background: var(--btn-bg); color: var(--btn-fg);
    border: 1px solid var(--btn-border); border-radius: var(--radius-btn);
    min-height: 32px; padding: 0 14px; font-size: var(--text-base);
    font-weight: 600; cursor: pointer; line-height: 1;
    box-shadow: inset 0 0 0 1px transparent;
    transition: background-color 0.1s, border-color 0.1s;
}
button:hover { background: var(--btn-bg-hover); border-color: var(--border-strong); }
button:active { background: var(--btn-bg-active); }
button:focus { box-shadow: inset 0 0 0 1px var(--accent); }
button:disabled { opacity: var(--disabled-opacity); }
button::before { background: rgba(0,0,0,0.05); } /* needs: pseudo-box */
button::after { background: rgba(0,0,0,0.1); }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--border); border-bottom-color: var(--border-strong);
    border-radius: var(--radius-btn); min-height: 32px;
    padding: 0 10px; font-size: var(--text-base);
    transition: border-color 0.15s;
}
input:hover, select:hover, textarea:hover { border-color: var(--border-strong); }
input:focus, select:focus, textarea:focus {
    border-color: var(--accent); border-bottom-color: var(--accent);
} /* 下划线展开动画走 kComponentsCss 的 ::after（needs: pseudo-box） */
textarea { padding: 6px 10px; overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 16px; height: 16px; }
progress { width: 100%; height: 4px; border-radius: 2px; }
)";

/* --- metro: Win8 Metro 扁平（Fabric Core 9.6.1 色板） --- */
const char* kMetroCss = R"(
/* metro（来源：Office UI Fabric Core 9.6.1 色板）：扁平无圆角无阴影，
 * hover 半透明块、主按钮 accent 实色块 */
a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }
button {
    background: transparent; color: var(--fg);
    border: none; border-radius: 0; min-height: 34px;
    padding: 0 16px; font-size: var(--text-base); cursor: pointer;
    transition: background-color 0.1s;
}
button:hover { background: var(--hover); }
button:active { background: var(--active); }
button:disabled { opacity: var(--disabled-opacity); }
button::before { background: var(--hover); }
button::after { background: var(--active); }
.btn-primary { background: var(--btn-bg); color: var(--btn-fg); }
.btn-primary:hover { background: var(--btn-bg-hover); }
.btn-primary:active { background: var(--btn-bg-active); }
.btn-primary::before { background: rgba(255,255,255,0.12); }
.btn-primary::after { background: rgba(0,0,0,0.18); }
input, select, textarea {
    background: transparent; color: var(--fg);
    border: none; border-bottom: 2px solid var(--border-strong);
    border-radius: 0; min-height: 32px; padding: 0 4px;
    font-size: var(--text-base);
    transition: border-color 0.12s;
}
input:focus, select:focus, textarea:focus { border-bottom-color: var(--accent); }
textarea { padding: 6px 4px; overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 16px; height: 16px; }
progress { width: 100%; height: 6px; border-radius: 0; }
)";

/* --- classic: Windows Classic 3D 斜面（手写翻译） --- */
const char* kClassicCss = R"(
/* classic（手写）：Win95/98 3D 斜面。引擎无四边异色边框/多阴影，用
 * 渐变 + 单 inset 阴影模拟斜角高光（needs: 4-side-bd, multi-shadow） */
a { color: var(--link); text-decoration: underline; }
button {
    background: linear-gradient(to bottom, #ffffff, #e0ddd5 48%, #d4d0c8 52%, #c8c4ba);
    color: var(--btn-fg); border: 1px solid #8a8678;
    border-radius: 2px; min-height: 26px; padding: 2px 12px;
    font-size: 13px; font-weight: normal; cursor: pointer;
    box-shadow: inset 1px 1px 0 #fff, inset -1px -1px 0 #8a8678;
    transition: none;
} /* needs: multi-shadow */
button:hover { background: linear-gradient(to bottom, #ffffcc, #e0ddd5 48%, #d4d0c8 52%, #c8c4ba); }
button:active {
    background: linear-gradient(to bottom, #c8c4ba, #d4d0c8 48%, #e0ddd5 52%, #ffffff);
    box-shadow: inset 1px 1px 0 #8a8678, inset -1px -1px 0 #fff;
    transform: translateY(1px);
} /* needs: multi-shadow */
button:disabled { color: var(--muted); text-shadow: 0 1px 0 #fff; }
button::before { background: rgba(0,0,0,0.03); }
button::after { background: rgba(0,0,0,0.06); }
.btn-primary { background: linear-gradient(to bottom, #f8f4e8, #e0ddd5 48%, #d4d0c8 52%, #c8c4ba); }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid #7f7c6e; border-radius: 0;
    min-height: 22px; padding: 2px 6px; font-size: 13px;
    box-shadow: inset 1px 1px 1px rgba(0,0,0,0.2);
}
input:focus, select:focus, textarea:focus { background: #ffffff; }
textarea { overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 13px; height: 13px; }
progress { width: 160px; height: 14px; border: 1px solid #8a8678; }
)";

/* --- aero: Win7 Aero 玻璃渐变（7.css） --- */
const char* kAeroCss = R"(
/* aero（来源：github.com/khang-nd/7.css）：按钮三段渐变 + ::before hover
 * 渐变蓝 + ::after active 渐变深蓝，边框 hover 变蓝，全部 opacity 过渡。
 * needs: pseudo-box, grad-trans, 4-side-bd（输入框四边深浅边框） */
a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }
button {
    background: linear-gradient(to bottom, var(--btn-bg) 45%, var(--btn-bg-s1) 45%, var(--btn-bg-s2));
    color: var(--btn-fg); border: 1px solid var(--btn-border);
    border-radius: var(--radius-btn); min-height: 26px; padding: 2px 12px;
    font-size: 13px; cursor: pointer;
    box-shadow: inset 0 0 0 1px rgba(255,255,255,0.8);
    transition: border-color 0.3s;
}
button:hover { border-color: #3c7fb1; }
button:active { border-color: #6d91ab; }
button:disabled { opacity: var(--disabled-opacity); }
button::before {
    background: linear-gradient(to bottom, #eaf6fd 45%, #bee6fd 45%, #a7d9f5);
} /* needs: pseudo-box - 7.css hover 渐变覆盖层 */
button::after {
    background: linear-gradient(to bottom, #e5f4fc, #c4e5f6 30%, #98d1ef 55%, #68b3db);
} /* needs: pseudo-box - 7.css active 渐变覆盖层 */
.btn-primary { background: linear-gradient(to bottom, #4fc1ff 45%, #2b8fd6 45%, #1a6cb0); }
.btn-primary:hover { border-color: #1a6cb0; }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid #abadb3; border-radius: var(--radius-btn);
    min-height: 24px; padding: 3px 6px; font-size: 13px;
    box-shadow: inset 0 1px 1px rgba(0,0,0,0.08);
    transition: border-color 0.3s;
}
input:hover, select:hover, textarea:hover { border-color: #3c7fb1; }
input:focus, select:focus, textarea:focus { border-color: #3d7bad; }
textarea { padding: 6px; overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 14px; height: 14px; }
progress { width: 160px; height: 14px; border-radius: 2px; }
)";

/* --- material: Material Design 3（mdui tokens） --- */
const char* kMaterialCss = R"(
/* material（来源：github.com/zdhxiong/mdui，M3）：胶囊主按钮 + elevation
 * 阴影 + 状态层（::before/::after = M3 state layer），文本按钮透明底 */
a { color: var(--link); text-decoration: none; transition: color 0.15s; }
a:hover { text-decoration: underline; }
button {
    background: var(--btn-bg); color: var(--btn-fg);
    border: none;
    border-radius: 18px; /* 胶囊 = min-height/2（999px 触发引擎胶囊渲染 bug） */
    min-height: 36px; padding: 0 22px; font-size: var(--text-base);
    font-weight: 600; cursor: pointer; text-transform: none;
    box-shadow: var(--shadow-sm);
    transition: background-color 0.15s, box-shadow 0.15s;
}
button:hover { background: var(--btn-bg-hover); box-shadow: var(--shadow); }
button:active { background: var(--btn-bg-active); box-shadow: none; transform: translateY(1px); }
button:disabled { opacity: var(--disabled-opacity); box-shadow: none; }
button::before { background: rgba(255,255,255,0.10); } /* needs: pseudo-box */
button::after { background: rgba(0,0,0,0.14); }
.btn-secondary, .btn-subtle { background: transparent; box-shadow: none; }
.btn-secondary:hover, .btn-subtle:hover { box-shadow: none; }
.btn-secondary { color: var(--accent); }
.btn-secondary:hover { background: var(--btn-bg-hover); }
.btn-outline {
    background: transparent; border: 1px solid var(--border-strong);
    box-shadow: none;
}
.btn-outline:hover { background: var(--btn-bg-hover); }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--border); border-radius: var(--radius-btn);
    min-height: 36px; padding: 0 12px; font-size: var(--text-base);
    transition: border-color 0.15s, box-shadow 0.15s;
}
input:focus, select:focus, textarea:focus {
    border-color: var(--accent);
    box-shadow: 0 0 0 1px var(--accent);
}
textarea { padding: 8px 12px; overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 18px; height: 18px; }
progress { width: 100%; height: 4px; border-radius: 2px; }
)";

/* --- gtk: GNOME Adwaita（adwaita-web） --- */
const char* kGtkCss = R"(
/* gtk（来源：github.com/mclellac/adwaita-web）：白/深底按钮 + 1px 边框 +
 * focus ring（0 0 0 2px accent），禁用整体透明度 */
a { color: var(--link); text-decoration: none; }
a:hover { color: var(--link-hover); text-decoration: underline; }
button {
    background: var(--btn-bg); color: var(--btn-fg);
    border: 1px solid var(--btn-border); border-radius: var(--radius-btn);
    min-height: 32px; padding: 4px 14px; font-size: var(--text-base);
    font-weight: 600; cursor: pointer;
    transition: background-color 0.15s, border-color 0.15s;
}
button:hover { background: var(--btn-bg-hover); }
button:active { background: var(--btn-bg-active); }
button:focus { box-shadow: 0 0 0 2px var(--focus-ring); }
button:disabled { opacity: var(--disabled-opacity); }
button::before { background: rgba(0,0,0,0.04); }
button::after { background: rgba(0,0,0,0.08); }
.btn-primary { background: var(--accent); color: var(--accent-fg); border-color: transparent; }
.btn-primary:hover { background: var(--accent-hover); }
.btn-primary:active { background: var(--accent-active); }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--border); border-radius: var(--radius-btn);
    min-height: 32px; padding: 4px 10px; font-size: var(--text-base);
    box-shadow: inset 0 1px 1px rgba(0,0,0,0.06);
    transition: border-color 0.1s, box-shadow 0.1s;
}
input:focus, select:focus, textarea:focus {
    border-color: var(--accent);
    box-shadow: inset 0 1px 1px rgba(0,0,0,0.06), 0 0 0 2px var(--focus-ring);
} /* needs: multi-shadow */
textarea { overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 16px; height: 16px; }
progress { width: 100%; height: 6px; border-radius: 3px; }
)";

/* --- macos: macOS（photon） --- */
const char* kMacosCss = R"(
/* macos（来源：github.com/connors/photon）：渐变按钮 + active 变平，
 * 输入框 focus 外发光环（needs: multi-shadow） */
a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }
button {
    background: linear-gradient(to bottom, var(--btn-bg), var(--btn-bg-hover));
    color: var(--btn-fg); border: 1px solid var(--btn-border);
    border-radius: var(--radius-btn); min-height: 26px; padding: 2px 12px;
    font-size: var(--text-sm); cursor: pointer; line-height: 1.4;
    box-shadow: 0 1px 1px rgba(0,0,0,0.06);
    transition: background 0.1s, box-shadow 0.1s;
}
button:hover { background: linear-gradient(to bottom, #ffffff, #e8e8e8); }
button:active { background: var(--btn-bg-active); box-shadow: none; }
button:disabled { opacity: var(--disabled-opacity); }
button::before { background: rgba(0,0,0,0.05); }
button::after { background: rgba(0,0,0,0.1); }
.btn-primary { background: linear-gradient(to bottom, #6eb4f7, #1a82fb); color: #fff; border-color: #388df8; }
.btn-primary:hover { background: linear-gradient(to bottom, #7ec0f8, #2e8cfb); }
.btn-primary:active { background: #3e9bf4; }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--border); border-radius: var(--radius-btn);
    min-height: 25px; padding: 3px 8px; font-size: var(--text-sm);
    line-height: 1.6;
    transition: border-color 0.12s, box-shadow 0.12s;
}
input:focus, select:focus, textarea:focus {
    border-color: #6db3fd;
    box-shadow: 0 0 0 3px rgba(109,179,253,0.4);
}
textarea { overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 14px; height: 14px; }
progress { width: 100%; height: 8px; border-radius: 4px; }
)";

/* =====================================================================
 * 4. Per-style default stylesheet = UA + components + theme overlay.
 * =================================================================== */
void build_css(const ThemeDef& d, std::string& css)
{
    css = kUaCss;
    css += kComponentsCss;
    if (std::strcmp(d.name, "fluent") == 0) {
        css += kFluentCss;
    } else if (std::strcmp(d.name, "metro") == 0) {
        css += kMetroCss;
    } else if (std::strcmp(d.name, "material") == 0) {
        css += kMaterialCss;
    } else if (std::strcmp(d.name, "classic") == 0) {
        css += kClassicCss;
    } else if (std::strcmp(d.name, "aero") == 0) {
        css += kAeroCss;
    } else if (std::strcmp(d.name, "gtk") == 0) {
        css += kGtkCss;
    } else if (std::strcmp(d.name, "macos") == 0) {
        css += kMacosCss;
    } else { /* browser */
        css += kBrowserCss;
    }
}

/* =====================================================================
 * 5. Light/dark variable tables.
 * =================================================================== */
void brighten_accent(const std::string& accent, std::string& out)
{
    unsigned int c = 0;
    if (accent[0] != '#' || std::sscanf(accent.c_str() + 1, "%x", &c) != 1) {
        out = accent;
        return;
    }
    unsigned int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r = static_cast<unsigned>((r * 3 + 255) / 4);
    g = static_cast<unsigned>((g * 3 + 255) / 4);
    b = static_cast<unsigned>((b * 3 + 255) / 4);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    out = buf;
}

void darken_accent(const std::string& accent, std::string& out)
{
    unsigned int c = 0;
    if (accent[0] != '#' || std::sscanf(accent.c_str() + 1, "%x", &c) != 1) {
        out = accent;
        return;
    }
    unsigned int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r = static_cast<unsigned>(r * 85 / 100);
    g = static_cast<unsigned>(g * 85 / 100);
    b = static_cast<unsigned>(b * 85 / 100);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    out = buf;
}

/* shadow ladder shared by most themes; aero/classic/metro override */
void default_shadows(std::map<std::string, std::string>& out, bool dark)
{
    const char* a = dark ? "0.4" : "0.12";
    const char* k = dark ? "0.45" : "0.14";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "0 0 2px rgba(0,0,0,%s), 0 1px 2px rgba(0,0,0,%s)", a, k);
    out["--shadow-2xs"] = buf;
    std::snprintf(buf, sizeof(buf), "0 0 2px rgba(0,0,0,%s), 0 2px 4px rgba(0,0,0,%s)", a, k);
    out["--shadow-xs"] = buf;
    std::snprintf(buf, sizeof(buf), "0 0 2px rgba(0,0,0,%s), 0 2px 6px rgba(0,0,0,%s)", a, k);
    out["--shadow-sm"] = buf;
    std::snprintf(buf, sizeof(buf), "0 0 2px rgba(0,0,0,%s), 0 4px 8px rgba(0,0,0,%s)", a, k);
    out["--shadow"] = buf;
    std::snprintf(buf, sizeof(buf), "0 0 2px rgba(0,0,0,%s), 0 6px 12px rgba(0,0,0,%s)", a, k);
    out["--shadow-md"] = buf;
    std::snprintf(buf, sizeof(buf), "0 0 4px rgba(0,0,0,%s), 0 8px 16px rgba(0,0,0,%s)", a, k);
    out["--shadow-lg"] = buf;
    std::snprintf(buf, sizeof(buf), "0 0 8px rgba(0,0,0,%s), 0 16px 32px rgba(0,0,0,%s)", a, k);
    out["--shadow-xl"] = buf;
}

void fill_vars(const ThemeDef& d, bool dark, const char* accent,
               std::map<std::string, std::string>& out)
{
    /* ---- shared tokens ---- */
    out["--font-sans"] = "\"Segoe UI\", \"Helvetica Neue\", Arial, sans-serif";
    out["--font-mono"] = "\"Cascadia Mono\", \"Courier New\", monospace";
    out["--text-sm"] = "12px";
    out["--text-base"] = "14px";
    out["--text-lg"] = "17px";
    out["--disabled-opacity"] = "0.6";
    char rad[16];
    std::snprintf(rad, sizeof(rad), "%dpx", d.card_radius);
    out["--radius"] = rad;
    out["--radius-lg"] = rad;
    std::snprintf(rad, sizeof(rad), "%dpx", d.btn_radius);
    out["--radius-btn"] = rad;
    std::snprintf(rad, sizeof(rad), "%dpx", d.in_radius);
    out["--radius-field"] = rad;
    out["--radius-sm"] = "2px";
    out["--radius-md"] = "4px";

    /* ---- defaults (fluent baseline, overridden per style below) ---- */
    out["--bg"] = dark ? "#202020" : "#f5f5f5";
    out["--fg"] = dark ? "#f5f5f5" : "#1a1a1a";
    out["--heading"] = out["--fg"];
    out["--card"] = dark ? "#2d2d2d" : "#ffffff";
    out["--field"] = dark ? "#262626" : "#ffffff";
    out["--panel"] = dark ? "#2b2b2b" : "#f0f0f0";
    out["--sidebar"] = dark ? "#252526" : "#f3f3f3";
    out["--border"] = dark ? "#3d3d3d" : "#d4d4d4";
    out["--border-strong"] = dark ? "#5a5a5a" : "#a0a0a0";
    out["--muted"] = dark ? "#9f9f9f" : "#767676";
    out["--hover"] = dark ? "rgba(255,255,255,0.08)" : "rgba(0,0,0,0.06)";
    out["--active"] = dark ? "rgba(255,255,255,0.14)" : "rgba(0,0,0,0.12)";
    out["--selection-bg"] = "#0067c0";
    out["--selection-fg"] = "#ffffff";
    out["--link"] = "#0067c0";
    out["--link-hover"] = "#00529b";
    out["--danger"] = dark ? "#ff6b6b" : "#c42b1c";
    out["--warning"] = dark ? "#f2c94c" : "#a15c00";
    out["--success"] = dark ? "#57ab5a" : "#107c10";
    out["--focus-ring"] = "rgba(0,120,212,0.5)";
    default_shadows(out, dark);

    const char* style = d.name;

    if (std::strcmp(style, "browser") == 0) {
        out["--bg"] = dark ? "#1e1e1e" : "#ffffff";
        out["--fg"] = dark ? "#e6e6e6" : "#1a1a1a";
        out["--card"] = dark ? "#252526" : "#ffffff";
        out["--field"] = dark ? "#252526" : "#ffffff";
        out["--border"] = dark ? "#4a4a4a" : "#dadce0";
        out["--border-strong"] = dark ? "#666666" : "#b9bdc4";
        out["--muted"] = dark ? "#9d9d9d" : "#5f6368";
        out["--selection-bg"] = dark ? "#264f78" : "#0066cc";
        out["--selection-fg"] = "#ffffff";
        out["--link"] = dark ? "#4d94ff" : "#0066cc";
        out["--link-hover"] = dark ? "#7ab0ff" : "#0052a3";
        out["--btn-bg"] = dark ? "#3a3a3a" : "#f1f1f1";
        out["--btn-fg"] = dark ? "#e6e6e6" : "#1a1a1a";
        out["--btn-border"] = dark ? "#666666" : "#dadce0";
        out["--btn-bg-hover"] = dark ? "#454545" : "#ebebeb";
        out["--btn-bg-active"] = dark ? "#505050" : "#e0e0e0";
        out["--accent"] = accent && *accent ? accent : (dark ? "#4d94ff" : "#0066cc");
        out["--accent-fg"] = "#ffffff";
        out["--accent-hover"] = accent && *accent ? accent : (dark ? "#5ea2ff" : "#0073e6");
        out["--accent-active"] = accent && *accent ? accent : (dark ? "#3d82e6" : "#0057b3");
        default_shadows(out, dark);
    } else if (std::strcmp(style, "metro") == 0) {
        /* Fabric Core 9.6.1：neutralPrimary #333 / themePrimary #0078d4 */
        out["--bg"] = dark ? "#1b1a19" : "#ffffff";
        out["--fg"] = dark ? "#f3f2f1" : "#333333";
        out["--card"] = dark ? "#1b1a19" : "#ffffff";
        out["--field"] = dark ? "#1b1a19" : "#ffffff";
        out["--panel"] = dark ? "#292827" : "#f8f8f8";
        out["--sidebar"] = dark ? "#292827" : "#f8f8f8";
        out["--border"] = dark ? "#484644" : "#eaeaea";
        out["--border-strong"] = dark ? "#666666" : "#c8c8c8";
        out["--muted"] = dark ? "#a19f9d" : "#767676";
        out["--hover"] = dark ? "rgba(255,255,255,0.08)" : "rgba(0,0,0,0.06)";
        out["--active"] = dark ? "rgba(255,255,255,0.14)" : "rgba(0,0,0,0.12)";
        out["--selection-bg"] = dark ? "#4cc2ff" : "#0078d4";
        out["--selection-fg"] = dark ? "#000000" : "#ffffff";
        out["--link"] = dark ? "#4cc2ff" : "#0078d4";
        out["--link-hover"] = dark ? "#80d3ff" : "#106ebe";
        out["--accent"] = accent && *accent ? accent : (dark ? "#4cc2ff" : "#0078d4");
        out["--accent-fg"] = dark ? "#1b1a19" : "#ffffff";
        out["--accent-hover"] = accent && *accent ? accent : (dark ? "#6ecbff" : "#106ebe");
        out["--accent-active"] = accent && *accent ? accent : (dark ? "#2ba0e8" : "#005a9e");
        out["--btn-bg"] = out["--accent"];
        out["--btn-fg"] = out["--accent-fg"];
        out["--btn-bg-hover"] = out["--accent-hover"];
        out["--btn-bg-active"] = out["--accent-active"];
        out["--shadow-2xs"] = "none";
        out["--shadow-xs"] = "none";
        out["--shadow-sm"] = "none";
        out["--shadow"] = "none";
        out["--shadow-md"] = "none";
        out["--shadow-lg"] = "none";
        out["--shadow-xl"] = "none";
    } else if (std::strcmp(style, "material") == 0) {
        /* Material Design 3 tokens（mdui：m3.material.io） */
        out["--bg"] = dark ? "#141218" : "#fef7ff";
        out["--fg"] = dark ? "#e6e1e5" : "#1c1b1f";
        out["--card"] = dark ? "#211f26" : "#f3edf7";
        out["--field"] = dark ? "#15131a" : "#ffffff";
        out["--panel"] = dark ? "#1a181f" : "#f7f2fa";
        out["--sidebar"] = dark ? "#1a181f" : "#f7f2fa";
        out["--border"] = dark ? "#49454f" : "#c4c7c5";
        out["--border-strong"] = dark ? "#938f99" : "#79747e";
        out["--muted"] = dark ? "#cac4d0" : "#49454f";
        out["--selection-bg"] = dark ? "#d0bcff" : "#6750a4";
        out["--selection-fg"] = dark ? "#381e72" : "#ffffff";
        out["--link"] = dark ? "#d0bcff" : "#6750a4";
        out["--link-hover"] = dark ? "#e8def8" : "#7a62b4";
        out["--accent"] = accent && *accent ? accent : (dark ? "#d0bcff" : "#6750a4");
        out["--accent-fg"] = dark ? "#381e72" : "#ffffff";
        out["--accent-hover"] = accent && *accent ? accent : (dark ? "#e8def8" : "#7a62b4");
        out["--accent-active"] = accent && *accent ? accent : (dark ? "#c2b0e0" : "#5a4a92");
        out["--btn-bg"] = out["--accent"];
        out["--btn-fg"] = out["--accent-fg"];
        out["--btn-bg-hover"] = out["--accent-hover"];
        out["--btn-bg-active"] = out["--accent-active"];
        out["--btn-border"] = "transparent";
        out["--shadow-2xs"] = dark ? "0 1px 2px rgba(0,0,0,0.3)" : "0 1px 2px rgba(0,0,0,0.15)";
        out["--shadow-xs"] = dark ? "0 1px 3px rgba(0,0,0,0.35)" : "0 1px 3px rgba(0,0,0,0.18)";
        out["--shadow-sm"] = dark ? "0 2px 4px rgba(0,0,0,0.4)" : "0 2px 4px rgba(0,0,0,0.2)";
        out["--shadow"] = dark ? "0 4px 8px rgba(0,0,0,0.45)" : "0 4px 8px rgba(0,0,0,0.22)";
        out["--shadow-md"] = dark ? "0 6px 12px rgba(0,0,0,0.5)" : "0 6px 12px rgba(0,0,0,0.24)";
        out["--shadow-lg"] = dark ? "0 10px 24px rgba(0,0,0,0.55)" : "0 10px 24px rgba(0,0,0,0.28)";
        out["--shadow-xl"] = dark ? "0 16px 40px rgba(0,0,0,0.6)" : "0 16px 40px rgba(0,0,0,0.32)";
    } else if (std::strcmp(style, "classic") == 0) {
        out["--bg"] = dark ? "#2b2b2b" : "#ece9d8";
        out["--fg"] = dark ? "#e0e0e0" : "#000000";
        out["--card"] = dark ? "#3a3a3a" : "#ffffff";
        out["--field"] = dark ? "#3a3a3a" : "#ffffff";
        out["--panel"] = dark ? "#333333" : "#d4d0c8";
        out["--sidebar"] = dark ? "#333333" : "#d4d0c8";
        out["--border"] = dark ? "#5a5a5a" : "#aca899";
        out["--border-strong"] = dark ? "#6a6a6a" : "#8a8778";
        out["--muted"] = dark ? "#b0b0b0" : "#404040";
        out["--selection-bg"] = dark ? "#003366" : "#000080";
        out["--selection-fg"] = "#ffffff";
        out["--link"] = dark ? "#6699ff" : "#0000ee";
        out["--link-hover"] = dark ? "#88aaff" : "#551a8b";
        out["--accent"] = accent && *accent ? accent : "#000080";
        out["--accent-fg"] = "#ffffff";
        out["--accent-hover"] = accent && *accent ? accent : "#0000a0";
        out["--accent-active"] = accent && *accent ? accent : "#000060";
        out["--btn-bg"] = dark ? "#3d3d3d" : "#d4d0c8";
        out["--btn-fg"] = dark ? "#e0e0e0" : "#000000";
        out["--btn-border"] = dark ? "#5a5a5a" : "#8a8678";
        out["--btn-bg-hover"] = dark ? "#4a4a4a" : "#e0ddd5";
        out["--btn-bg-active"] = dark ? "#333333" : "#c8c4ba";
        out["--shadow-2xs"] = "none";
        out["--shadow-xs"] = "none";
        out["--shadow-sm"] = "none";
        out["--shadow"] = "none";
        out["--shadow-md"] = "none";
        out["--shadow-lg"] = "none";
        out["--shadow-xl"] = "none";
    } else if (std::strcmp(style, "aero") == 0) {
        /* Win7：7.css 元素色（w7-el-bd #8e8f8f，按钮三段灰渐变） */
        out["--bg"] = dark ? "#1b1b1b" : "#f0f0f0";
        out["--fg"] = dark ? "#e0e8f0" : "#1a1a1a";
        out["--card"] = dark ? "#26282c" : "#ffffff";
        out["--field"] = dark ? "#20242a" : "#ffffff";
        out["--panel"] = dark ? "#2a2d33" : "#e8eef5";
        out["--sidebar"] = dark ? "#2a2d33" : "#e8eef5";
        out["--border"] = dark ? "#3d444e" : "#8e8f8f";
        out["--border-strong"] = dark ? "#4d5663" : "#6d91ab";
        out["--muted"] = dark ? "#93a1b0" : "#5f7a92";
        out["--selection-bg"] = dark ? "#3c7fb1" : "#0078d7";
        out["--selection-fg"] = "#ffffff";
        out["--link"] = dark ? "#4fc1ff" : "#0078d7";
        out["--link-hover"] = dark ? "#80d3ff" : "#005a9e";
        out["--accent"] = accent && *accent ? accent : (dark ? "#4fc1ff" : "#0078d7");
        out["--accent-fg"] = "#ffffff";
        out["--accent-hover"] = accent && *accent ? accent : (dark ? "#6ecbff" : "#106ebe");
        out["--accent-active"] = accent && *accent ? accent : (dark ? "#2ba0e8" : "#005a9e");
        /* 7.css --w7-el-grad: #f2f2f2 45% / #ebebeb 45% / #cfcfcf */
        out["--btn-bg"] = dark ? "#3a3a3a" : "#f2f2f2";
        out["--btn-bg-s1"] = dark ? "#3a3a3a" : "#ebebeb";
        out["--btn-bg-s2"] = dark ? "#303030" : "#cfcfcf";
        out["--btn-fg"] = dark ? "#e0e0e0" : "#222222";
        out["--btn-border"] = dark ? "#555555" : "#8e8f8f";
        out["--btn-bg-hover"] = dark ? "#3d444e" : "#eaf6fd";
        out["--btn-bg-active"] = dark ? "#2a2d33" : "#d0d0d0";
        out["--shadow-2xs"] = "0 1px 2px rgba(0,0,0,0.15)";
        out["--shadow-xs"] = "0 1px 3px rgba(0,0,0,0.18)";
        out["--shadow-sm"] = "0 1px 4px rgba(0,0,0,0.2)";
        out["--shadow"] = "0 2px 6px rgba(0,0,0,0.22)";
        out["--shadow-md"] = "0 4px 10px rgba(0,0,0,0.24)";
        out["--shadow-lg"] = "0 6px 16px rgba(0,0,0,0.26)";
        out["--shadow-xl"] = "0 10px 28px rgba(0,0,0,0.3)";
    } else if (std::strcmp(style, "gtk") == 0) {
        /* GNOME Adwaita（adwaita-web） */
        out["--bg"] = dark ? "#242424" : "#fafafa";
        out["--fg"] = dark ? "#ffffff" : "#1b1b1b";
        out["--card"] = dark ? "#1e1e1e" : "#ffffff";
        out["--field"] = dark ? "#1e1e1e" : "#ffffff";
        out["--panel"] = dark ? "#303030" : "#ffffff";
        out["--sidebar"] = dark ? "#282828" : "#ebebeb";
        out["--border"] = dark ? "#4a4a4a" : "#deddda";
        out["--border-strong"] = dark ? "#5e5e5e" : "#c0bfbc";
        out["--muted"] = dark ? "#9a9996" : "#5e5c64";
        out["--selection-bg"] = dark ? "#3584e4" : "#3584e4";
        out["--selection-fg"] = "#ffffff";
        out["--link"] = dark ? "#78aeed" : "#1c71d8";
        out["--link-hover"] = dark ? "#9ec2f2" : "#0461be";
        out["--accent"] = accent && *accent ? accent : "#3584e4";
        out["--accent-fg"] = "#ffffff";
        out["--accent-hover"] = accent && *accent ? accent : "#5a9cf0";
        out["--accent-active"] = accent && *accent ? accent : "#2a6fd0";
        out["--btn-bg"] = dark ? "#3d3d3d" : "#ffffff";
        out["--btn-fg"] = dark ? "#ffffff" : "#1b1b1b";
        out["--btn-border"] = dark ? "#555555" : "#c0bfbc";
        out["--btn-bg-hover"] = dark ? "#4a4a4a" : "#f0efec";
        out["--btn-bg-active"] = dark ? "#565656" : "#e0dfdc";
        out["--focus-ring"] = "rgba(53,132,228,0.4)";
        default_shadows(out, dark);
    } else if (std::strcmp(style, "macos") == 0) {
        /* macOS（photon） */
        out["--bg"] = dark ? "#1e1e1e" : "#ececec";
        out["--fg"] = dark ? "#f0f0f0" : "#333333";
        out["--card"] = dark ? "#2c2c2c" : "#ffffff";
        out["--field"] = dark ? "#2c2c2c" : "#ffffff";
        out["--panel"] = dark ? "#333333" : "#e8e6e8";
        out["--sidebar"] = dark ? "#333333" : "#f5f5f4";
        out["--border"] = dark ? "#3f3f3f" : "#d0d0d0";
        out["--border-strong"] = dark ? "#5c5c5c" : "#a8a8a8";
        out["--muted"] = dark ? "#9c9c9c" : "#808080";
        out["--selection-bg"] = dark ? "#0a84ff" : "#007aff";
        out["--selection-fg"] = "#ffffff";
        out["--link"] = dark ? "#4da3ff" : "#007aff";
        out["--link-hover"] = dark ? "#7abaff" : "#0066d6";
        out["--accent"] = accent && *accent ? accent : (dark ? "#0a84ff" : "#007aff");
        out["--accent-fg"] = "#ffffff";
        out["--accent-hover"] = accent && *accent ? accent : (dark ? "#2e95ff" : "#1a86ff");
        out["--accent-active"] = accent && *accent ? accent : (dark ? "#0070e0" : "#0066d6");
        out["--btn-bg"] = dark ? "#4a4a4a" : "#fcfcfc";
        out["--btn-fg"] = dark ? "#f0f0f0" : "#333333";
        out["--btn-border"] = dark ? "#555555" : "#c2c0c2";
        out["--btn-bg-hover"] = dark ? "#555555" : "#f1f1f1";
        out["--btn-bg-active"] = dark ? "#3f3f3f" : "#e1e1e1";
        default_shadows(out, dark);
    } else { /* fluent：fluent-css neutral 色板 */
        out["--bg"] = dark ? "#0f0f0f" : "#f5f5f5";
        out["--fg"] = dark ? "#d4d4d4" : "#262626";
        out["--heading"] = dark ? "#ffffff" : "#171717";
        out["--card"] = dark ? "#1f1f1f" : "#ffffff";
        out["--field"] = dark ? "#1f1f1f" : "#ffffff";
        out["--panel"] = dark ? "#1a1a1a" : "#f0f0f0";
        out["--sidebar"] = dark ? "#1a1a1a" : "#f0f0f0";
        out["--border"] = dark ? "#404040" : "#d4d4d4";
        out["--border-strong"] = dark ? "#737373" : "#a3a3a3";
        out["--muted"] = dark ? "#737373" : "#525252";
        out["--hover"] = dark ? "rgba(255,255,255,0.08)" : "rgba(0,0,0,0.06)";
        out["--active"] = dark ? "rgba(255,255,255,0.14)" : "rgba(0,0,0,0.12)";
        out["--selection-bg"] = dark ? "#4cc2ff" : "#0067c0";
        out["--selection-fg"] = "#ffffff";
        out["--link"] = dark ? "#4cc2ff" : "#0067c0";
        out["--link-hover"] = dark ? "#80d3ff" : "#004e8c";
        out["--accent"] = accent && *accent ? accent : (dark ? "#4cc2ff" : "#0067c0");
        out["--accent-fg"] = "#ffffff";
        out["--accent-hover"] = accent && *accent ? accent : (dark ? "#6ecbff" : "#106ebe");
        out["--accent-active"] = accent && *accent ? accent : (dark ? "#2ba0e8" : "#005a9e");
        out["--btn-bg"] = out["--card"];
        out["--btn-fg"] = out["--fg"];
        out["--btn-border"] = out["--border"];
        out["--btn-bg-hover"] = dark ? "#2d2d2d" : "#f0f0f0";
        out["--btn-bg-active"] = dark ? "#3a3a3a" : "#e5e5e5";
        out["--focus-ring"] = "rgba(0,103,192,0.5)";
        default_shadows(out, dark);
    }
}

} // namespace

extern "C" int whaleui_theme_count(void)
{
    return kThemeCount;
}

extern "C" const char* whaleui_theme_name(int i)
{
    if (i < 0 || i >= kThemeCount) {
        i = 0;
    }
    return kThemes[i].name;
}

extern "C" const char* whaleui_theme_label(int i)
{
    if (i < 0 || i >= kThemeCount) {
        i = 0;
    }
    return kThemes[i].label;
}

extern "C" const char* whaleui_theme_resolve(const char* name)
{
    if (!name) {
        return kThemes[0].name;
    }
    return find_def(name)->name;
}

extern "C" const char* whaleui_theme_default_css(const char* style)
{
    static std::string buf;
    buf.clear();
    build_css(*find_def(style), buf);
    return buf.c_str();
}

extern "C" void whaleui_theme_vars(const char* style, whaleui_theme_t theme,
                                   const char* accent,
                                   std::map<std::string, std::string>& out)
{
    bool dark = theme == WHALEUI_THEME_DARK;
    fill_vars(*find_def(style), dark, accent, out);
}
