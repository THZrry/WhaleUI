# HTML / CSS 特性支持情况(当前实现核对)

> 本文档按当前代码(`src/`)逐项核对,记录 WhaleUI 目前对 HTML 与 CSS 各特性的**实际支持情况**。
> 与 `../internal/css-priority.md`(需求优先级清单)和 `../internal/09-implementation.md`(实现状态)配套阅读:
> 前者是"该做什么",后者是"做到哪一步",本文是"具体支持了什么/没支持什么"。
> 核对基准:lexbor(HTML 解析)、自研 CSS 解析器(`src/style/css.cpp`)、自研布局引擎(`src/layout/`)、CPU/GPU 渲染(`src/render/`)。

---

## 1. HTML 支持情况

### 1.1 解析

| 项目 | 状态 | 说明 |
|------|------|------|
| HTML 解析 | ✅ | 接入 lexbor(HTML5 容错解析),DOM 节点即 lexbor 对象,无包装层 |
| HTML 文件读取 | ✅ | `src/fs/fs.cpp` 读取 + `whaleui_dom_parse` 解析 |
| 编码 | ✅ | lexbor 自带编码探测,UTF-8 为常态 |

### 1.2 标签支持(全部 tag 视为"带默认样式的 div")

**UA 默认样式**(`src/style/theme.cpp` `base_css`,参照 WHATWG UA stylesheet)按 display 分组:

| display | 标签 |
|---------|------|
| `none`(不渲染) | `head style script title meta link base template noscript source track param area wbr dialog option` |
| `block` | `div section article nav aside header footer main address p hr pre blockquote ol ul menu li dl dt dd figure figcaption table caption colgroup col thead tbody tfoot tr td th form fieldset details picture map iframe embed object video audio canvas` |
| `inline` | `a em strong small s cite q dfn abbr data time code var samp kbd sub sup i b u mark ruby rt rp bdi bdo span label` |
| `inline-block` | `img input button select textarea output progress meter datalist` |

**排版默认值**:`h1`-`h6` 字号/字重、`strong/b` 加粗、`em/i`/`var/cite/dfn` 斜体、`small`/`sub/sup` 相对字号、`s` 删除线、`u` 下划线、`mark` 高亮底色、`code/kbd/samp/pre` 等宽字体、`blockquote` 左边框引用样式、`ul/ol/li/dl/dt/dd/figure/figcaption/hr/summary` 的 margin/缩进/padding。

**通用 class 预设**:`.card .row .column .center .app .header .badge .hidden .muted .select-open`。

### 1.3 标签特殊行为(C++ 层实现,非纯 CSS)

| 标签 | 行为 |
|------|------|
| `details`/`summary` | 点击 summary 切换 `open` 布尔属性,折叠时只渲染第一个 summary;run 注入 ▸/▾ marker |
| `input[type=checkbox/radio]` | 原生勾选框绘制,点击切换 `checked`;radio 按 `name` 组互斥 |
| `input[type=text]` / `textarea` / `contenteditable` | 文本编辑:光标、方向键/Home/End/Backspace/Delete/Enter、Ctrl+A 全选;IME 组合文本 |
| `select` / `option` | 下拉框:点击展开、选项选择、回调;`option` 默认 `display:none`,活在弹层里 |
| `ul` / `ol` / `li` | 列表 marker 注入(• / "N. ");嵌套列表不换 marker |
| `progress` / `meter` | 轨道+填充绘制;meter 不区分 low/optimum/high 色区间 |
| `img` | 本地图片解码 + 固有尺寸;`object-fit: cover/contain/fill`;远程/缺失显示占位框 |
| `br` | 文本换行;`hr` 分隔线 |
| `table/tr/td/th` | 仅按 block 垂直堆叠,**无表格布局**(`display:table` 未实现) |

`tag_id` 组件(ECS 第一步,`src/layout/layout.cpp` `tag_id_of`):每个元素在布局期算一次 `WUI_TAG_*` 整数类别,绘制/命中热路径用 O(1) 比较替代字符串比较。

### 1.4 DOM API(`src/dom/dom.cpp` + `events.cpp`)

