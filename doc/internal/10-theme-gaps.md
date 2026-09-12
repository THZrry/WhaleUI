# Theme CSS: 两层保真度 + 引擎缺口的实测清单

> **引擎侧现在的支持矩阵与修复记录在 `doc/internal/16-engine-support.md`**。
> `temp/engine-gaps.md` 保留的是**修复之前**的原始实测数据（探针页 + 逐项
> 复现命令），别当成现状。本文只讲主题侧的结构与取舍，并指向那两份。

主题样式表是若干**真实设计系统**的翻译（classic 原本也在其中，因参考集
`temp/Themes/` 里没有它，已按要求删除）：

| style   | source                                                                 |
|---------|------------------------------------------------------------------------|
| browser | 手写 WHATWG UA 默认样式表 + temp/Themes/Simple/readme.md                |
| fluent  | https://github.com/aipx-proto/fluent-css + Win11 Fluent 2               |
| metro   | Office UI Fabric Core 9.6.1 palette                                    |
| material| https://github.com/zdhxiong/mdui (Material Design 3 tokens)             |
| aero    | https://github.com/khang-nd/7.css (Win7)                               |
| gtk     | https://github.com/mclellac/adwaita-web (GNOME Adwaita)                 |
| macos   | https://github.com/connors/photon                                      |

## 文件划分

| 文件               | 内容                                                  |
|--------------------|-------------------------------------------------------|
| `theme_css.cpp`    | UA 基线 + 组件类 + 每主题结构覆盖                      |
| `theme_colors.cpp` | 每主题的深浅色调色板                                   |
| `theme.cpp`        | 字号/圆角/控件尺寸表 + 组装 + C API                    |
| `theme_internal.h` | `VarMap` + 三处跨文件入口                              |

一份样式表 = 共享 UA 基线（每个 tag 一个 WHATWG 风格默认值）+ 共享组件类
（`.card` / `.btn-*` / `.tabs` / `.toolbar` / `.pane` / `.anim-*`，靠
`class=` 选择加入）+ 一层薄的每主题覆盖。所有外观值都是 `var(--*)`，
主题只提供值。渲染器原生控件读 `--accent`/`--field`/`--border-strong`/
`--card`；select 弹层由虚拟类 `.select-popup`/`.select-option*` 绘制。

## 两层保真度（有意为之）

- **browser / Simple**：只用引擎现有写法，当前**每条规则都能渲染**，是"已知
  正确"的基准。
- **其余主题**：照参考系统原样写，**包括**引擎还没实现的特性；依赖它们的规则
  挂 `needs:` 标记，引擎补齐即生效。

## 实测支持（推翻旧文档的几条）

- `display: table` / `table-row` / `table-cell` → **有真正的表格布局**
  （三个 `td` 横排等分；`temp/gaps3.html`）。
- 属性选择器 `[attr]` / `[attr="value"]` → **支持**。
- `:not()` → **支持**。
- `flex: 0 0 100px`（flex-basis + flex-shrink）→ **精确生效**。
- `::before` 盒（`position`+`left/top/width/height`+`background`）→ 父盒是
  block 时**画得出来**；仅 flex 按钮内的百分比尺寸不准。
- `placeholder` → **有字形**。
- `inline-block` / `block` + `position: relative` → **正常**（只有
  `inline-flex` + relative 会炸）。

## 主题 CSS 的取舍（引擎缺口修好之后）

修复记录与逐项证据见 `16-engine-support.md`。下面这些取舍**在缺口修好后
已经回到参考写法**：

- `multi-shadow`：各主题的 `--shadow-*` 照参考写成"环境层 + 键层"，
  现在两层都画（含 spread 与 `0 0 0 Npx` 焦点环）。
- `flex-rel`：material / aero 的按钮回到参考的 `inline-flex` + flex 居中，
  workaround 的 `display: inline-block; line-height: var(--btn-h)` 已删除。
