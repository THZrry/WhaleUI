/* Theme styles: browser / Fluent / Metro / Classic / Aero / Material / GTK /
 * macOS. Each style = shared UA baseline + shared component classes +
 * per-style control theme, plus a light/dark variable table.
 *
 * The stylesheets below are TRANSLATIONS of existing, verified design
 * systems into this engine's CSS subset (no outline / :has / :checked /
 * multi-shadow / >2-stop GPU gradients). Reference sources, marked inline:
 *   - browser : hand-written UA default stylesheet (WHATWG HTML standard)
 *   - fluent  : https://github.com/aipx-proto/fluent-css
 *   - metro   : Office UI Fabric Core 9.6.1
 *               (static2.sharepointonline.com/files/fabric/.../fabric.min.css)
 *   - classic : hand-written Windows Classic translation
 *   - aero    : https://github.com/khang-nd/7.css
 *   - material: https://github.com/zdhxiong/mdui (Material Design 3 tokens)
 *   - gtk     : https://github.com/mclellac/adwaita-web
 *   - macos   : https://github.com/connors/photon
 *
 * Themes are global: every element that uses var(--*) or inherits body
 * color/font follows the current style, including tags the user's HTML does
 * not style. --accent / --field / --border / --card are also read by the
 * renderer for the native checkbox/radio/progress/select-popup/scrollbar
 * drawing. */

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
    {"classic",  "Windows Classic",  3,   1,   0},
    {"aero",     "Windows 7 Aero",   3,   2,   4},
    {"gtk",      "GTK (Adwaita)",    6,   6,   6},
    {"macos",    "macOS",            4,   4,   4},
    {"browser",  "Browser (UA)",     3,   3,   4},
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
 * 1. UA baseline - every tag gets a default (WHATWG HTML UA stylesheet
 * translated for this engine). Shared by ALL themes; a theme overrides
 * the parts it wants with later rules of equal specificity.
 * =================================================================== */
const char* kUaCss = R"(
/* --- UA 默认样式：每个 tag 一个默认外观（WHATWG HTML 标准） --- */
/* 不可渲染的元数据 / 媒体来源：display:none */
head, style, script, title, meta, link, base, template, noscript,
source, track, param, area, wbr { display: none; }
/* 块级：div 及其后代标签默认 block */
div, section, article, nav, aside, header, footer, main, address,
p, hr, pre, blockquote, ol, ul, menu, li, dl, dt, dd, figure,
figcaption, caption, colgroup, col, form, fieldset, details,
dialog, picture, map, iframe, embed, object, video, audio, canvas {
    display: block;
}
/* 表格映射到网格引擎：table 是二维网格，行/单元格不能是 block */
table { display: table; border-collapse: collapse; }
thead, tbody, tfoot { display: table-row-group; }
tr { display: table-row; }
td, th { display: table-cell; }
/* 行内：文字流中的标签保持 inline */
a, em, strong, small, s, cite, q, dfn, abbr, data, time, code, var,
samp, kbd, sub, sup, i, b, u, mark, ruby, rt, rp, bdi, bdo, span,
label { display: inline; }
/* 行内块：表单控件 */
img, input, button, select, textarea, output, progress, meter,
datalist { display: inline-block; }
/* 页面与正文 */
html { background-color: var(--bg); }
body {
    background-color: var(--bg); color: var(--fg);
    font-family: var(--font-sans); font-size: var(--text-base);
    line-height: 1.5;
}
/* 标题：h1 最大逐级递减，加粗（浏览器 UA 比例，主题可覆盖） */
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
code, kbd, samp, pre {
    font-family: var(--font-mono); font-size: 0.9em;
}
kbd {
    border: 1px solid var(--border); border-radius: 3px;
    padding: 1px 4px; background: var(--card);
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
option { display: none; } /* option 活在 select 弹出层里 */
hr { border: none; border-top: 1px solid var(--border); margin: 0.9em 0; }
img { max-width: 100%; }
table { width: auto; }
td, th { padding: 4px 8px; border: 1px solid var(--border); }
th { font-weight: 600; background: var(--card); }
caption { caption-side: top; padding: 4px; color: var(--muted); font-size: 12px; }
/* 文本选择：全局 ::selection 高亮（渲染器读 selection-bg） */
::selection { background: var(--selection-bg); color: var(--selection-fg); }
)";