| 类别 | 状态 |
|------|------|
| 查询(getElementById / querySelector 等) | ✅ |
| 导航(parentNode / children / firstChild / nextSibling 等) | ✅ |
| 结构(appendChild / removeChild / insertBefore / replaceChild) | ✅ |
| classList(add/remove/toggle/contains) | ✅ |
| innerHTML / outerHTML / textContent | ✅ |
| 表单 value / checked / disabled / title | ✅ |
| focus() / 几何(getBoundingClientRect,依赖最近渲染帧) | ✅(未渲染时失败) |
| focus / blur 事件 | ❌ DOM 层 no-op(焦点在 render 层) |

### 1.5 事件(`src/dom/events.cpp`)

| 事件 | 真实派发(接入事件循环) | 手动 dispatch_event 可触发 |
|------|:---:|:---:|
| click / mousedown / mouseup | ✅ | ✅ |
| keydown / keyup | ✅ | ✅ |
| mousemove / focus / change / input / submit / load / scroll / resize / wheel / contextmenu 等其余类型 | ❌ | ✅ |
| 冒泡 / 捕获 | ❌ 仅目标元素派发 | — |

### 1.6 交互能力(render 层)

- 鼠标:点击、`:hover/:active/:focus`、拖动(滚动条/文本选择)、滚轮滚动(`overflow:auto/scroll` 容器与整页,位置夹取 `[0, scroll_max]`,行为可替换)
- 文本选择:跨元素基于布局树前序序号 O(1) 判定,拖选高亮
- 文本编辑 + IME(`SDL_StartTextInput` / `SDL_EVENT_TEXT_INPUT` / `SDL_EVENT_TEXT_EDITING`)
- 剪贴板(Ctrl+C/V):❌ 未实现

---

## 2. CSS 支持情况

### 2.1 选择器

| 选择器 | 状态 |
|--------|------|
| 标签 / `#id` / `.class` | ✅ |
| 组合(`div.card`)、后代(`div .card`)、子代(`>`)、相邻兄弟(`+`) | ✅ |
| `:hover` / `:active` / `:focus` / `:focus-visible`(经交互状态匹配,:hover/:active 按 CSS 语义向祖先冒泡) | ✅ |
| `:disabled`(按 disabled 属性) | ✅ |
| `:first-child` / `:last-child` / `:nth-child(odd\|even\|N\|n\|An+B)` | ✅ |
| `::before` / `::after`(`content` + 独立 paint 属性合并) | ✅ |
| 其余伪类(如 `:not` / `:checked` / `:visited` 等) | ❌ 解析时跳过 |

### 2.2 级联规则(`src/style/style.cpp`)

`!important` > 特异性(id > class/属性/伪类 > 类型)> 源顺序;inline style 最高(除非规则带 `!important`);`var()` 全局解析(含 fallback)。

### 2.3 @ 规则

| @ 规则 | 状态 |
|--------|------|
| `@media` | ✅ `max-width` / `min-width` / `prefers-color-scheme` / `prefers-reduced-motion`;其余条件安全地判 false |
| `@keyframes` | ✅ 帧 `0%/from/to/百分比`,动画引擎插值 |
| `@font-face` | 🟡 解析器识别,字体注册走应用 API(见 `07-api-font.md`) |
| `@import` / `@charset` / `@supports` | ❌ 未处理 |

### 2.4 单位与函数

| 项目 | 状态 |
|------|------|
| 单位 | ✅ px / % / em / vw / vh / 无单位;`auto` / `max-content` / `min-content` / `fit-content` 按 auto 处理 |
| 数学函数 | ✅ `clamp()` / `min()` / `max()` |
| 渐变 | ✅ `linear-gradient`(角度/方向词)/ `radial-gradient`(圆/椭圆、百分比中心)多色标 |
| `var(--x, fallback)` | ✅ 级联后统一解析 |

### 2.5 属性支持矩阵

#### A. 渲染/布局生效