- `inset`：`inset: 0` 能给绝对定位盒定尺寸，样式表可用简写。
- 四边异色 `border-color`：aero / macos 的按边描边（四边深浅、深底边）生效。
- `outline`：Fabric(metro) 与 7.css(aero) 的 dotted 焦点框显示（短段近似）。
- 水平 `padding` / `margin`：`h1..h6` 的三值 margin、`blockquote` / `dd` 的
  四值简写都生效，无需退回两值写法。
- `background-size` / `-position` / `-repeat`、无方向 `linear-gradient()`、
  `hsl()`、`:checked`、`::before` 百分比尺寸 —— 均可用。
- `overflow: hidden`：引擎层面能写，但**按钮基础样式里不写** ——
  overflow≠visible 的元素在 flex 行里 min-content 归零（规范行为），
  拥挤的一行会把按钮压到 1px（material 实测）。

## 本轮（主题重写 + 验证）

- 按参考 token 重写全部样式表；模块拆成
  `theme_css.cpp` / `theme_colors.cpp` / `theme.cpp`（+`theme_internal.h`），
  每个文件都能一次读完（原来是单文件 1020 行）。
- 颜色成对指定：`--fg`/`--heading` 对 `--bg`/`--card`，`--field-fg` 对
  `--field`，`--btn-fg` 对 `--btn-bg`，`--accent-fg` 对 `--accent`。
  Material 用 M3 token 三元组（primary `#6750a4` / on-primary `#ffffff`，
  深色 `#d0bcff` / `#371e73`），Metro 用 Fabric Core 9.6.1，GTK 用 Adwaita
  `#3584e4`，Aero 用 7.css `--w7-*`，macOS 用 photon。
- 新增 `--btn-border-hover` / `--field-border-hover` / `--btn-h` / `--btn-px` /
  `--btn-weight` / `--in-h` / `--in-px` / `--h1`..`--h6` / `--edge-light` /
  `--edge-dark` / `--shade` / `--mark-bg` / `--mark-fg` / `--password-char` /
  `--danger-hover` —— 组件层不再硬编码数值。
- **修回被我改坏的表格**：UA 里 `table`/`tr`/`td` 一度被写成 `display: block`
  （我按过时文档改的），已改回 `display: table`/`table-row`/`table-cell`。
- `body` 不带外边距（零留白是既有契约，多处按坐标断言），Simple 要的留白由
  组件层承担；标题字号阶梯按主题给。
- 删除 `classic`（参考集外）：kSpecs / overlay / 调色板 / demo 与 showcase
  下拉一并摘除，主题数 8 → 7。`tests/test_style.cpp` 的 `n >= 8` 断言同步
  改为 `n >= 7`。

### 已验证

- `xmake b -y`：三个库 + demo + 10 个测试 target 全部构建通过；**10/10 测试
  套件 exit=0**（含删 classic 后的 `test_style`）。
- 离屏渲染逐套主题 × 深浅配对自检通过：浅色主题主色亮度 0.94–1.00、深色
  0.07–0.23，text/surface spread 0.44–0.96（`temp/theme_check.py`）。
- 控件像素与 token 精确一致：material primary `#6750a4` 91.5%、aero primary
  `#3c7fb1` 85.4%、classic 按钮渐变 `#ffffff → #d9d6d0 → #d0cdc4`（classic
  当时仍在，数据保留为实测记录）。
- 按钮高度按主题生效（fluent 32 / material 42 / aero 25 px）。

### 仍未做

- [ ] 与参考截图（`temp/Themes/*/*.png`）**肉眼**比对 —— 无头检查只能证明
      配色配对与 token 命中，证明不了"像不像"。
- [x] ~~引擎修好 `flex-rel` / `overflow-clip` / 四边 border / `outline` /
      多阴影 / 伪元素百分比尺寸之后，重新渲染并删掉 material/aero 的
      `inline-block` workaround~~ —— 已完成，见 `16-engine-support.md`；
      7 主题 × 深浅 15 张离屏自检全部通过。
