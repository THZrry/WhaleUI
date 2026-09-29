# 引擎 CSS 支持情况（实测矩阵 + 本轮缺口修复）

本文回答一个问题：**WhaleUI 现在到底支持哪些 CSS**。
清单取自 `temp/csses..md`（W3C 现行属性 / 选择器 / 伪类 / 伪元素 / 函数，
按 80% / 50% / 20% 网页使用率分层）。`10-theme-gaps.md` 记的是主题侧的
取舍，`temp/engine-gaps.md` 是修缺口之前的原始实测记录。

## 实测方法（不要看代码猜，用探针）

```powershell
cd C:\aaa\whaleui
xmake b -y render_shot            # target 只在 temp/render_shot.cpp 存在时定义
$exe = '.\build\mingw\x86_64\release\render_shot.exe'
# dump: stderr 每行 "tag xy=(x,y) wh=(w,h) 文本"
& $exe "file://C:/aaa/whaleui/temp/gaps7.html" temp/gaps7.ppm browser light 400 dump 5 0
python temp\rect_color.py temp/gaps7.ppm 24 96 60 112   # 像素判决
```

探针页：`temp/gaps.html`（阴影/outline/四边 border/伪元素/表格）、
`gaps2.html`（flex + relative）、`gaps3.html`（table/flex 宽度）、
`gaps4.html`（rotate/text-shadow）、`gaps5.html`（水平 padding/margin）、
`gaps7/8/9.html`（四边与单边简写）、`gaps10.html`（overflow:hidden 组合）、
`gaps11/12.html`（阴影层与 spread）、`gaps13/14.html`（text-shadow）、
`gaps17.html`（颜色通道）、`gaps18.html`（background-size/渐变）、
`gaps19/20/21/22.html`（`:checked` 与无值属性）。

两条容易踩的坑：

- **颜色通道要用非对称色**（`#123456`）。纯红 `#ff0000` 与纯蓝 `#0000ff`
  在 R/B 互换的 bug 下会互换成另一个纯色，看起来"颜色是对的"。本轮就是
  靠 `#123456` 测得文字 R/B 反转。
- **带 BOM 的探针文件会让 CSS 整体失效**（见下文第 11 条），文件写成
  无 BOM 再测，否则量到的是"CSS 没加载"而不是你想问的那条属性。

## 本轮修复（现象 → 根因 → 位置）

