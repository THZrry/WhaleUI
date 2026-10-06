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
| 50% | `flex`（含 `flex-direction/wrap/justify-content/align-items/gap`）、`grid`、`aspect-ratio`、`transform`（translate/scale/scaleX/scaleY/skewX/skewY 生效；`matrix()` 分解为已支持 ops；`rotateX/Y` 降级为透视压缩；`rotateZ`=rotate；`perspective/translateZ/scaleZ` 忽略；锚点走 `transform-origin`；**rotate 转背景/边框/渐变轴/命中**——GPU 顶点旋转近似 + 渐变投影方向同转 + hit_test 逆旋转重测，文本/子树/`::before` 不转）、`perspective`、`backface-visibility`、`animation(-*)`、`text-shadow`、`border-image`（未验证）、`outline`、`outline-offset`、`accent-color`、`caret-color`、`scroll-behavior`、`scroll-snap-type`、`user-select`、`pointer-events`、`resize`、`counter-reset/increment/set`（作用域栈，见函数备注） | ⚠️ 见备注 |
| 20% | `align-content/self`、`justify-self/items`、`place-*`、`row/column-gap`、`grid-template-*`、`grid-auto-*`、`inset`（含四值简写）、`writing-mode`、`direction`、`text-orientation`、`text-decoration-*`、`overscroll-behavior`、`scroll-margin/padding`、`container-*`、`all`、`animation-composition`、`transform-style` | ⚠️ 见备注 |

备注：

- `backdrop-filter` **GPU 路径已实现**（blur 双档近似：`blur>=8` mip 阶梯、
  `blur<8` 预定义 3x3 二项式核作用于单 mip 层，见 notes <15>；元素自身
  body——纯色/渐变/边框——走 LATE pass 画在模糊之上，不再丢失；圆角盒的
  模糊区被 SDF 裁剪；<16> 的 param buffer 上溢回归已修）；Minimal/纯
  CPU 回退路径只画 body、不模糊。
- `box-shadow` 的 `inset` **GPU 路径已实现**：`1 - blurred_mask(px - off)`
  的 spread-收缩盒 mask（offset 精确，无四象限近似、无接缝），圆角用同一
  SDF 裁剪，`blur:0` 出实线环；CPU 路径是带 offset 的同心内缩环 /
  硬环四条边。`spread` 为负按 0 处理（收缩语义无定义扩展）。
- `background-blend-mode` / `mix-blend-mode` **已实现**：11 种可分离模式精确
  + Porter-Duff 合成；hue/saturation/color/luminosity 亮度混合近似。
  background-blend 在渐变发射前折角颜色（零新增 draw）；mix-blend 走 pass F
  compute（body 单色 vs painted frame，子树内容不参与）。
  `border-image` / `writing-mode` / `text-orientation` / `container-*` /
  `animation-composition` **未实现**（清单里保留为待办，不要当成已支持）。
- `filter`（元素滤镜）**GPU 路径已实现**：composite 后的 pass E，mip blur
  （记录按 blur 外扩，晕出盒）+ 4x5 颜色矩阵
  （brightness/contrast/saturate/grayscale/invert/sepia/hue-rotate/opacity，
  CSS 顺序左乘）；CPU 路径只做颜色矩阵（blur 跳过）。
- `clip-path` **已实现**（inset/circle/ellipse；polygon 解析但按普通绘制）：
  顶点尾字段（ctype/cc/cr/crad，quad 局部坐标，旋转 follow）+ solid shader
  第二 SDF 相交 + CPU `inside_clip` 镜像；paint_node 压栈/弹栈（含早退弹栈），
  子树继承；solid/round/rot/gradient-linear/gradient-radial/图片/文字全发射器
  接入。回归：test_scroll `[clip]` 像素。
- `mask-image`（含 `-webkit-mask-image` 别名）**已实现**（linear-gradient
  alpha 遮罩）：mask 渐变按 `fill_gradient` 同轴投影取四角 alpha，
  paint_node 压栈/弹栈（栈式，子树继承）；发射器按逐角 alpha 相乘——solid/
  round（round 走 gradient_rect + radius 尾参，一次 draw 同时带圆角 SDF 与
  逐角 alpha）/gradient-linear（四角 tint）/gradient-radial（逐层中心采样）/
  图片+文字 blit（逐像素）；CPU 全路径逐像素相乘。非 linear 遮罩解析但跳过
  （不透明绘制，不杀整串）。热路径门控（无 mask/clip 时零新增检查）。
  回归：test_scroll `[mask]` 像素（红→背景渐隐梯度）。
- `padding/margin` 的**四值简写与四条 longhand 都生效**（第 1 条修掉之后）；
  百分比 padding 按父内容宽解析，与规范一致。

### 选择器 / 伪类 / 伪元素

| 组 | 项目 | 状态 |
|----|------|------|
| 基础 | `*`、`tag`、`.class`、`#id`、`[attr]`、`[attr="v"]`、`A,B` | ✅ |
| 组合器 | 后代、`>`、`+`、`~` | ✅ |
| 80% | `:hover` `:active` `:focus` `:link` `:visited` `:first-child` `:last-child` `:not()`（取参数最大权重） `:enabled` `:required` `:optional` `:empty` `::before` `::after` `::placeholder` `::marker` | ✅ |
| 50% | `[attr^=]` `[attr$=]` `[attr*=]` `[attr~=]` `[attr|=]`、`:nth-child()` `:nth-of-type()` `:target` `:focus-within` `:is()`（取参数最大权重） `:where()`（零权重） `:disabled` `:checked` `:valid` `:invalid`、`::selection` | ⚠️ |
| 20% | `:has()` `:nth-last-child()` `:nth-last-of-type()` `:only-child` `:only-of-type` `:first-of-type` `:last-of-type` `:dir()` `:lang()` `:user-valid` `:user-invalid` `:focus-visible`、`::first-line` `::first-letter` | ⚠️ 多数未实现 |