/* =====================================================================
 * 2. Component classes + keyframes - shared by ALL themes. These are
 * opt-in extensions (class="card", class="btn-primary", ...) layered on
 * top of the UA defaults. Class selectors out-rank tag selectors, so a
 * theme's tag rules never fight them.
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
/* --- 卡片（浮动层 / 面板底色） --- */
.card {
    background: var(--card); border: 1px solid var(--border);
    border-radius: var(--radius-lg); padding: 16px 18px;
    box-shadow: var(--shadow);
    transition: transform 0.18s;
}
.card:hover { transform: translateY(-2px); }
/* --- 按钮变体（默认按钮外观由主题定义） --- */
.btn-primary { background: var(--btn-bg); color: var(--btn-fg); border-color: transparent; }
.btn-primary:hover { background: var(--btn-bg-hover); }
.btn-primary:active { background: var(--btn-bg-active); }
.btn-secondary { background: transparent; color: var(--accent); }
.btn-secondary:hover { background: var(--hover); }
.btn-secondary:active { background: var(--active); }
.btn-subtle { background: transparent; border-color: transparent; color: var(--fg); }
.btn-subtle:hover { background: var(--hover); }
.btn-subtle:active { background: var(--active); }
.btn-outline { background: transparent; }
.btn-outline:hover { background: var(--hover); }
.btn-danger { background: var(--danger); color: #ffffff; border-color: transparent; }
.btn-danger:hover { background: var(--btn-bg-active); }
/* --- 输入框变体 --- */
.input { background: var(--field); color: var(--fg); border: 1px solid var(--border); }
.input:focus { border-color: var(--accent); }
/* --- 文本工具 --- */
.muted { color: var(--muted); }
.text-left { text-align: left; }
.text-center { text-align: center; }
.text-right { text-align: right; }
.small { font-size: 12px; }
.hidden { display: none; }
.disabled { opacity: var(--disabled-opacity); }
/* --- 徽章 / 标签 --- */
.badge {
    display: inline-block; padding: 2px 10px;
    background: var(--accent); color: var(--accent-fg);
    border-radius: 999px; font-size: 11px; font-weight: 600;
}
/* --- 标签页（Fluent 风格下划线，theme: .tabs > .tab.selected） --- */
.tabs { display: flex; flex-direction: row; gap: 4px; border-bottom: 1px solid var(--border); }
.tab {
    padding: 6px 12px; cursor: pointer; background: transparent;
    border: none; border-radius: 0; color: var(--fg); font-weight: 400;
    transition: all 0.12s;
}
.tab:hover { background: var(--hover); }
.tab.selected { color: var(--heading); font-weight: 600; box-shadow: inset 0 -2px 0 var(--accent); }
/* --- 工具条 / 窗格（macOS photon / GTK 布局） --- */
.toolbar {
    display: flex; align-items: center; gap: 8px; padding: 6px 10px;
    background: var(--panel); border-bottom: 1px solid var(--border);
}
.pane { flex: 1; overflow: auto; border-left: 1px solid var(--border); padding: 12px; }
.pane:first-child { border-left: none; }
.sidebar { background: var(--sidebar); border-right: 1px solid var(--border); }
/* --- 尺寸（Fluent .size-sm/.size-lg 等比例） --- */
.size-sm { font-size: var(--text-sm); }
.size-lg { font-size: var(--text-lg); }
/* --- 动画 keyframes + 工具类 --- */
@keyframes wui-fade-in { from { opacity: 0; } to { opacity: 1; } }
@keyframes wui-rise {
    from { opacity: 0; transform: translateY(12px); }
    to   { opacity: 1; transform: none; }
}
@keyframes wui-pulse {
    0% { opacity: 0.4; }
    50% { opacity: 1; }
    100% { opacity: 0.4; }
}
@keyframes wui-spin {
    from { transform: rotate(0deg); }
    to   { transform: rotate(360deg); }
}
.anim-fade-in { animation: wui-fade-in 0.3s ease-out both; }
.anim-rise    { animation: wui-rise 0.4s ease-out both; }
.anim-pulse   { animation: wui-pulse 1.6s ease-in-out infinite; }
.anim-spin    { animation: wui-spin 1.2s linear infinite; }
)";