```
display(block/inline/inline-block/flex/inline-flex/grid/inline-grid/none)
width, height, min/max-width/height, box-sizing
margin/padding(简写+四边), border(简写+四边宽度/样式/颜色, border-block 简写)
position(static/relative/absolute/fixed/sticky 基本), top/left/right/bottom, inset 简写
z-index, opacity
flex: flex-direction, flex-wrap, justify-content, align-items, gap, flex-grow, flex 简写
grid: grid-template-columns(px/fr/auto/repeat()), grid-column 整行跨越, column-gap/row-gap
background-color/background(纯色+渐变), background-image(渐变)
border-radius(背景与边框沿弧线)
box-shadow(外阴影 GPU: mipmap 模糊近似 / CPU: 同心环;inset 阴影 GPU: 对角线渐变三角形 + mipmap 模糊 / CPU: 同心内缩环), backdrop-filter: blur()(GPU mipmap 近似)
text-shadow, -webkit-text-stroke
text-decoration(underline/line-through), color
font-size, font-family(按注册字体+fallback 链), font-weight(bold/≥600 合成粗体), font 简写
text-align(left/center/right), line-height(数字/px/em/%), letter-spacing
text-transform(ASCII), white-space(nowrap), overflow(hidden/auto/scroll+滚轮)
cursor(记录), img + object-fit(cover/contain/fill)
transition(property/duration/delay/timing, 白名单属性插值), animation(+@keyframes)
```

#### B. 引擎可处理但渲染未消费(解析/级联/动画可用,绘制暂不生效)

```
transform(动画/transition 可插值,但布局与绘制不位移元素)
transform-origin(仅默认中心)
filter(saturate/blur/drop-shadow 等), clip-path
background-image url(本地文件/data:)
outline
float / clear
```

#### C. 未实现(解析器/引擎未处理)

```
word-spacing, background-size/position/repeat
flex-shrink, flex-basis(flex 简写仅消费 grow), align-content, place-items, align-self
justify-self, justify-items, order(布局按 DOM 序,不支持重排)
list-style, visibility, pointer-events, user-select
text-indent, vertical-align, word-break, overflow-wrap, aspect-ratio
column-count(column-gap 仅 grid 用), scroll-behavior, tab-size, resize
grid: 显式 grid-template-rows / 命名区域 / dense 打包 / grid-row
position:sticky 容器底部钳制
表格布局(display:table / table-row / table-cell)
```

### 2.6 主题

7 套主题样式(Fluent / Metro / Material / Classic / Aero / GTK / macOS),各含深浅色变量,全局生效;`whaleui_app_set_theme_style` 或页面 `<select>` 热切换;默认跟随系统深浅色。

---

## 3. 与 css-priority.md 清单对照

### 3.1 "最基础"清单(README-css 第 4 行)→ **全部实现** ✅

`color, background, margin, padding, border, display, width, height, font-size, font-weight, text-align, line-height, position, top, left, right, bottom, z-index, flex, justify-content, align-items, gap, opacity, cursor, overflow, box-sizing`

### 3.2 "必须实现"清单(README-css 第 6 行)

| 属性 | 状态 |
|------|------|
| font-family, text-decoration, text-transform, letter-spacing, white-space, border-radius, box-shadow, background-color, background-image, transition, animation, flex-direction, flex-wrap, flex-grow, grid-template-columns, grid-gap, max/min-width/height, margin/padding 四边, border-width/style/color, var/media/keyframe | ✅ |
| word-spacing | ❌ |
| background-size / background-position / background-repeat | ❌(渐变填充不受其约束) |
| transform | 🟡 动画可插值,渲染未生效 |
| flex-shrink / flex-basis | ❌ |
| align-content / place-items | ❌ |
| grid-template-rows | ❌ |
| float / clear | 🟡 已解析未生效 |
| list-style | ❌ |
| visibility | ❌ |
| pointer-events / user-select | ❌ |
| outline | 🟡 已解析未生效 |

### 3.3 "最好实现"清单(README-css 第 12 行)

