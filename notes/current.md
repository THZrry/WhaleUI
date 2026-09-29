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

# <13>: 子节点"分块/树"存储

SOLVED

用户方案: 子节点用数组块, 块间连成树; 不设阈值, 小的不生长; 对外 first_child/
next 语义不变(读点 ~280 处全部保住, 写入点只有 ~10 处)。已落地:

- 基础设施(9a43a81): layout.h 的 `WUI_CHUNK=64` + `whaleui_child_chunk`
  (块内首尾指针 + 合并盒), `chunk_arena` deque 提供稳定地址; 追加子节点时
  自然裂解, 无严格长度控制。
- paint 按块跳(48d7a49): 100k 兄弟 11-14ms -> 2-4ms/帧; hit_test 同一套
  块盒判断且无 cull margin(9851186, 命中必须精确)。
- 切段/续建配套: 续建点携带 run_marker_done/summary_seen(594ec5e)、切段
  预算通用化到任何元素(fb4230d)、compute 接受首帧切段预算(ec2bb8b)、
  首帧只建视口 + worker 后台补齐(9383dca)。

保留的实测结论(别再踩):

- **分块的收益在"遍历次数"不在内存布局**: 布局节点本就在
  `std::deque<whaleui_layout_node_t> arena` 里, 同一父元素的子节点按分配
  顺序相邻, 顺序遍历的 cache 局部性已经够 —— 块的合并盒才是省时间的部分
  (viewport 只需要"合并后的位置和大小", O(子节点数) -> O(块数))。
- **bounds 失效机制的现状与铁律**: 最初把 bounds_valid 换成逐节点脏位的
  尝试错在两处(祖先的脏没人消; 4517 段设 bounds_valid=1 又把帧内根重算
  拱住 -> 剔除用了陈旧 bounds), test_scroll 抓到 hover 盒子被剔掉。
  d8cbcfe 以修正版落地: 标脏只走冒泡(mark_bounds_dirty), "所有盒子都动了"
  的全量帧用 mark_bounds_dirty_all; 重算从根开始、干净子树早退; 布局节点
  带 bd 脏位, 创建时置 1; visible=0 也冒泡标脏。效果: 局部重排不再整树
  重算(40k 节点滚动 4-6ms/帧)。**铁律不变**: bounds_valid=1 的语义是
  "本帧不重算", 任何动失效机制的改动必须先过 test_scroll 的 hover 用例。

# <14>: xf 预计算标志与动画逐帧写 style 脱节 —— transform 过渡不上浮

SOLVED

现象: hover 用 transform 过渡(`transition:transform` + `:hover{transform}`)
时卡片完全不上浮; 同一页面去掉过渡就正常。实测期间(ced305b 调试)已有直接
证据: 有节点 transform 非 none 而 xf=0。

根因: xf/fx/sk 是 paint 路径的预计算位(build 时每节点缓存一次, 让 paint 读
位而不是每帧 std::map 查找)。此前全项目只有 Builder::build 一个写点, 而
transform 过渡的注册(apply_transition)会把 style 钉在中性值 "none"(插值
起点), 之后插值只经 `whaleui_anim_apply_ov` 逐帧写进 nd->style —— 不经
build, xf 永远停在 0:

- paint_node 的 `has_xf` 门不开 -> 永不位移(渲染_paint.cpp:1059);
- 状态重排的脏条带累加同样读 xf -> 漏掉位移后的位置(render.cpp:4862);
- 动画脏区累加器(4907 分支)没事, 它直接读 style。

修法(通用机制, 不做局部特调): 把 build 里的标志块抽成
`whaleui_layout_node_refresh_paint_flags`(layout.cpp 定义 + layout.h 声明),
build 每节点照旧调用; render.cpp 的 find_anim 在 `whaleui_anim_apply_ov`
之后对被动画节点调用同一次(与紧随其后的 opacity 逐帧刷新并列)。
fx/sk 不能经 paint-only 动画到达(transform/position 里只有 transform 不在
is_layout_props, position 变化会走 needs_layout -> relayout -> build),
刷新它们只是维持"单一写点"不变式。

回归: tests/test_scroll.cpp 的 hover-transform-transition 用例 ——
`#b{transition:transform 0.2s}` + `:hover{transform:translateY(-40px)}`,
hover 后轮询帧直到盒子上方采样带变色(0.2s 过渡, 1s 预算, 抗时钟抖动);
**修前 lifted=0 断言失败, 修后 lifted=1**(均实测)。
验证: xmake b -y 通过; 10 项测试全绿。
