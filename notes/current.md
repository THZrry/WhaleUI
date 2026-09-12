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
