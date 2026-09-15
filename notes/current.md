# <2>: 主题样式与参考截图的肉眼比对未做

UNRESOLVED

<1>(主题 CSS 重写)与 <4>(引擎缺口修复)的编译与配对自检已完成,见
notes/index.md。剩下的是"像不像参考"这一层:本机没有图像理解能力,只能做
像素统计,肉眼比对要人工做。

## 已完成(证据)

- `xmake b -y` 全量构建通过(三个库 + demo + 10 个测试 target);
  **10/10 测试套件 exit=0**(含删 classic 后调整过断言的 `test_style`)。
- 离屏渲染逐套主题 × 深浅两模式,`python temp/theme_check.py` 全部通过:
  浅色模式主色亮度 0.94~1.00(白/浅灰基调),深色模式 0.07~0.23,
  text/surface spread 0.44~0.96(文字与底不同向)。
- 控件颜色与 token 精确一致:material primary `#6750a4` 91.5%、
  aero primary `#3c7fb1` 85.4%、fluent primary `#0067c0` 86.9%。
- 按钮高度按主题生效:fluent 32 / material 42 / aero 25 px。
- 引擎缺口的**修复前**原始实测数据留在 `temp/engine-gaps.md`(探针页
  `temp/gaps*.html`);修复后的支持矩阵在 `doc/internal/16-engine-support.md`。

## 未做

- 与 `temp/Themes/Fluent/FluentUI1..3.png`、`temp/Themes/MaterialDesign/*.png`
  等参考截图逐张肉眼对照(间距、圆角、字重、状态层次、动画观感)。

## 复现命令

```powershell
cd C:\aaa\whaleui
# render_shot 的 target 只在 temp/render_shot.cpp 存在时才定义(见 xmake.lua 末尾)
xmake b -y render_shot
foreach ($t in @('fluent','metro','material','aero','gtk','macos','browser')) {
  foreach ($m in @('light','dark')) {
    .\build\mingw\x86_64\release\render_shot.exe `
      "file://C:/aaa/whaleui/temp/showcase.html" "temp/g_${t}_${m}.ppm" $t $m 720 nodump 5 0
  }
}
python temp/theme_check.py temp/g_fluent_dark.ppm
python temp/rect_color.py temp/g_material_light.ppm 68 232 155 264

# 引擎探针:dump 出每个节点的 tag/xy/wh,再按像素判决
foreach ($p in @('gaps','gaps2','gaps5','gaps7','gaps18','gaps21')) {
  & .\build\mingw\x86_64\release\render_shot.exe `
    "file://C:/aaa/whaleui/temp/$p.html" "temp/$p.ppm" browser light 400 dump 5 0
}
```

拿某主题的元素坐标:`render_shot ... <style> light 720 dump 5 0`(stderr 打印
每个节点的 tag/xy/wh)。

## 回归清单(缺口修复后已核)

`temp/engine-gaps.md` 的 P0/P1/P2 落地后重渲染并确认:

1. [x] `theme_css.cpp` 里 material / aero 按钮的
   `display: inline-block; line-height: var(--btn-h); /* needs: flex-rel */`
   已删除,恢复参考的 `inline-flex` + flex 居中;
2. [x] material 按钮的 `overflow: hidden` **不进基础样式** —— 引擎层面能写,
   但 overflow≠visible 的元素在 flex 行里 min-content 归零(规范行为),
   拥挤的一行会把按钮压成 1px(实测),ripple 自己按需加;
3. [x] state layer(`::before` 百分比尺寸)、aero 四边异色输入框、
   metro/aero 的 dotted outline、各主题双层阴影都已出现;
4. [x] `h1..h6` 三值 margin、`blockquote`/`dd` 四值简写恢复预期留白。

# <5>: transform: rotate 只解析、不绘制

UNRESOLVED

rotate 现在能被解析(deg/rad/turn)、参与插值、写进 `whaleui_transform_t.rot`,
同一条 `transform` 里的 translate/scale 不再被它带崩(此前 `rotate()` 会让
整串 `transform` 返回失败)。但**渲染器仍按未旋转的盒绘制**:b3/b4 那种
`::before` 状态层、spinner 的 `@keyframes spin` 都不会转。要真正生效需要一条
矩阵 paint 路径(背景/边框/文本/子树走同一变换),成本远超本轮其它项。

其余未实现项(`conic-gradient` / `repeating-*-gradient` / `hwb()` /
`oklab|oklch|lab|lch` / `color-mix()` / `attr()` / `counter()` /
`::first-line` / `:has()` / 容器查询 / `filter` 与混合模式 / CPU 路径的
`backdrop-filter`)逐条列在 `doc/internal/16-engine-support.md` 末尾。

# <13>: 子节点"分块/树"存储 —— 收益点与正确切口(未做)

用户方案: 子节点用数组块, 块间连成树; 不设阈值, 小的不生长; 对外 first_child/
next 语义不变(读点 ~280 处全部保住, 写入点只有 ~10 处)。补充指导: 不做严格长度
控制, 超过/不足一定数量自然裂解/合并; 大小不靠遍历, 元素脏了再更新并逐层向上冒泡。

实测前提(重要):
- 布局节点在 `std::deque<whaleui_layout_node_t> arena` 里(layout.h:144), 同一父元素
  的子节点按分配顺序相邻 -> `first_child`/`next` 遍历本来就是顺序内存访问, deque
  已提供分块。分块对遍历 cache 局部性几乎无增量收益。
- **真正的收益在遍历次数**: 父元素挂几万个孩子时(agent 聊天历史、wpt-css index
  每个 summary 下几十到几万个对象), viewport 只需要"合并后的位置和大小"。按块存
  合并盒 -> 遍历从 O(子节点数) 降到 O(块数)。
- `bounds`(子树合并盒)与 `r->bounds_valid` **已经存在**且已经惰性: 滚动帧不重算,
  只有"几何变了"才 `compute_paint_bounds`(O(n) 后序一次); paint-only 的改动走
  `dom_repaint` + 局部 `compute_paint_bounds(nd)`。

已尝试并回退: 把 `bounds_valid` 全树标志换成逐节点脏位 + 冒泡
(`mark_bounds_dirty` / `mark_bounds_dirty_all`, 布局节点加 `bd` 位)。
`test_scroll` 抓到它破坏了 hover 的部分重绘 —— 设了 `bounds_valid = 1` 之后那一帧
不会再重算, 刚改样式的盒子被剔除逻辑剔掉。结论: **bounds 的失效机制已经是对的,
别再动它**。

正确切口(下次实现): 在**遍历**里用"块的合并盒"跳过 —— 给父元素的子节点分块
(每块 N 个, 松弛分裂/合并), 每块维护一个合并盒; `paint_cull` / `hit_test` /
`compute_paint_bounds` 的循环按块判断, 不相交就整块跳过。这与 `first_child`/`next`
语义兼容(块内仍是 next 链), 写入点只有那 ~10 处。