- `:checked` 匹配 `checked` / `selected` **属性**（点击引擎画的原生 checkbox
  会 set/remove 该属性，因此动态勾选也能命中）。
- `:indeterminate` 没有属性对应物，引擎接受一个 `indeterminate` 属性作为表达方式。
- `:not()` 支持单层简单选择器（含 `:not(:hover/:active/:focus)` 状态取反，
  与正向同冒泡规则）；嵌套 / 复杂参数未验证。
- `::selection` 的 `background` + `color` 都生效（`selection-bg/fg` 虚属性，
  页面规则赢主题，`var()` 展开；光栅逐字形换色）。
- `::before`/`::after` 盒合并伪规则的**全部声明**（无属性白名单；
  纯 `content` 的仍走行内文本 run）；`transform-origin` 参与 scale/rotate
  锚点（含 hit_test）。

### 函数

| 组 | 函数 | 状态 |
|----|------|------|
| 80% | `rgb() rgba() hsl() hsla()`、`hwb()`、`lab()` `lch()` `oklab()` `oklch()`（→sRGB，域外钳制）、`color-mix(in srgb/srgb-linear/hsl + 百分比权重，递归解析，oklab 近似走线性）`、`var()`、`url()`、`calc()`、`linear-gradient()`、`conic-gradient()`（CPU 精确 atan2 角度 закладочный start ray + 中心；GPU 跳过留底色，主题零用量）、`repeating-linear-gradient()`（CPU 逐像素取模精确；GPU 轴向按 stop 段切分精确条纹，对角回退整盒取模近似）、`repeating-radial-gradient()`（CPU 取模精确；GPU 10 层取模近似） | ✅ |
| 50% | `min()` `max()` `clamp()`（N 元 + `calc()` 乘除 + `rem`/`vmin`/`vmax`）、`radial-gradient()`（size 关键字 + position 关键字，见备注）、`attr()`（`content` 内）、`counter()` `counters()`（`counter-reset/increment/set` 作用域 + decimal/alpha/roman 样式，见备注） | ✅ |
| 20% | `conic-gradient()`、`repeating-*-gradient()` 已见上（conic GPU 跳过，repeating 对角 GPU 近似） | ⚠️ 部分近似 |
| 20% | `image-set()`、`minmax()` `fit-content()` `repeat()`、`abs()` `sign()` | ⚠️ `minmax()`/`repeat()`/`fit-content()` 在 grid 里 ✅；其余 ❌ |

备注（新增）：

- `animation-play-state` 生效（`paused` 冻结/`running` 恢复，apply 与 tick
  双路径）；多过渡 per-property duration/delay/timing（逗号列表按索引循环，
  不再坍缩到第一组）。
- `object-position` 生效（cover/contain 拟合后定位）；附带修 object-fit 轴
  选反 + RGB24 无 alpha 图蓝通道丢失（转 RGBA32）。
- `aspect-ratio: w/h` 单轴 auto 时从另一轴推；sticky 支持 bottom/right
  （相对视口远边）+ 容器钳制；`scrollbar-width/color` 可配；
  `list-style-type`（square/circle/none/alpha/roman）+ `li::marker{content}`；
  `overflow-x/y` 分写。
- `radial-gradient` 的 closest/farthest-side/corner 由 `radial_resolve` 按盒
  解算（circle 取单半径；ellipse 取双轴）。
- `counter-reset/increment/set` + `content: counter()/counters()` 生效：
  Builder 持作用域栈（reset/set 压层继承父副本，increment 改栈顶，RAII
  guard 配对弹栈，::before 看到递增后值）；`counters(name, sep)` 连接所有
  含名层（嵌套列表 "1.1" 是规范行为）；decimal/alpha/roman 复用 marker 转换；
  `content` 混合 token（引号串 + attr() + counter()) 按顶层空格切分展开。
  附带修两处：纯引号 fast-path 误剥混合 content 首尾引号（加内引号检查）；
  全显式渐变 stop 被均匀重写（repeating 硬条纹变过渡带，batch3 中心 E6）。

## 仍未做（连同原因）

1. **`transform: rotate()` 的文本/子树**：解析、插值、背景/边框/渐变轴/命
   中的旋转都有了（GPU 顶点旋转 + 渐变投影同转 + hit_test 逆旋转重测，
   notes <15>/<16>；锚点走 `transform-origin`，<17>）。文本、子元素仍直立；
   要全部转需要一条矩阵 paint 路径（文本/子树走同一变换）。
   `::before` 状态层不随转。
2. **未转 sRGB 的颜色输入**：`whaleui_render_parse_color` 认 hex、
   `rgb()/rgba()`、`hsl()/hsla()`、`hwb()`、`lab()/lch()/oklab()/oklch()`、
   `color-mix()` 与具名色；`color-mix` 的 oklab 空间走线性近似（保色相精确
   是后续步骤，页面几乎全用 srgb 混合）。
3. **`backdrop-filter` / `inset` 的 CPU 回退**：GPU 有（双档近似 / mask
   反演），Minimal/软件路径只画 body（backdrop 不模糊；inset 是带 offset
   的同心环近似）。
4. **`::first-line` / `::first-letter`**：伪元素支持
   `::before`/`::after`/`::marker`/`::selection`/`::placeholder`。
5. `:has()` 等高级选择器、容器查询。

这些条目同时留在 `temp/engine-gaps.md`（原始实测数据）与
`notes/current.md`（未解决项）里，别让它们只剩在一个地方。