| # | 现象 | 根因 | 位置 |
|---|------|------|------|
| 1 | `padding-left` / `margin-left` / 四值简写"失效"（`padding: 0 0 0 60px` 文字仍在 x=0，`padding: 0 60px 0 0` 反而 x=60） | `m[]/p[]/border_w[]` 索引是 `[top,right,bottom,left]`，左边缘却取了索引 1（right）。对称值能自洽，所以只有单边/不对称才露馅 | `layout.cpp` 盒 pass（`border.x`/`content.x`）与 flex 重布局 |
| 2 | `inline-flex` 元素撑满整行（按钮 `wh=(920,32)`） | `display_kind()` 把 `inline-flex` 与 `flex` 都归为 1，宽度分支只对 `inline`/`inline-block` shrink-to-fit | `layout.cpp` `inline_level_display()` + 宽度分支 |
| 3 | `inline-flex` + `overflow:hidden` 高度爆炸（`wh=(98,210)`，文本 run 被排成 1px 宽、95px 高） | flex item 的 min-content floor 会因 `overflow:hidden` 降为 0；**text run 继承了元素的 computed style**，被误当成 overflow 容器 → main size=1px → 绘制时一字符一行 | `layout.cpp` `layout_flex` min_main |
| 4 | flex 容器里的 `::before{position:absolute;inset:0}` 状态层尺寸错（`(49,2)`） | 绝对定位子项被当成 flex item 参与分配；且 auto 高度容器在子布局时还不知道自己的高度 | `layout.cpp`：out-of-flow 退出 flex 流；盒 pass 末尾按**最终 padding box** 重定位 out-of-flow 子树 |
| 5 | `linear-gradient(#f00, #00f)`（无方向）整条不画，带 `background-size` 时"连渐变本体都没有" | `parse_gradient` 无条件把 `parts[0]` 当方向头跳过，首个颜色丢失 → 单 stop 渐变什么都不画 | `render_color.cpp` `parse_gradient` |
| 6 | `background-size` / `-position` / `-repeat` 一律忽略 | 渐变/背景固定铺满 border box | `render_paint.cpp`：tile 盒 + 平铺/单块 |
| 7 | 多阴影只画第一层（主题的"环境层+键层"丢掉最显眼的键层）；`0 0 0 3px` 焦点环不画 | `paint_shadow` 只解析单层，`parse_shadow` 要求 blur>0，无 spread | `render_paint.cpp`：按顶层逗号拆层、逆序绘制、支持 spread 与 blur=0 硬阴影 |
| 8 | `outline` / `outline-offset` 完全不绘制 | 渲染器不消费 | `render_paint.cpp` `paint_outline()`（画在盒子外侧，dotted/dashed 用短段近似） |
| 9 | 四边异色 `border-color` 退化成单色 | 取色只看 `border` 简写 | `render_color.cpp` `border_side_color()`（longhand → TRBL 简写 → `border` 简写） |
| 10 | **所有彩色文字 R/B 反转**（`color:#123456` 画成 `#563412`），`text-shadow` 因此颜色不对 | text layer 声明为 `B8G8R8A8` + 上传按 BGR 直传 + composite shader 再 `.bgra` 一次 = 两次交换。D3D12 的 `Load` 按格式名解码，于是净效果反转；Vulkan 侧却是对的 | `gpu.cpp`（层改 `R8G8B8A8`、上传时逐像素交换）、`gpu_shaders.h`（去掉 shader 里的 swap） |
| 11 | 带 UTF-8 BOM 的 HTML：`<style>` 被当正文渲染 + 整页 CSS 失效 | BOM 进入解析器成为 `<html>` 前的文本节点，样式表落到错误的作用域 | `dom.cpp` `whaleui_dom_parse_html`、`css.cpp` `whaleui_css_parse_full` 跳过 `EF BB BF` |
| 12 | `[checked]` / `:checked` / `:disabled` / `[disabled]` 一律不匹配（HTML 里这类属性通常没有值） | lexbor 对**无值属性**不存 value，`lxb_dom_element_get_attribute` 返回 NULL；存在性判断必须用 `lxb_dom_element_has_attribute` | `style.cpp` `part_match()` |
| 13 | `transform: rotate(45deg)` 让整条 `transform` 失效（同串的 translate/scale 一起丢） | `transform_parse` 遇到未知函数 `return false` | `animate.cpp`：新增 op=4 rotate（deg/rad/turn）、插值、`whaleui_transform_t.rot` |
| 14 | 渐变 hover 过渡是瞬时的 | `value_lerp` 对渐变值无法插值 | `animate.cpp` `gradient_lerp()`：按 stop 逐个插值（stop 数不同则瞬时） |
| 15 | `hsl()`/`hsla()` 颜色不画（落到默认色）；`rgb(18, 52, 86)`（逗号后有空格）同样不画 | `parse_color` 只认 hex 与无空格的 `rgb()/rgba()` | `render_color.cpp`：新增 `hsl/hsla`（deg/grad/rad/turn + S/L%）与 `rgb/rgba` 的空格容忍 |
| 16 | checkbox/radio 的布局盒 32px 高（`height:20px` 也压不下去），而绘制是 16x16 —— 命中区与画面不一致 | UA 基线把 `min-height: var(--in-h)`（32px）套到了原生小控件上；另外行盒会把 inline 成员的高度统一成行高，覆盖显式 `height` | `theme_css.cpp`：checkbox/radio 的 `min-height/min-width/padding` 归零；`layout.cpp`：行盒不再覆盖带显式 `height` 的成员 |

## 支持矩阵

### 属性

| 组 | 属性 | 状态 |
|----|------|------|
| 80% | `width/height/min-*/max-*`、`margin`、`padding`、`border`、`border-radius`、`box-sizing`、`color`、`font-family/size/weight`、`line-height`、`text-align`、`text-decoration`、`background(-color/-image/-repeat/-position/-size)`、`opacity`、`display`、`position`、`top/left/right/bottom`、`z-index`、`float`、`clear`、`cursor`、`overflow(-x/-y)`、`white-space`、`word-break`、`text-overflow`、`box-shadow`、`transition` | ✅ |
| 50% | `flex`（含 `flex-direction/wrap/justify-content/align-items/gap`）、`grid`、`aspect-ratio`、`transform`（translate/scale/scaleX/scaleY 生效；**rotate 只转背景/边框**——GPU 顶点旋转近似，文本/子树/渐变/命中不转）、`transform-origin`、`perspective`、`backface-visibility`、`animation(-*)`、`text-shadow`、`border-image`（未验证）、`outline`、`outline-offset`、`accent-color`、`caret-color`、`scroll-behavior`、`scroll-snap-type`、`user-select`、`pointer-events`、`resize`、`counter-reset/increment`（未验证） | ⚠️ 见备注 |
| 20% | `align-content/self`、`justify-self/items`、`place-*`、`row/column-gap`、`grid-template-*`、`grid-auto-*`、`inset`（含四值简写）、`writing-mode`、`direction`、`text-orientation`、`text-decoration-*`、`background-blend-mode`、`mix-blend-mode`、`filter`、`backdrop-filter`、`overscroll-behavior`、`scroll-margin/padding`、`container-*`、`all`、`animation-composition`、`transform-style` | ⚠️ 见备注 |