/* =====================================================================
 * 3. Per-theme control themes (override the UA defaults for the parts
 * that make each system recognizable: buttons, inputs, links, focus).
 * =================================================================== */

/* --- browser: 仿浏览器默认 UA 控件（Chrome/Firefox 观感，手写） --- */
const char* kBrowserCss = R"(
/* browser 主题：浏览器默认控件（来源：WHATWG UA 样式翻译，手写） */
a { color: var(--link); }
a:hover { text-decoration: underline; }
button {
    background: var(--btn-bg); color: var(--btn-fg);
    border: 1px solid var(--btn-border); border-radius: 3px;
    padding: 4px 14px; font-size: 13px; cursor: pointer;
    transition: background-color 0.12s, border-color 0.12s;
}
button:hover { background: var(--btn-bg-hover); border-color: var(--border-strong); }
button:active { background: var(--btn-bg-active); }
button:disabled { opacity: var(--disabled-opacity); }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--border); border-radius: 3px;
    padding: 4px 8px; font-size: 13px;
    transition: border-color 0.12s, background-color 0.12s;
}
input:hover, select:hover, textarea:hover { border-color: var(--border-strong); }
input:focus, select:focus, textarea:focus { border-color: var(--accent); }
textarea { overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 14px; height: 14px; }
progress { width: 160px; height: 12px; }
)";

/* --- fluent: Win11 Fluent (fluent-css) --- */
const char* kFluentCss = R"(
/* fluent 主题（来源：github.com/aipx-proto/fluent-css） */
a { color: var(--link); text-decoration: none; transition: color 0.1s; }
a:hover { text-decoration: underline; }
button {
    background: var(--btn-bg); color: var(--btn-fg);
    border: 1px solid var(--btn-border); border-radius: var(--radius-btn);
    min-height: 32px; padding: 0 14px; font-size: var(--text-base);
    font-weight: 600; cursor: pointer; line-height: 1;
    transition: background-color 0.1s, border-color 0.1s;
}
button:hover { background: var(--btn-bg-hover); }
button:active { background: var(--btn-bg-active); }
button:focus { box-shadow: inset 0 0 0 1px var(--accent); }
button:disabled { opacity: var(--disabled-opacity); }
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
}
textarea { padding: 6px 10px; overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 16px; height: 16px; }
progress { width: 100%; height: 4px; border-radius: 2px; }
)";

/* --- metro: Win8 Metro 扁平（Office UI Fabric Core 9.6.1 色板） --- */
const char* kMetroCss = R"(
/* metro 主题：扁平、无圆角无阴影、hover 半透明块（来源：Office UI
 * Fabric Core 9.6.1 色板 + Metro 设计语言） */
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
.btn-primary { background: var(--btn-bg); color: var(--btn-fg); }
.btn-primary:hover { background: var(--btn-bg-hover); }
.btn-primary:active { background: var(--btn-bg-active); }
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

/* --- classic: Windows Classic 3D 控件（手写翻译） --- */
const char* kClassicCss = R"(
/* classic 主题：Win95/98/2000 3D 斜面控件（手写翻译；引擎无四边异色
 * 边框/多阴影，用两段渐变模拟斜角高光） */