| 属性 | 状态 |
|------|------|
| text-shadow, -webkit-text-stroke(扩展), font-style(italic/oblique → TTF 斜体), transition-duration/delay/timing-function, animation-duration/delay/iteration-count/direction/fill-mode, column-gap(仅 grid), content(::before/::after), inset | ✅ |
| text-indent, vertical-align, word-break, overflow-wrap, aspect-ratio, resize, object-fit(✅ 实际已实现), filter, backdrop-filter(✅ blur 已实现,CPU 路径缺), clip-path, transform-origin(默认中心), grid-column(✅ 整行), grid-row, grid-auto-flow, order, align-self, justify-self/items, column-count, counter-* , quotes, scroll-behavior, scroll-margin/padding, tab-size, fill/stroke/stroke-width | ❌/🟡 见 2.5 |

---

## 4. 已知限制速览

- 布局期按估算宽度折行,渲染期才做真实字形度量;长文本可能溢出盒子(`overflow:hidden` 可裁)
- 表格无 table 布局;inline 混排顶部对齐、无基线对齐
- 无事件冒泡/捕获;部分事件类型仅可手动派发
- 单线程渲染;脏矩形/子树包围盒/滚动截取已做,规则级联缓存未做(大页面重布局约 30ms)
- `box-shadow` 多阴影列表只取第一个;`backdrop-filter` 无 CPU 回退(lite/minimal 不生效);`box-shadow: inset` 的偏移量(ox/oy)按盒子本体渲染(近似忽略)
- 媒体查询仅支持 4 种条件;`@import`/`@supports` 未处理
- 剪贴板、`pointer-events`、`user-select` 等交互属性未实现

---

## 5. 用例

以下用例均落在第 2 章"已支持"范围内,可直接放入页面 `<style>` / HTML 验证效果。

### 5.1 布局:flex 与 grid

```html
<style>
  .toolbar { display: flex; justify-content: space-between; gap: 8px; }
  .grid3   { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; }
  .sticky  { position: sticky; top: 0; }
  .overlay { position: fixed; left: 50%; top: 50%; transform: translate(-50%, -50%); }
</style>
<div class="toolbar"><span>左</span><span>右</span></div>
<div class="grid3"><div class="card">1</div><div class="card">2</div><div class="card">3</div></div>
```

> `transform` 仅供动画插值,不位移元素;固定居中请用 `left/top` 配合 `margin` 或 flex 容器。

### 5.2 交互标签

```html
<details open>
  <summary>展开详情</summary>
  <p>折叠时只显示 summary,点击切换 ▸/▾。</p>
</details>

<input type="checkbox" checked> 勾选
<input type="radio" name="g" checked> A
<input type="radio" name="g"> B   <!-- 同 name 互斥 -->

<select>
  <option>选项一</option>
  <option selected>选项二</option>
</select>

<progress value="60" max="100"></progress>
<input type="text" placeholder="可编辑,支持 IME">
```

### 5.3 视觉样式:渐变 / 圆角 / 阴影 / 变量

```html
<style>
  :root { --brand: #4a7dff; }
  .hero {
    background: linear-gradient(135deg, var(--brand), #a0c0ff);
    border-radius: 12px;
    box-shadow: 0 4px 12px rgba(0,0,0,0.15);
    color: #fff; padding: 24px;
    transition: background-color 200ms ease;
  }
  .hero:hover { background-color: #3a6dff; }
  @media (prefers-color-scheme: dark) {
    .hero { box-shadow: 0 4px 12px rgba(0,0,0,0.5); }
  }
</style>
<div class="hero">渐变卡片</div>
```

### 5.4 动画与伪元素

```html
<style>
  @keyframes fade { 0% { opacity: 0; } 100% { opacity: 1; } }
  .toast { animation: fade 300ms ease-out; }
  .note::before { content: "→ "; color: var(--brand); }
  .note::after  { content: " ←"; color: var(--brand); }
</style>
<div class="toast">淡入提示</div>
<p class="note">伪元素插入了前后标记</p>
```

### 5.5 深浅色与主题切换

```css
/* 系统深浅色跟随:whaleui_app_set_theme(app, WHALEUI_THEME_SYSTEM)(默认) */
/* 手动切换:WHALEUI_THEME_LIGHT / WHALEUI_THEME_DARK;运行时 T 键亦可 */
@media (prefers-color-scheme: dark) {
  body { background: #1e1e1e; color: #eee; }
}
```