备注：

- `backdrop-filter` **GPU 路径已实现**（blur 双档近似：`blur>=8` mip 阶梯、
  `blur<8` 预定义 3x3 二项式核作用于单 mip 层，见 notes <15>）；Minimal/纯
  CPU 回退路径不画。
- `mix-blend-mode` / `filter` / `background-blend-mode` / `border-image` /
  `writing-mode` / `text-orientation` / `container-*` / `animation-composition`
  **未实现**（清单里保留为待办，不要当成已支持）。
- `padding/margin` 的**四值简写与四条 longhand 都生效**（第 1 条修掉之后）；
  百分比 padding 按父内容宽解析，与规范一致。

### 选择器 / 伪类 / 伪元素

| 组 | 项目 | 状态 |
|----|------|------|
| 基础 | `*`、`tag`、`.class`、`#id`、`[attr]`、`[attr="v"]`、`A,B` | ✅ |
| 组合器 | 后代、`>`、`+`、`~` | ✅ |
| 80% | `:hover` `:active` `:focus` `:link` `:visited` `:first-child` `:last-child` `:not()` `::before` `::after` `::placeholder` | ✅ |
| 50% | `[attr^=]` `[attr$=]` `[attr*=]`（**部分**：解析支持，未逐项实测）、`:nth-child()` `:nth-of-type()` `:empty` `:target` `:focus-within` `:enabled` `:disabled` `:checked` `:valid` `:invalid`、`::selection` `::marker` | ⚠️ |
| 20% | `:is()` `:where()` `:has()` `:nth-last-child()` `:nth-last-of-type()` `:only-child` `:only-of-type` `:first-of-type` `:last-of-type` `:dir()` `:lang()` `:user-valid` `:user-invalid` `:focus-visible`、`::first-line` `::first-letter` | ⚠️ 多数未实现 |

- `:checked` 匹配 `checked` / `selected` **属性**（点击引擎画的原生 checkbox
  会 set/remove 该属性，因此动态勾选也能命中）。
- `:indeterminate` 没有属性对应物，引擎接受一个 `indeterminate` 属性作为表达方式。
- `:not()` 支持单层简单选择器；嵌套 / 复杂参数未验证。

### 函数

| 组 | 函数 | 状态 |
|----|------|------|
| 80% | `rgb() rgba() hsl() hsla()`、`var()`、`url()`、`calc()`、`linear-gradient()` | ✅ |
| 50% | `min()` `max()` `clamp()`、`radial-gradient()`、`counter()` `counters()` `attr()` | ⚠️ 数学函数 ✅（实测）；`radial-gradient` 走 `parse_gradient` 的 radial 分支，**未逐项实测**；`counter()/attr()` ❌ |
| 20% | `hwb()`、`oklab/oklch/lab/lch`、`color-mix()`、`conic-gradient()`、`repeating-*-gradient()`、`image-set()`、`minmax()` `fit-content()` `repeat()`、`abs()` `sign()` | ⚠️ `minmax()`/`repeat()`/`fit-content()` 在 grid 里 ✅；其余 ❌ |

## 仍未做（连同原因）

1. **`transform: rotate()` 的文本/子树/渐变/命中**：解析、插值、背景/边框
   的旋转绘制都有了（GPU 顶点旋转，notes <15>）。文本、子元素、渐变轴、
   hit_test 仍按未旋转处理；要全部转需要一条矩阵 paint 路径（背景/边框/
   文本/子树走同一变换）。`::before` 状态层不随转。
2. **`conic-gradient` / `repeating-*-gradient` / `image-set` / `hwb()` /
   `oklab/oklch/lab/lch` / `color-mix()`**：`whaleui_render_parse_color` 认
   hex、`rgb()/rgba()`、`hsl()/hsla()` 与具名色，其余不认。
3. **`backdrop-filter` 的 CPU 回退**：GPU 有（双档近似），Minimal/软件路径没有。
4. **`::first-line` / `::first-letter` / `::marker`**：伪元素只支持 `::before`/`::after`。
5. `:has()` 等高级选择器、容器查询、`filter`/混合模式。

这些条目同时留在 `temp/engine-gaps.md`（原始实测数据）与
`notes/current.md`（未解决项）里，别让它们只剩在一个地方。