a { color: var(--link); text-decoration: underline; }
button {
    background: linear-gradient(to bottom, #ffffff, var(--btn-bg));
    color: var(--btn-fg); border: 1px solid var(--btn-border);
    border-radius: 3px; padding: 3px 12px; font-size: 13px;
    font-weight: normal; cursor: pointer;
    transition: background 0.1s;
}
button:hover { background: linear-gradient(to bottom, #ffffcc, var(--btn-bg)); }
button:active { background: linear-gradient(to bottom, var(--btn-bg), #ffffff); }
button:disabled { opacity: var(--disabled-opacity); }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--btn-border); border-radius: 0;
    padding: 2px 6px; font-size: 13px;
}
input:focus, select:focus, textarea:focus { background: #ffffff; }
textarea { overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 13px; height: 13px; }
progress { width: 160px; height: 14px; border: 1px solid var(--btn-border); }
)";

/* --- aero: Win7 Aero 玻璃感渐变控件（7.css） --- */
const char* kAeroCss = R"(
/* aero 主题（来源：github.com/khang-nd/7.css；7.css 用 ::before/::after
 * 覆盖层实现 hover/active 渐变，引擎支持伪元素 + opacity transition，
 * 此处直接翻译为伪元素覆盖层方案） */
a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }
button {
    background: var(--btn-bg); color: var(--btn-fg);
    border: 1px solid var(--btn-border); border-radius: var(--radius-btn);
    box-shadow: inset 0 1px 0 rgba(255,255,255,0.7);
    min-height: 26px; padding: 2px 12px; font-size: 13px; cursor: pointer;
    position: relative; transition: border-color 0.3s;
}
/* hover/active 渐变覆盖层：7.css --w7-el-grad-h / --w7-el-grad-a */
button::before {
    content: ""; position: absolute; top: 0; left: 0;
    width: 100%; height: 100%; border-radius: var(--radius-btn);
    background: linear-gradient(to bottom, #eaf6fd, #a7d9f5);
    opacity: 0; transition: opacity 0.3s;
}
button::after {
    content: ""; position: absolute; top: 0; left: 0;
    width: 100%; height: 100%;
    background: linear-gradient(to bottom, #e5f4fc, #68b3db);
    opacity: 0; transition: opacity 0.3s;
}
button:hover { border-color: #3c7fb1; }
button:hover::before { opacity: 1; }
button:active { border-color: #6d91ab; }
button:active::after { opacity: 1; }
button:disabled { opacity: var(--disabled-opacity); }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--border); border-radius: var(--radius-btn);
    padding: 3px 6px; font-size: 13px;
    transition: border-color 0.3s;
}
input:hover, select:hover, textarea:hover { border-color: var(--border-strong); }
input:focus, select:focus, textarea:focus { border-color: var(--accent); }
textarea { overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 14px; height: 14px; }
progress { width: 160px; height: 14px; border-radius: 2px; }
)";

/* --- material: Material Design 3（mdui tokens） --- */
const char* kMaterialCss = R"(
/* material 主题（来源：github.com/zdhxiong/mdui，Material 3 tokens；
 * 主按钮胶囊形 + elevation 阴影 + 输入框 focus 强调下边框） */
a { color: var(--link); text-decoration: none; transition: color 0.15s; }
a:hover { text-decoration: underline; }
button {
    background: var(--btn-bg); color: var(--btn-fg);
    border: none; border-radius: var(--radius-btn);
    min-height: 36px; padding: 0 22px; font-size: var(--text-base);
    font-weight: 600; cursor: pointer; text-transform: none;
    box-shadow: var(--shadow-sm);
    transition: all 0.15s;
}
button:hover { background: var(--btn-bg-hover); box-shadow: var(--shadow); }
button:active { background: var(--btn-bg-active); box-shadow: none; }
button:disabled { opacity: var(--disabled-opacity); box-shadow: none; }
.btn-secondary, .btn-subtle { box-shadow: none; }
.btn-secondary:hover, .btn-subtle:hover { box-shadow: none; }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--border); border-radius: var(--radius-btn);
    min-height: 36px; padding: 0 12px; font-size: var(--text-base);
    transition: border-color 0.15s, box-shadow 0.15s;
}
input:focus, select:focus, textarea:focus {
    border-color: var(--accent);
    box-shadow: inset 0 -1px 0 var(--accent);
}
textarea { padding: 8px 12px; overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 18px; height: 18px; }
progress { width: 100%; height: 4px; border-radius: 2px; }
)";

/* --- gtk: GNOME Adwaita（adwaita-web） --- */
const char* kGtkCss = R"(
/* gtk 主题（来源：github.com/mclellac/adwaita-web；Adwaita 按钮 view
 * 底色 + 边框，输入框 inset 阴影 + focus ring，禁用用整体透明度） */
