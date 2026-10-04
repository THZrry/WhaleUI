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

# <5>: transform: rotate 的文本/子树仍不转

UNRESOLVED(范围再缩小)

rotate 现在能被解析(deg/rad/turn)、参与插值、写进 `whaleui_transform_t.rot`,
并且**背景/边框随 rotate 真实旋转**(<15> 同轮):GPU 路径把 solid quad 的四个
角点绕中心旋转(whaleui_gpu_rect_rot),u/v/size/radius 留在盒子局部坐标系,
仿射不变性让 SDF 圆角在任意角度下保持精确(圆旋转还是圆),零额外
纹理/draw,光栅化器免费做旋转;CPU 路径回退未旋转盒。spinner 这类纯色/
border-radius 圆环因此可转。@keyframes spin 的逐帧插值也随 <14> 的 xf
刷新生效。

<16> 同轮补齐两处(性能仍是近似解,零新增纹理/draw):
渐变轴随 rot 旋转(fill_gradient 新增 rot_deg 参数,把投影方向转过同样角度,
band 位置照旧从盒角投影来,CSS 语义"渐变随元素转"只需几次 ALU);
命中测试逆旋转重测(hit_test 在 laid-out 盒未命中时,把点逆旋转回局部系再测
一次,门控在 xf 标志 + rot!=0,菱形边角因此可点)。

仍不转(近似边界,均为 ponytail):文本与子元素保持直立;
`::before` 状态层不随转。要全部转需要真矩阵
paint 路径(背景/边框/文本/子树走同一变换),成本远超本轮。

其余未实现项(`conic-gradient` / `repeating-*-gradient` / `hwb()` /
`oklab|oklch|lab|lch` / `color-mix()` / `attr()` / `counter()` /
`::first-line` / `:has()` / 容器查询 / `filter` 与混合模式 /
`backdrop-filter` 的 CPU 回退路径)逐条列在
`doc/internal/16-engine-support.md` 末尾。

# <16>: backdrop body 丢失 + inset 新实现 + param buffer 上溢 + rotate 渐变/命中

SOLVED

三件事 + 一个回归,一起落地:

**(1) backdrop-filter 的 body 丢失。** `paint_node` 的 backdrop 分支为了让
compute pass"覆盖式替换"不冲掉 body,把整个 body 绘制(纯色+渐变+边框)跳过,
只传一个单色 `bg_c` 给 shader 做盖顶。纯色场景没事,渐变背景和边框在 GPU
模式下全走 solids——命中 backdrop 就从没发射过。修:backdrop 不再跳 body,
照常发射但路由到 LATE 队列(`whaleui_gpu_late_begin/end`),flush 在 pass C
与 pass A2 之间多一次 LOAD 不清的渲染(geometry → backdrop 模糊 → late
body → inset)。另补 radius:shader 里对区域做 SDF 圆角裁剪(圆角外保持原像素,
body 的透明角露出真实 backdrop)。
回归:tests/test_scroll.cpp 的 backdrop-body 用例——渐变 body 盖在模糊白页上,
顶红底蓝(被冲掉会是白色)。

**(2) inset box-shadow 新实现(用户 7 步方案的 mask 反演替代)。** 4 象限梯度权重/
接缝混合/双层裁剪的每步直译成本太高(4 套权重函数 + 拼接 + 重叠混合 + 双 mask):
7 步的语义等价形式是"1 - 覆盖盒 mask(px - offset)"——移采样点精确实现 offset
(正 ox 点亮左缘,CSS 方向),mask 自身边界截断Shadow(干净中间天然为零,替代
"内缩 blur 半径挖空"),圆角用同一 SDF 做显式裁剪。`whaleui_gpu_inset` 新签名
`(x,y,w,h,radius,blur,ox,oy,spread,color)`:mask 源是 spread-收缩白盒
(kBlurDiv 缩放,圆角 radius-spread),kInsetCS 步长改 4 float4
`{x,y,w,h}{blur,r,g,b}{a,ox,oy,spread}{radius,0,0,0}`。`blur:0` 合法(采样
level-0 脆 mask,出实线环)。CPU 路径同步:硬环画四条边(带 offset),模糊环按
offset 移层。旧四三角形路径与 kFlatPS 整条删除。
回归:test_scroll 的 inset-shadow 用例——硬环(rim 红中绿)/offset 20px(仅左侧红)
/模糊 14px(上缘雾化、中绿)。

**(3) 外阴影 mask 采样位移(自引入,同 <15> 的 backdrop 修法)。** kShadowCS 的
`uv = px/fb * 0.5` 把形状采到 px/4,柔边位移 2 倍。修:`uv = px/fb`(形状按
px/kBlurDiv 推送,pass 视口即 blur_tex)。经基线对照确认改动前后主题一致。

**(4) param buffer 上溢回归(本轮引入、本轮修)。** inset 改 16 floats 后曾把三
个 buffer 统一改成 1040B,而 shadow/backdrop 的 12-float 记录在 material 页
(34 条,1648B)越界 `UploadToGPUBuffer`——D3D12 报 `Failed to close command
list 0x80070057`,整帧黑屏(material 深浅双崩,其余主题记录少未触发,Vulkan
不报错)。修:各 buffer 按自家跨度独立定尺寸(4096/4096/5120)。定位过程:
Vulkan CLEAN + solids 截断/各 pass 禁用均仍 ERROR,最后数 param 字节抓到。

