# 交互状态（:hover / :active / :focus）与布局重排

这份文档记 hover 的完整链路、它触发的两条重排路径，以及 2026-09 那轮
"hover 页面上下跳 / flex 盒子悄悄变长 20px" 的两个根因与复现方法。

## 链路

| 步骤 | 位置 | 做什么 |
|------|------|--------|
| 1 | `core/app.cpp` `SDL_EVENT_MOUSE_MOTION` | 每次移动都调 `whaleui_render_set_hover(r, x, y)` |
| 2 | `render.cpp` `whaleui_render_set_hover` | `hit_test` → `hover_el = hit->el`；把旧目标压进 `hover_prev`，置 `state_pending = 1`。**这一步零布局** |
| 3 | `render.cpp` 帧循环（`if (r->state_pending)`） | targets = `hover_prev` + `{hover_old_el, hover_el, focus_old_el, focus_el, pressed_old_el, pressed_el}`，逐个重新 cascade |
| 4 | 同上 | 拿新 cascade 与**节点当前的 style** 比 `kLayoutKeys`（render.cpp:59） |
| 5a | 布局键变了 | `dom_dirty.push_back(el)` → 走 DOM-dirty 段：`whaleui_layout_relayout`（子树重建 + **全树盒 pass**） |
| 5b | 布局键没变 | `whaleui_layout_relayout_style`（只重建子树，**复制旧几何**，不跑盒 pass） |

`:hover` 会沿祖先链冒泡匹配（`style.cpp`），这是 CSS 语义，不是特例。

## 两个根因（都已修）

### 1. 重排路径缺文本度量上下文 `g_metric_render`

`render_text_metric`（文本宽度 hook）需要一个渲染器上下文：

```cpp
whaleui_render_t* r = g_metric_render;
if (!r || !utf8 || len == 0) {
    return 0;   /* -> layout 侧回退 0.6em/字符的估计宽度 */
}
```

`g_metric_render` 只在帧内**全量布局分支**前后成对设置/清空
（`render.cpp`：`g_metric_render = r;` … 清空）。而 **state relayout 段**与
**DOM-dirty 增量重排段**在它**之外**，于是那两条路径里：

- 真实字形宽 251px 的 `"WhaleUI Demo"` 被算成 **231px**（`12 字符 × 0.6em`）；
- flex 的 min-content floor 跟着变小，h1 的可用宽被压到 231px；
- 文本真宽 251 > 231 → **换行成 2 行**（run 高 43 → 86）；
- `header` 高 71 → 104，页面高度 +25，**下面所有元素整体下移**。

`temp/probe_hover.cpp` 的整树 diff 里就是这一行：

```
#7 h1#text  xy=(32,45)->(32,45) wh=(251,43)->(231,86)  WhaleUI Demo
```

修法：两条重排段各自在段首设 `g_metric_render = r`，段末清空
（与既有风格一致，各段自洽，不依赖全局顺序）。

### 2. `display` 归一化被当成"布局变化"

`layout.cpp` 的 build 会把若干 inline 盒归一成 `inline-block`：`<select>`、
以及**没有显式 width 的 contenteditable**（给它 `width:12em` +
`display:inline-block`，否则打字会横向长）。而第 4 步比较的是**重新 cascade
的原始值**：

```
[dbg] key=display old='inline-block' new='inline'
```

`inline` ≠ `inline-block` → **每次 hover 都判成"布局变化"** → **白跑一次
全树盒 pass**。这既是性能问题，也是把根因 1 放大成"hover 必跳"的原因。

修法：新增 `same_layout_display()`（render.cpp），把 `layout.cpp`
`display_kind()` 视为同一类的字符串对当作相等 ——
`inline|inline-block`、`flex|inline-flex`、`grid|inline-grid`、
`table|inline-table`。两条重排路径共用 `layout_key_differs()`。

## 复现 / 验证（探针，不入库）

`temp/probe_hover.cpp` 会加载页面、跑 10 帧取基线、再 hover 到
`contenteditable` 的中心、再移开，每次打印**全树** `xy/wh` 的 diff，外加
`root.scroll_y / scroll_max / geom_relaid`：

```powershell
cd C:\aaa\whaleui
# target 只在 temp/probe_hover.cpp 存在时定义（xmake.lua 末尾）
xmake b -y probe_hover
& .\build\mingw\x86_64\release\probe_hover.exe            # 默认 temp/demo_full.html
& .\build\mingw\x86_64\release\probe_hover.exe "file://C:/aaa/whaleui/temp/rep_state.html"
```

修复后的期望输出：`baseline -> +8 frames (NO hover)` 与
`baseline -> hover span` **都只剩进度条动画那一行**（`.anim-bar` 的 width
动画在跑），`scroll_max` 全程不变。

修复前（或缺任一处修复时）会看到 h1 与它下面几十个节点一起变。

**探针里注意**：不要为了"制造重排"而直接调 `whaleui_layout_relayout_geometry()`
—— 那绕过了持有度量上下文的 render 帧，会得到一个**假**的差异（本目录
`temp/rep_state.html` + 一次受控的 hover 才是有效对照）。

## 回归测试的边界

`tests/test_render.cpp` 有一条"交互状态不得移动布局"的测试：hover 一个
contenteditable span，然后逐节点比对 hover 前后的 `border` 四元组必须完全
相同。它锁的是**不变量**（paint-only 的状态变化不动布局）。

注意它**单独抓不到上面两个 bug 中的任何一个**：两者会互相掩盖 ——
只缺度量上下文时，小页面的 hover 未必走到那条盒 pass；只缺 display 归一化
时，那次盒 pass 又有正确的度量。两个都回归时它才会红。**完整的复现靠
`temp/probe_hover.cpp`**，别把这条测试当成那两个 bug 的唯一保险。

## 相关文件

- `src/render/render.cpp`：`kLayoutKeys` / `same_layout_display` /
  `layout_key_differs` / state relayout 段 / DOM-dirty 段 / `g_metric_render`
- `src/layout/layout.cpp`：`display_kind`、`estimate_content_width`、
  `relayout_impl`（`style_only` 与否）、contenteditable 的 `width:12em` 归一
- `src/animate/animate.cpp`：paint-only vs layout 动画的判定
  （`whaleui_anim_needs_layout`，见 4150 附近的注释）