a { color: var(--link); text-decoration: none; }
a:hover { color: var(--link-hover); text-decoration: underline; }
button {
    background: var(--btn-bg); color: var(--btn-fg);
    border: 1px solid var(--btn-border); border-radius: var(--radius-btn);
    padding: 6px 14px; font-size: var(--text-base); font-weight: 600;
    cursor: pointer;
    transition: background-color 0.15s, border-color 0.15s;
}
button:hover { background: var(--btn-bg-hover); }
button:active { background: var(--btn-bg-active); }
button:focus { box-shadow: 0 0 0 2px var(--focus-ring); }
button:disabled { opacity: var(--disabled-opacity); }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--border); border-radius: var(--radius-btn);
    padding: 6px 10px; font-size: var(--text-base);
    box-shadow: inset 0 1px 1px rgba(0,0,0,0.06);
    transition: border-color 0.1s;
}
input:focus, select:focus, textarea:focus {
    border-color: var(--accent);
    box-shadow: inset 0 1px 1px rgba(0,0,0,0.06), 0 0 0 2px var(--focus-ring);
}
textarea { overflow: auto; }
select { cursor: pointer; }
input[type="checkbox"], input[type="radio"] { width: 16px; height: 16px; }
progress { width: 100%; height: 6px; border-radius: 3px; }
)";

/* --- macos: macOS（photon） --- */
const char* kMacosCss = R"(
/* macos 主题（来源：github.com/connors/photon；渐变按钮 + active 变平，
 * 输入框 focus 用外发光环 —— 引擎单阴影近似） */