rotate 近似(同轮,性能优先):渐变轴随 rot 旋转(fill_gradient 新增 rot_deg 参数,
投影方向同转,零新增纹理/draw);hit_test 逆旋转重测(xf 门控)。文本/子树/
`::before` 仍是近似边界,见 <5>。`transform-origin` 在本轮 <17> 落地,
不再是边界。

验证:10 项测试全绿;14 张主题离屏渲染像素自检(深浅配对+text/surface)全 OK;
探针 temp/mini*.html 分段二分过程保留在 temp/。

# <17>: P0 缺口四项(transform-origin/:not()/::selection-fg/伪元素去名单)

SOLVED

`temp/css-todo.md` 建清单(P0 四项 + P1 分类),P0 逐项落地:

**(1) `transform-origin`。** 新增 `whaleui_transform_origin_eval`
(`animate.cpp`):关键字/%/px,单值/双值,`top left` 换位容忍,缺省 50% 50%。
scale 锚点从居中改 `ox*(1-s)`(paint_node 的 nox/noy);rotate 锚点透进
`whaleui_gpu_rect_rot` 新参(ax/ay,framebuffer 锚点,角偏移先换到锚点系再转,
默认中心退化为旧式);hit_test 逆旋转用同锚点。fluent 下划线 `scaleX(0→1)`
从左侧展开。
回归:test_scroll 的 transform-origin 用例(左锚 scaleX(0.5) 占 x=20..70)。

**(2) `:not()` 单层。** `SelPart` 加 `has_not/not_sel` + 状态取反
`not_hover/not_active/not_focus`(`style.cpp`):伪类扫描改括号感知
(`.`/`#`/`:`/`[` 只在 depth 0 终止——`:not(.a)` 的 `.` 不截断);
内层裸状态走外层 match() 同冒泡取反,其余静态取反走 part_match 递归
(剥状态保保守);复杂参数(含组合子)丢弃。aero 的 `:not(:hover)` 慢出恢复。
回归:test_style 静态+状态断言。

**(3) `::selection` 前景色。** `selection-fg` 虚属性:页面规则赢主题(从后往前
扫,两色分头找全才停),`var()` 展开(主题用变量);光栅循环逐字形换色
(`L.map[ci]` 判选中字节区间,字形缓存本就按色 key,不分裂;run 光栅选中时
跳缓存——命中分支在前,光清写入 key 不够)。踩坑两处:用例误插进 `#if 0`
历史块(预处理 246→1670 行消失,查 pp.ii 定位);主题规则排页面前被
`break` 误取。
回归:test_style 白盒(两色搬运)+test_render selfg 像素(red=557 black=0)。

**(4) 伪元素去名单。** `pseudo_box_style` 全量合并伪规则声明(只跳 `content`);
成盒判定改"有 content + 有其他声明"(纯文本伪元素仍走行内文本 run,语义不变);
`kPseudoBoxProps` 删除;checkbox 的 `::after` 本就有 `nc_el` 保护。
回归:test_scroll pseudo-props(绿边框+红蓝渐变)。

验证:10 项测试全绿。清单 `temp/css-todo.md` P0 全勾,P1 待排期。

# <18>: CSS 缺口四批(追加式,零热路径改动)

SOLVED

`temp/css-todo.md` 建清单,按"小→中"四批落地,全部追加语义(无行为回归):

**批1(小):** `animation-play-state`(冻结/恢复双路径);`text-stroke` 分写;
`::placeholder{color}`(虚属性+两处读取);`overflow-x/y` 分写(三处);
`object-position`(拟合后定位)。附带修:object-fit 轴选反
(`(cover)==(ar>dar)` 全反)+RGB24 无 alpha 蓝丢(转 RGBA32)。
回归:anim 暂停用例 + test_scroll batch1(phred/stblue/overflow 裁剪)
+ `temp/objpos.html` 探针(左红右蓝)。

**批2(小·选择器):** `:enabled/:required/:optional/:empty/:link`,
`[^$*~|=]` 六档(attr_op),`content:attr()`。回归:test_style 白盒 +
test_scroll attr 像素。

**批3(中·单位/函数/选择器):** `rem/vmin/vmax`(双 resolve 点),
`min/max` N 元,`calc` 乘除(注释避 `*/` 坑),`radial` size 关键字
(`radial_resolve` 按盒解算,顺手修 rcy 绝对值分支),
`:focus-within`(match 外层+全重绘后祖先链补 cascade),
`:is/:where`(单层逗号)。回归:layout 数学用例 + style 白盒 +
test_scroll batch3(closest-side 红心+form 变绿)。
踩坑:focus 采样点落在 input 框内(改采右下角);sticky 测试期望误读
(见批4)。

**批4(中·行为):** 多过渡 per-property timing(`TransItem`+longhand 循环铺,
修 longhand 丢 items 的回归);sticky bottom/right 改相对视口远边
(之前误对 parent 底)+容器钳制改文档坐标;`scrollbar-width/color`;
`aspect-ratio`;`::marker` 桶+`list-style-type`(square/circle/alpha/roman)。
回归:anim 双时钟 + test_scroll batch4。
踩坑:sticky 测试设计三改才对(bottom 语义是"不被推出视口",非"吸底";
parent 底误作视口底)。

验证:10 项测试全绿;14 张主题自检全 OK。

# <19>: filter(元素滤镜)GPU pass E + CPU 颜色矩阵

SOLVED

复用 backdrop compute 模式,但作用于元素自身合成后内容:
`parse_filter`(render_color.cpp)合并 4x5 矩阵(CSS 顺序左乘)+blur 半径;
kFilterCS(6 float4 stride):mip blur(composite 后 blit target2→blur_tex)+
矩阵+圆角 SDF;pass E 在 composite 后读写 target2;记录按 blur 外扩(晕出盒);
CPU 只做颜色矩阵(blur 跳过,与 backdrop 一致)。
支持:blur/brightness/contrast/saturate/grayscale/invert/sepia/hue-rotate/
opacity,未知 op 跳过其余照算。
踩坑两处:记录 28→24 floats(stride 7→6,bias 进每行 w);blur 晕只在盒内
(记录须外扩)。
附带修:外阴影 pass 被之前插桩 `false &&` 关掉忘恢复(一直关着,本轮恢复)。
回归:test_scroll filter(灰/亮蓝/品红/粉晕)。
验证:10 项测试全绿;14 张主题自检全 OK。

# <20>: transform skew/matrix/3D(顶点剪切近似栈)

SOLVED

延续 rotate 近似栈(零新增纹理/draw):skewX/Y 精确到顶点(剪切绕同锚点,
tan 钳 ±10,hit 逆剪切行列式判奇异,渐变投影协变);matrix(6 数)分解为
translate+scale+shear 折角(atan 小角近似,工具生成的 scale/translate 矩阵
精确);rotateX/Y 降级为 scaleY/X(cos)透视压缩;rotateZ=rotate;
perspective/translateZ/scaleZ 消费忽略(不再杀整串)。
踩坑:perspective 前缀数错(12→13→12,python 才数对)+分支漏 `p += N`
(parse_num 在函数名上失败,加 dbg 打印抓到)+xmake 缓存 stale(删 obj)。
回归:anim 求值白盒(skew/matrix/rotateX-Z/perspective穿透)+test_scroll
skew 像素(顶边不动/底边右移/matrix 平移)。
验证:10 项测试全绿;14 张主题自检全 OK。

SOLVED

两件事,一起修/落地:

**(1) GPU backdrop 采样错位 —— 历史 bug,首测即现形。** 离屏探针页
(左红右蓝两半 + `backdrop-filter:blur(4px)` 覆盖盒)上,模糊区域整体变成
纯红:shader 的 uv 是 `px/fb * 0.5`,这是从 shadow 路径抄的约定 —— shadow
的形状顶点在光栅化前已除以 kBlurDiv(gpu.cpp push 时 `x/kBlurDiv`),
uv*0.5 与之自洽;而 backdrop 的源是**全屏 blit**(geom_cur -> blur_tex),
fb 像素应对应 `uv = px/fb`。乘 0.5 等于只采样帧左上四分之一,覆盖盒恰好
落在那里就整片错色。基线(stash 改动后)复现同样的红 → 确认是自引入即坏,
不是新回归。修:kBackdropCS 去掉 *0.5(kShadowCS/kInsetCS 不动,约定不同)。

**(2) blur 双档近似(用户方案)。** 原来所有半径都走 mip 阶梯,小半径档位
不足。现抽成共用 `blur_sample`(gpu_shaders.h,三个 CS 同用):
- `blur >= 8`:原 mip 阶梯(3 tap 跨相邻层,level L 等效半径 ~2^(L+1) px);
- `blur < 8`:**预定义 3x3 二项式卷积核,作用在单个 mip 层上**。
  层取 octave 下整 `floor(log2(blur))`(夹到 2),该层一个 texel 跨
  2^(L+1) px,核的 sigma(1 texel)正好等于该 octave 顶端的半径;octave
  内按 `t = log2(blur)-L` 从恒等(t=0,blur<=1 不糊)线性升到全核
  (t=1),权重和恒为 1(中心 1-0.75t / 边 0.125t / 角 0.0625t)。
  9 次 SampleLevel、无循环、层内双线性平滑,1..8px 单调且在 8px 处与
  阶梯无缝交接。

回归:tests/test_scroll.cpp 的 backdrop4 用例 —— 边界两侧采样
(l=FFBF0040 / r=FF4000BF,两通道互混)、远处保持纯色、f 外不受影响;
rotate-diamond 用例见 <5>。探针页 temp/backdrop_probe.html +
render_shot + temp/rect_color.py。验证:10 项测试全绿,fluent 主题
自检(shadow 与 backdrop 同用 blur_sample,无退化)。

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