a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }
button {
    background: linear-gradient(to bottom, var(--btn-bg), var(--btn-bg-active));
    color: var(--btn-fg); border: 1px solid var(--btn-border);
    border-radius: var(--radius-btn); padding: 3px 12px;
    font-size: var(--text-sm); cursor: pointer; line-height: 1.4;
    box-shadow: 0 1px 1px rgba(0,0,0,0.06);
    transition: background 0.1s;
}
button:hover { background: linear-gradient(to bottom, #fcfcfc, #e4e4e4); }
button:active { background: var(--btn-bg-active); box-shadow: none; }
button:disabled { opacity: var(--disabled-opacity); }
.btn-primary { background: linear-gradient(to bottom, #6eb4f7, #1a82fb); color: #ffffff; border-color: #388df8; }
.btn-primary:hover { background: linear-gradient(to bottom, #7ec0f8, #2e8cfb); }
.btn-primary:active { background: #3e9bf4; }
input, select, textarea {
    background: var(--field); color: var(--fg);
    border: 1px solid var(--border); border-radius: var(--radius-btn);
    min-height: 25px; padding: 3px 8px; font-size: var(--text-sm);
    line-height: 1.6;
}
input:focus, select:focus, textarea:focus {
    border-color: #6db3fd; box-shadow: 0 0 0 3px rgba(109,179,253,0.4);
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
/* lighten a #RRGGBB color by mixing 25% white (custom-accent hover) */
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

/* darken a #RRGGBB color by 15% (custom-accent pressed state) */
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
    out["--shadow-sm"] = "0 1px 2px rgba(0,0,0,0.1)";
    out["--shadow"] = "0 2px 8px rgba(0,0,0,0.12)";
    out["--shadow-lg"] = "0 8px 24px rgba(0,0,0,0.18)";

    const char* style = d.name;

    if (std::strcmp(style, "browser") == 0) {
        /* 仿浏览器 UA：白底灰控件 */
        out["--bg"] = dark ? "#1e1e1e" : "#ffffff";
        out["--fg"] = dark ? "#e6e6e6" : "#1a1a1a";
        out["--card"] = dark ? "#252526" : "#ffffff";
        out["--field"] = dark ? "#252526" : "#ffffff";
        out["--border"] = dark ? "#4a4a4a" : "#b6b6b6";
        out["--border-strong"] = dark ? "#666666" : "#8a8a8a";
        out["--muted"] = dark ? "#9d9d9d" : "#717171";
        out["--selection-bg"] = dark ? "#264f78" : "#0066cc";
        out["--selection-fg"] = "#ffffff";
        out["--link"] = dark ? "#4d94ff" : "#0066cc";
        out["--link-hover"] = dark ? "#7ab0ff" : "#0052a3";
        out["--btn-bg"] = dark ? "#3a3a3a" : "#f0f0f0";
        out["--btn-fg"] = dark ? "#e6e6e6" : "#1a1a1a";
        out["--btn-border"] = dark ? "#666666" : "#a6a6a6";
        out["--btn-bg-hover"] = dark ? "#454545" : "#e4e4e4";
        out["--btn-bg-active"] = dark ? "#505050" : "#d0d0d0";
        out["--accent"] = accent && *accent ? accent : (dark ? "#4d94ff" : "#0066cc");
        out["--accent-fg"] = "#ffffff";
        out["--accent-hover"] = accent && *accent ? accent : (dark ? "#5ea2ff" : "#0073e6");
        out["--accent-active"] = accent && *accent ? accent : (dark ? "#3d82e6" : "#0057b3");
        out["--shadow"] = "0 1px 3px rgba(0,0,0,0.15)";
        out["--shadow-sm"] = "0 1px 2px rgba(0,0,0,0.1)";
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
        out["--shadow"] = "none";
        out["--shadow-sm"] = "none";
        out["--shadow-lg"] = "none";
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
        out["--shadow-sm"] = dark ? "0 1px 3px rgba(0,0,0,0.4)" : "0 1px 3px rgba(0,0,0,0.2)";
        out["--shadow"] = dark ? "0 4px 12px rgba(0,0,0,0.5)" : "0 4px 12px rgba(0,0,0,0.2)";
        out["--shadow-lg"] = dark ? "0 10px 24px rgba(0,0,0,0.55)" : "0 10px 24px rgba(0,0,0,0.25)";
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
        out["--btn-border"] = dark ? "#5a5a5a" : "#aca899";
        out["--btn-bg-hover"] = dark ? "#4a4a4a" : "#e8e4dc";
        out["--btn-bg-active"] = dark ? "#333333" : "#c0bcb0";
        out["--shadow"] = "none";
        out["--shadow-sm"] = "none";
        out["--shadow-lg"] = "none";
    } else if (std::strcmp(style, "aero") == 0) {
        /* Win7：窗口蓝 + 7.css 元素色（w7-el-bd #8e8f8f） */
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
        out["--btn-bg"] = dark ? "#333333" : "#f2f2f2";
        out["--btn-fg"] = dark ? "#e0e0e0" : "#222222";
        out["--btn-border"] = dark ? "#555555" : "#8e8f8f";
        out["--btn-bg-hover"] = dark ? "#3d444e" : "#eaf6fd";
        out["--btn-bg-active"] = dark ? "#2a2d33" : "#d0d0d0";
        out["--shadow"] = "0 1px 4px rgba(0,0,0,0.2)";
        out["--shadow-sm"] = "0 1px 2px rgba(0,0,0,0.15)";
        out["--shadow-lg"] = "0 8px 20px rgba(0,0,0,0.25)";
    } else if (std::strcmp(style, "gtk") == 0) {
        /* GNOME Adwaita（adwaita-web：window-bg #fafafa / view #fff，
         * accent #3584e4） */
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
        out["--shadow"] = "0 1px 3px rgba(0,0,0,0.12)";
        out["--shadow-sm"] = "0 1px 2px rgba(0,0,0,0.08)";
        out["--shadow-lg"] = "0 8px 20px rgba(0,0,0,0.16)";
    } else if (std::strcmp(style, "macos") == 0) {
        /* macOS（photon：#fff 窗口 / #ececec 桌面，accent #007aff） */
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
        out["--shadow"] = "0 2px 8px rgba(0,0,0,0.12)";
        out["--shadow-sm"] = "0 1px 1px rgba(0,0,0,0.06)";
        out["--shadow-lg"] = "0 8px 24px rgba(0,0,0,0.16)";
    } else { /* fluent：fluent-css neutral 色板（tailwind neutral scale） */
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
        out["--shadow"] = "0 2px 8px rgba(0,0,0,0.12)";
        out["--shadow-sm"] = "0 1px 2px rgba(0,0,0,0.1)";
        out["--shadow-lg"] = "0 8px 24px rgba(0,0,0,0.18)";
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
