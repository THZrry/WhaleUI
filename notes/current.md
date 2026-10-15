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

# <21>: 混合模式(background/mix-blend, 11种精确+4种近似)

SOLVED

无 element 隔离(全帧只有 target 系),走两条轻量路:
background-blend 在 fill_gradient 发射前折角颜色(GPU 零新增 draw,CPU
逐像素 blend_pixels);mix-blend 走 pass F compute(body 单色 vs painted
frame,Porter-Duff,子树不参与,文档写明)。
11 种可分离精确+dodge/burn/soft-light 全公式;hue/saturation/color/
luminosity 亮度混合近似。CPU/GPU 共用 blend_pixels/blend_ch 逻辑。
踩坑:mixblend pipeline num_samplers 对不上(D3D12 E_INVALIDARG,加空 sampler
声明匹配签名)+Load 同 texture 读写(改走 out_tex,去 frame_tex 绑定)+
测试 CSS 简写覆盖底色(改 background-image 分写)。
附带修:resize 的 text_layer 格式 B 序(与 create R 序不一致)。
回归:test_scroll blend(双纯黑)。
验证:10 项测试全绿;14 张主题自检全 OK。

# <22>: clip-path / mask-image(裁剪/遮罩管线)

SOLVED

clip-path: inset(TRBL/mirror, round 角)/circle(closest-side 缺省)/ellipse
解析为盒内形(parse_clip_path, polygon 解析但普通绘制);顶点尾字段
(ctype/cc/cr/crad, quad 局部坐标, 随 rotate/skew 同变换)+ solid shader
第二 SDF 相交(双形取 max, 1px AA);CPU inside_clip 镜像;
paint_node 压栈/弹栈(含 overflow 早退弹栈), 子树继承;
solid/round/rot/gradient-linear/gradient-radial(逐层 quad 局部重映射)/
blend_surface(门控逐像素)/文字直写循环(门控逐像素)全发射器接入。
mask-image(含 -webkit- 别名):linear-gradient alpha 遮罩, 按 fill_gradient
同轴投影取四角 alpha(mask_from_linear), 栈式压栈/弹栈(子树继承);
发射器按逐角 alpha 相乘 —— solid/round 走 gradient_rect(给 gradient_rect
加 radius 尾参, 一次 draw 同时带圆角 SDF 与逐角 alpha; 之前两 draw 相叠
写法是错的, src-over 不相乘)/gradient-linear 四角 tint/gradient-radial
逐层中心采样/图片+文字逐像素; CPU 全路径逐像素相乘。非 linear 遮罩解析
但跳过(不透明绘制, 不杀整串)。热路径门控: 无 mask/clip 时 blend_surface/
文字循环零新增检查(needs_clip/use_mask 两分支)。
踩坑:mask 像素断言写错通道 —— 浅色底上红混合后 R 一直高, 衰减信号在 G
通道(l=0D/m=7C/r=EA); 测试要按 G 通道判。另 xmake 增量在 stash 切换时混
obj 致 test_style 误报, 重编即过(与本轮无关)。
回归:test_scroll [clip](inset 中红/边裁掉/圆心蓝/角背景)+[mask]
(l=FE0D0D/m=FA7C7C/r=F5EAEA 渐隐梯度 + 不透明 mask 蓝盒完好)。
验证:10 套件全绿(test_scroll 含新用例); 14 张主题自检全 OK。

# <23>: 颜色与渐变值(hwb/lab/oklab/color-mix/conic/repeating)

SOLVED

render_color.cpp 内追加, 零热路径改动(无 mask/clip 时新增分支不执行):
颜色: hwb(HWB→满饱和扇区+白黑混合, w+b>1 按比成灰); lab→XYZ→线性 sRGB,
oklab→LMS 立方→线性 sRGB(规范矩阵照抄), lch/oklch 极坐标先转 a/b;
color-mix(in space, c1 p%, c2) 递归解析(权重缺省互补/0.5), srgb/srgb-linear
精确, hsl 短弧插值, oklab 走线性近似(文档写明)。空格分隔参数靠
split_color_args 同时按逗号/斜杠/空格顶层切分(旧版只认逗号斜杠, hwb/lab
的空格写法整体落到一个 part 直接 -5)。
渐变: Gradient type 加 2=conic(from 角+at 中心复用 cx/cy)/3=rep-linear/
4=rep-radial(rep-conic 按普通 conic 解析不杀串); opener 长度逐 kind 算
(repeating-conic 25 易错抄成 15, 单列 kind=5)。CPU: conic 逐像素 atan2
角度→t(中心取首 stop); rep-linear/rep-radial t 负安全取模。GPU:
rep-linear 轴向(|cos|>0.99)按 stop 段切分条带(每段起止色即 stop 色, 硬条纹
精确, 一次提交多 push, 总段 cap 8), 对角回退整盒取模角(近似, 文档写明);
rep-radial 10 层取模; conic 无近似(角度→色非双线性可表, 马赛克拼扇会糊 pie
边), 留底色, 主题零用量。
踩坑三处: (1) stop 全显式时旧 auto_pos 逻辑无条件均匀重写(中间 stop 全错,
repeating 硬条纹变过渡带) —— 改全 auto 才均匀, 部分 auto 逐个补, 全显式
保持; (2) conic 分支条件 `if (!lin)` 把 conic 吞进 radial( elses if 链,
改 `!lin && !conic`); (3) oklch 的 polar 判定抄错下标(s[2] 恒为 'l',
改 okl 前缀判 s[3])。
回归:test_scroll [colors](hwb 红绿精确/lab 中灰 777777/color-mix 紫
800080/oklch 砖红 + repeating 红蓝红蓝硬条纹)+[conic] parse 白盒(type/起角/
中心/rep-conic 降级)。
验证:10 套件全绿;14 张主题自检全 OK。

# <24>: content:counter()/counters()(计数器作用域)

SOLVED

layout.cpp 内追加, 与渲染热路径无关:
scope 栈随 build() 递归嵌套(Builder 成员, 每次 build 从空栈起, 增量
relayout 无陈旧值; resume 是整树重建, 早退帧丢弃, 无需续接)。
counter-reset/set 压层(父副本+赋值, 缺省 0), increment 改栈顶(缺省 +1,
无则隐式建), RAII guard 配对弹栈(含 budgeted 早退)。::before 在 enter
之后组装, 看到递增后值(浏览器行为, li 从 1 起)。
取值: counter(name[, style]) 取栈顶(无则 0); counters(name, sep[, style])
连接所有含名层(嵌套 "1.1" 是规范行为, 不是 bug)。样式 decimal 缺省,
alpha/roman 复用 marker 转换(抽 counter_text, marker 原代码保留)。
content 混合 token(`"第" counter(ch) "章"`)按顶层空格切分展开(引号/attr()/
counter() 三态)。
踩坑: 纯引号 fast-path 首尾引号误判混合 content(`"A" counter(x) "B"`
首尾恰为引号, 剥成 `A" counter(x) "B`) —— 加内引号检查才走 fast-path。
附带影响: stop 全显式修复后 batch3 中心 E6(旧 0.5/0.75 错位碰巧纯红,
GPU 10 层最小 t=0.1 的固有粒度; 断言仍过, CPU 精确)。
回归:test_layout [counter](外层 1./2. + 嵌套 1.1-i + 混合 A0B + set 41 白盒)。
验证:10 套件全绿;14 张主题自检全 OK。

# <25>: :is/:where 权重 + @media 剩余条件 + contain

SOLVED

三块小改, 追加式:
权重: sel_specificity 括号感知 —— `:where()` 整段跳过(零权重, 连冒号都不计),
`:is()`/`:not()` 取逗号分支最大权重(递归 sel_specificity, better 比较)。
附带修根因级 bug: emit_style_rule 按裸逗号切选择器, `:is(#y, .x)` 被拆成
`:is(#y` + `.x)` 两条坏规则(cascade 里 :is 多参从未生效, match 白盒因直传
字符串才过) —— 改括号深度感知切分。
@media: min/max-height、orientation(视口比较)、aspect-ratio(含 min/max,
±0.01)、hover/pointer(桌面常量 hover+fine)、resolution(96dpi 基准,
dppx/dpcm 换算); media_ok/filter_media 加 viewport_h 尾参(缺省 -1,
height 门控 fail-closed), window.cpp 传入 win->height。未知条件仍默认匹配。
contain: paint/strict/content 含即按 overflow:hidden 裁剪 —— paint 侧 eff
+ layout 定高钳制(visible 拉伸改不拉伸)。layout/size/style 无实现 no-op。
isolation/will-change 解析保留(cascade 无白名单)、零消费(文档写明)。
踩坑: contain 初版包在 `if (ov.empty())` 里, UA 的 `overflow:visible` 显式值
绕过检查 —— 改 `ov.empty() || ov == "visible"`; 更深一层是父盒被子撑高致裁剪
盒同步变大, layout 钳制处也要认 contain(调试打印 box=100x90 抓到)。
回归:test_style 级联白盒(:is green/:where red/:not green)+[media] evaluator
白盒(高/方向/比例/hover/pointer/分辨率)+[contain] cascade 存活白盒;
test_scroll [contain] 像素(内红/外背景)。
验证:10 套件全绿;14 张主题自检全 OK。

# <26>: ::first-line / ::first-letter(首行/首字拆分绘制)

SOLVED

行边界只在 paint 时存在(layout 只有整串 run, 换行在 draw_text_at 的
layout_text 里), 所以拆分放 paint_text 尾部:
style: match_pseudo 认 pv=4/5 + layout 伪规则桶(firstline/firstletter);
块节点 build 时探测置位(fl/flt 位, paint 读位不读表, 零热路径成本)。
paint: 仅块的首个 plain-text run(::before run 不染) + 无 selection/shadow/
stroke 时拆分(那些画整 run, 拆分会错位, 文档写明): first-line 按 wrap 边界
(tl 表复用 first_line_bytes, 首行 line 样式 + 余下原样式, 续行 align 归零);
first-letter 按首 display char 切(UTF-8 首字节长, 前导空格留行), letter 样式
独立 draw(bh=1 禁二次居中, 顶对齐行顶, 大字下溢) + 余下 x 偏移 advance
(wrap 宽同步收缩)。
踩坑三处: (1) 采样行 y=18 差 2px(字形顶 y≈20) —— 全区扫描定位;
(2) 首字 draw 传 bh=flt_gh 致 draw 内二次居中下移 —— 改 bh=1;
(3) 测试 park 污染 l1red(short 红字 park 到 y≈10 被首行扫描误收) —— 删 park,
全区扫描 + 坐标打印。
约束(文档写明): inline 子元素跨首行不管; 首字大字号溢出行盒(行高不重算);
有 selection/shadow/stroke 跳过拆分; 结合字符按首 display char 切。
回归:test_scroll [first](首行红@12,20/次行黑无红/首字红+余下黑/单行红)。
验证:10 套件全绿;14 张主题自检全 OK。

# <27>: :has() 父条件选择器(后代/子/兄弟)

SOLVED

匹配器右→左, :has 需向下/向旁看 —— 求值器独立(has_match_inner):
前导 combinator 定候选集(> 子 / + 紧邻弟 / ~ 后弟 / 无然后代 DFS 迭代),
余下是完整选择器(内部 combinator 可用), 复用 whaleui_style_match(零状态
近似, 同 :is 剥状态)。多个 :has 串联全中; :not(:has(...)) 经 :not 递归
可用。解析存 has_sel 向量; 权重取参数最大(同 :is); 选择器逗号切分与 paren
感知早已修好(:has(a,b) 不断裂)。
之前行为: `div:has(X)` 退化裸 div 误匹配(比不匹配更坏); 裸 `:has(X)`
整规则失效; specificity 漏计内部。
踩坑: 裸 `:has(span)` 期望"永不匹配"写错 —— 规范是"含 span 后代即匹配",
改期望 true/false 对。另 img placeholder 盖采样点(改小 img 采卡片角)。
回归:test_style 白盒(后代/子/兄弟/否定/裸)+test_scroll [has] 像素(有图红/
纯文白)。
验证:10 套件全绿;14 张主题自检全 OK。

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


# <28>: hover 失效的链路级根因(四处) + 动画帧无效操作 + 无界缓存

SOLVED

现象: hover 修过多次(度量上下文/display 归一/mouse_pending/祖先链补齐/
xf 标志/MOUSE_LEAVE)仍会复发。本轮把 事件->命中->级联->重排->重绘 整条
链重走一遍, 找到四处此前每次只修掉一个症状的结构性缺陷, 全部落在通用
机制层:

**(1) hit_test 与 paint 三处不一致 —— 命中的不是画出来的区域。**
  a. position:fixed: paint 在 fx 节点清零祖先滚动偏移, hit_test 没有 ->
     页面滚动 N px 后 fixed 头部被按"绘制位置上方 N px"命中, hover/点击
     落到下层内容("滚动后导航条失灵")。
  b. translate/scale: paint 对变换元素自身盒按 origin 缩放、对整棵子树
     平移 t + origin*(1-scale), hit_test 完全没镜像 -> 上浮/放大卡片在
     画出的位置点不中("hover 只在鼠标持续移动时有效"的另一半根因)。
     rot/skew 逆变换保留原近似, 锚点改用画出的盒。
  c. 兄弟重叠: paint 按文档序后画者盖前画者, hit_test 却"首中即返回" ->
     先画(下层)的兄弟抢走 hover/点击。改为"最后一个命中者胜"(与 paint
     优先级一致); chunk 合并盒剔除照旧, 非重叠时代价不变。

**(2) hover 只由 motion 驱动。** 滚动/重排把另一个元素移到静止指针下时,
:hover 要等下一次 1px 移动才更新。现在 render 记录最后指针位置
(mouse_wx/wy/mouse_has), 任何几何变更(状态重排的布局路径/全量重建/几何
reflow/字体缩放/DOM 结构/滚动)置 hover_recheck, 帧内 paint 前用最后位置
重命中; 命中变化走与 motion 相同的状态路径(state_pending), 本帧把新旧
两盒并进损伤条带。MOUSE_LEAVE 清 mouse_has, 防止指针离开窗口后重命中
复活 :hover。

**(3) 动画帧的无效操作。** 动画/悬停损伤累积、paint-only 动画查找、
layout 动画收集合计 3-4 次全树 std::function 走访/帧 -> 新增
whaleui_anim_els(枚举 tick 的 act 表), 配合 tree->by_el 直取, 全部改为
O(动画元素); whaleui_anim_apply_ov 改为返回"值是否真的变了", 暂停/
延迟期(值不变)帧不加损伤, 条带为空走既有 below-fold 跳过, 绘制整帧省掉。
GPU 侧合成早已按条带 dispatch(layer_rx/rw), 本轮补的是 CPU 侧。

**(4) 帧率策略落地。** max_fps 缺省时读显示器刷新率(动态刷新率), 电池+
省电模式 60 —— goal.md 的既定策略此前只写在注释里, 代码永远 60。

**内存:**
- g_tl_cache(行表)超预算时"跳过记账但仍 emplace" -> g_tl_bytes 不涨而
  map 无限生长; 改为超预算不入缓存, 单 scratch 槽交付(调用方当帧消费)。
- whaleui_render_reset_dom 漏清 el_ver/text_w_cache/text_size_cache/
  g_tl_cache —— 全是元素/文本缓冲指针键, 文档销毁后地址复用可静默命中
  旧数据 -> 全清 + tc_bytes=0; text_size_cache 加 65536 条上限(内容寻址,
  清空安全)。

**回归(修前 FAIL / 修后 PASS 均实测; 方法: git stash src 仅留测试):**
- test_scroll [hover-scroll]: 滚动 120px 后静止指针下的 B 须获得 :hover
  并画蓝 -- 修前 hover==b=0 像素红, 修后 1/蓝。
- test_scroll [hit-translate]: translateY(-40px) 上浮后的卡片在绘制位置
  (80,70)须命中 -- 修前 hit==b=0, 修后 1。
- test_scroll [hit-fixed]: fixed 头部滚动后 (150,20) 须命中头部且 :hover
  保持 -- 修前 under==h=0(命中下层内容), 修后 1 且像素蓝。

验证: 10 套件全绿; 4 张主题离屏自检 OK(fluent/material x 深浅);
probe_hover(demo_full) hover 段无布局漂移、scroll_max 不变; 旧代码下探针
[scroll] 输出与新版相同(scroll_y 打印的是布局字段, 探针口径, 非回归)。

踩坑: xmake f --tests=y 会触发 lexbor 重装报 "vs not found"(包已在,
忽略该报错, 直接 xmake b test_scroll 即可); hit 顺序改"末中者胜"后,
重叠兄弟的 hover 归属与 paint 一致, 既有非重叠用例全部不受影响。


# <29>: GPU 高帧率占用 —— 全屏 compute 区域化(五 pass) + clear 化 + 条带 scissor

SOLVED

现象: 高帧率(动画/悬停)下 GPU 占用高。帧管线 CPU 侧在 <28> 已按损伤收敛,
但 GPU 侧每帧仍有五处**全屏** compute + 一处全屏 RMW 写 + 全屏源 blit ——
与损伤面积无关, 只与 framebuffer 面积成正比:

**(1) 五个区域 pass 全屏 dispatch -> 记录包围盒 dispatch。**
kShadowCS/kBackdropCS/kInsetCS/kFilterCS/kMixBlendCS 每线程本来就要拿记录
盒做包围判定, 但 dispatch 覆盖全屏 —— 4k 一遍 ~13 万个 8x8 工作组,
其中绝大多数线程的判定结果为"不属于任何记录"。现在 CPU 侧先算当帧记录的
包围盒(region_of_records, 按 stride 混合步长通用), 作为 thread-id 偏移经
uniform 推给 shader(与 kTextCompositeCS 同款 cbuffer b0 space2 绑定 ——
该模式在 D3D12/Vulkan 双后端均已验证), dispatch 只覆盖包围盒。单个小元素
模糊的工作组数降约两个数量级; 全部记录出屏时整个 pass 跳过。
shader 内逐记录判定保留(多记录重叠区域仍需选层), 输出与全屏版**逐字节
一致**(fluent/material x 深浅 4 张主题 render_shot 哈希对照)。

**(2) shadow 目标清零: 全屏 RMW compute -> 固定功能 clear pass。** 旧
kShadowCS 对"不属于任何记录"的像素写 acc=0 —— 用 800 万个读改写线程做
memset。区域化后这些像素不再被触碰, 改为 CS 前一个空 render pass
LOADOP_CLEAR 透明黑(成本近乎零, 且语义更显式)。

**(3) backdrop/filter 的模糊源 blit 区域化。** 旧逻辑每帧把整幅
geom_cur/target2 blit 进 blur_tex 再全量 mip。现在只拷记录盒 + mip 足迹
外扩(blur>=8 -> level7 texel 跨 128 blur-tex px = 256 fb px, 双线性再拉
邻居, 外扩 520px; 小模糊 16px), 源坐标偶对齐(blur_tex 是 fb/kBlurDiv,
偶坐标保证 1:1 映射, 与全帧 blit 同约定)。GenerateMipmaps 仍是全纹理
(SDL API 无区域版, blur_tex 为半分辨率, 记为边界)。

**(4) partial 帧主 raster pass 条带 scissor。** hover/动画帧只重绘条带,
但页面级大四边形(全页背景)覆盖全屏 —— 没有.scissor 时光栅器对每个像素跑
SDF 像素着色器。load_only 帧把 scissor 钉到 text_layer 上传的条带上
(g->layer_rx/rw 与 paint_clip 同源), 条带外片段在固定功能阶段丢弃。
各 pass 开始时 SDL 自动重置 scissor 状态, 不外泄。

**(5) 越界守卫。** dispatch 取整到整组后边缘线程可能越过区域/fb 边界
(D3D12 静默丢弃 OOB RW 写, Vulkan 校验层报警): 五个区域 CS 按 hdr 里的
fb 尺寸丢弃边缘线程; kTextCompositeCS 的 uniform zw 分量(原来恒 0)改为
携带区域尺寸, 守卫 id < zw。

验证: 10 套件全绿(blur/backdrop/filter/inset/blend 像素断言全部与改动前
逐值相同); fluent/material x 深浅 4 张主题 render_shot 与改动前**哈希
一致**; xmake b 三库 + demo + tests 通过。

遗留: GenerateMipmapsForGPUTexture 全纹理(SDL 无区域 API); 最终
swapchain blit 全屏(present 本性); FSR 两 pass 全窗(仅 FSR 激活时)。


# <30>: hover 延迟真因(动画两路径不组合) + select 三连修 + DOM 三阶段事件

SOLVED

用户报告: demo 卡片 hover"鼠标移走才反应"; select"点不开 + 文字错位"。
三件事, 逐条都是"之前修过又复发"的链路级问题。

**(1) hover 延迟的真因 —— 两条动画路径不能组合。**
demo 卡片 `.anim-card:hover{transform}` 是 paint-only 过渡, 同一页的
`.anim-bar` 是 width 布局动画(infinite)。只要有布局动画在跑, 帧走
relayout_multi 分支, 而该分支只给重建集里的元素应用插值 —— paint-only
过渡的 apply_ov 路径被整段跳过, hover 的 transform 永远钉在过渡起点,
直到鼠标移开触发重级联才把中间值写进 style("移走才反应")。
修(通用机制, render.cpp): paint-only 应用抽成 apply_paint_only_anim,
两分支共用; layout-anim 分支在 relayout 后对重建集之外的动画元素同样
应用插值 + 刷新 xf + 重算 opacity 链; changed 标记并入
anim_values_changed(值不变帧仍可跳过绘制)。
回归: test_scroll [anim-compose] —— width 无限动画进行中悬停过渡必须
上浮; 修前 lifted=0 FAIL / 修后 lifted=1 PASS(均实测)。

**(2) select 三连修(handle_click 不再全量重建)。**
调试 select-pick 用例时逐层剥开三层:
  a. 开/关/选都置 has_dirty=1 -> 每次开关全量重建整树: 外部持有的布局
     节点指针变孤儿("点第二次打不开"的直接表象), 每次 toggle 付全布局
     代价。popup 画在树外, 开关不改任何样式/几何 —— 改走 scroll_el +
     scroll_dirty(整视口重绘一帧, 无 relayout)。附带: motion 高亮分支
     的 has_dirty 同样改为 scroll 帧。
  b. 点选后值文本不变: 值 raster 缓存键(元素/几何/颜色)都不随选择变 ->
     选 option 时 bump 该元素 el_ver, 旧光栅失效。
  c. pick 后帧根本没画: scroll_el 只在 wheel 路径连带 scroll_dirty, 帧的
     idle 早退只看 scroll_dirty -> select 三处同时置 scroll_dirty=1。
     (idle 早退不认 scroll_el 是链路洞, 本轮在调用点补齐。)
  d. 值文本基线: paint_select_value 用 content.y - 2 贴顶画, input 用
     content 盒居中 -> 高控件里 select 文字偏上("文字错位")。改 vy =
     content.y(与 input 同盒同居中); popup 行本来就是居中, 不动。
     主题对照: 差异像素只在两处 select 的值文本行(y≈47/655, 上移 2px),
     其余逐字节一致, 自检 OK。
回归: test_scroll [select-pick](开/关/重开/点选 BBB/值文本重绘) +
[prevent-press](phased 派发报告 prevented)。

**(3) DOM 三阶段事件(控件可靠性的地基)。**
监听 API 之前只有目标单点派发, 引擎默认行为(select toggle/checkbox/
details/focus)没有任何可取消的契约 —— 自定义控件只能绕着引擎行为打
补丁("控件容易坏"的根子)。现在:
- events.cpp: 捕获(根->父) -> 目标(先捕获监听后冒泡监听) -> 冒泡
  (父->根); Listener 加 capture 位; event 对象加 currentTarget + phase
  (1/2/3); stopPropagation 中断; 任一阶段 preventDefault 由
  whaleui_dom_dispatch_phased 经 out_prevented 带出。
- 公开 API(whaleui.h): add_event_listener_ex(el,type,cb,ud,capture),
  dispatch_phased, event_current_target/event_phase; 旧 add/dispatch 签名
  与语义不变(冒泡注册/目标派发是其子集)。
- app.cpp: mousedown 先派发(返回 prevented)再决定整条按下链
  (set_pressed + handle_click + click 派发); mouseup 同理。mousedown 被
  prevent 则整链取消(浏览器语义)。click 的 toggle 仍在 handle_click 内
  先执行(顺序保持本轮不变, 注释写明 follow-up: 把 toggle 移到 click
  派发之后需要 hit 在树编辑后存活)。
回归: test_dom 三阶段顺序
(bodyC1;divC1;spanC2;spanB2;divB3;bodyB3) + divB 处 stop 截断 bodyB。

验证: 10 套件全绿; 4 张主题自检 OK(差异仅 select 基线行); probe_hover
无布局漂移。踩坑: 测试里跨帧持有 layout 节点指针必成孤儿 —— select-pick
改用固定坐标(引擎不再重建, 坐标稳定); arena 遍历找节点只在同帧内有效。


# <31>: checkbox 不同行 + chunk 失步(白屏/hover 丢) + 位移块剔除

SOLVED

用户报告: input checkbox 与 label 不在同一行; 点击后几秒窗口清空;
select 仍打不开(本地 demo 级全路径验证通过, 见下)。

**(1) checkbox 不同行 —— inline_member 把 relative 当块。**
UA 给 input/select/textarea 置 `position:relative`, 而 layout 的
inline_member() 与 block 行判定要求 `position == static` —— 于是
checkbox 被当块独占一行, label 文本另起一行。relative/sticky 仍在流内,
只有 absolute/fixed 离流: 两处判定统一为 `pk != 1 && pk != 2`(与 flex
收集条件一致)。回归 test_scroll [check-row]: box(0,0 16x16) 与文本 run
同行(16,0), 之前是(0,16)。
主题对照: showcase 渲染与改动前 0 像素差异。

**(2) 点击 checkbox 白屏 —— toggle 的全量重建 + chunk 失步。**
checkbox toggle 置 has_dirty=1 全量重建(同 select 旧病): 改走 DOM 脏
路径(checked 是样式性变更, lexbor 直接调旁路了 mark_dirty, 这里补
whaleui_dom_mark_dirty; UA :checked 只改颜色, 布局键变化自动回退重建)。
回归 [check-row]: 点击后 checked 翻转、同一棵树、行内有墨。

**(3) 真根因: relayout 的 splice 不同步 chunk 链表 —— 白屏/hover 丢的
总源头之一。** relayout_impl / relayout_multi 把新子树接进 first_child
链, 但 parent->chunks 还指着旧 chunk 对象: paint/hit 的 chunk walk 用
陈旧盒剔除, 新成员被整块跳过 —— hover 一次就丢子树, 大面积漏画即
"白屏闪"。修(layout.cpp): 抽 rechunk_parent, 两处 splice 后重建 chunk
链 + bd 冒泡; state 分支的 style-only 重算从"只算 fnode 子树"改为 mark
冒泡 + root 统一重算(否则 parent 的合并 bounds/chunk 盒保持清零)。
hit_test 的 chunk 剔除加位移守卫: 盒 miss 但块内有 xf/fx/sk 成员时逐个
测(位移把绘制点带出布局盒是合法命中, 如上浮卡片)。
回归: test_scroll [chunk-sync](relayout 后 chunk 双向一致 + 命中 + 蓝)
+ [hit-translate] 恢复(上浮卡片绘制位置命中)。

**(4) select 本地结论。** demo 级复现(select-flex + select-demo: flex
header/7 选项/长页面/全 pressed+click 路径): 打开(open=1)、popup 绘制
(ink=170)、点选、值文本重绘全部通过 —— 点击链路本地无断点。用户环境
的"点不开"需他的精确操作序列/窗口配置复现, 测试探针已入库(select-demo
用例即 demo 的 header 结构, 可直接在其上加条件)。

验证: 10 套件全绿; 4 张主题自检 OK(checkbox 改动 0 差异, select 基线行
差异延续 <30>); probe_hover 无布局漂移。


# <32>: select 文字两遍 + 下拉全同字 + popup 的 CSS 与动画

SOLVED

用户报告: text 被渲染两遍(右侧还有一个); 下拉框文字全变成当前选项;
popup 不进树 —— CSS 能否改样式、动画是否支持。

**(1) 右侧第二遍 text —— 箭头命中了值文本的光栅。**
离屏探针实证: 值文本行有两个相同的 AAA 字形块(x≈35 与 x≈163, 后者恰
在箭头绘制位)。paint_select_value 画值文本与箭头 `▼` 共用同一个
ckey(select 元素)+同 fs/style/family/color, 而 raster 缓存键**不含文本
内容**(且 wrap=false 时几何归一为 -1)—— 箭头直接命中值文本 AAA 的光栅,
在箭头位画出第二个 AAA。修(render_text.cpp): 短文本(<256B, 引擎合成串
全在此列)把 FNV 内容哈希进键; 长布局 run 保持版本键(292k 字符 FNV 的
0.5ms/帧不能回来)。探针: 右侧变为真正的 ▼ 三角。
主题对照(n4->n5): 差异像素恰为两处 select 的箭头区(AAA->▼), 余一致。

**(2) 下拉全同字 —— 同一根因。**
popup 每行 draw_text_at 传同一 ckey(n->el)+同几何/色, 键不含文本 ->
行 1..N 全命中行 0 的光栅("全变成当前选项", AAA 恰是默认选项)。
修: popup 行传 ckey=nullptr 直接光栅(行文本每帧从 DOM 现组, 指针不稳;
行数个位, 直接画是最对的成本)。
回归: select-demo 断言三行哈希各异(修前三行相同 FAIL)。

**(3) popup 的 CSS —— 影子树匹配(whaleui_style_virtual_host)。**
之前只有裸 `.select-popup` 全等匹配: 页面 CSS 写 `select.foo
.select-popup` 定制不到。popup 是 select 的影子孩子: 分支匹配当且仅当
最右 compound 是虚拟类, 祖先段对宿主走正常 matcher(后代传递性使
`P .popup` 与 `P > .popup` 同时正确; `+`/`~` 无影子语义拒绝; 裸类恒中)。
paint_select_list 三处改 host 版。主题零行为变化(裸类路径保留)。

**(4) popup 的动画 —— CSS 打开过渡。**
render 加 open_select_t(打开时刻, anim 时钟); paint 按 popup 样式的
`transition-duration`(首值, s/ms, 缺省 120ms)算 ease-out cubic 的
opacity + -6px 上浮; alpha 在一处 fade 进所有 popup 颜色; 帧在打开后
2.5s 内保持 alive(过渡跑完无输入也收敛)。损伤天然覆盖(scroll 整视口
帧)。回归: select-demo 等过渡跑完再断言行文。

踩坑: 中间一次 chunk-sync 失败是 xmake 增量在 stash 来回切时的 stale
obj(重建即过, <22> 同类)。教训: stash 二分后必须 clean 重建再下结论。


# <33>: 点击闪一下 + scrollbar 列全树走访

SOLVED

用户报告: 点点鼠标窗口内容闪一下(脏渲染相关)。

**根因(两处, 同一帧管线):**

(1) `scroll_el` 一身两任。select 开/关/选举复用它做"重绘一帧"信号,
但帧把它无条件当"页面滚了": scroll_frame 分支清掉全部 hover/focus/
pressed_old 标记并整视口重绘 —— 于是任何 :hover/:active 损伤都被吞,
press 显示 :active、release 那帧却什么都不画, 下一次 motion 才补上
("点一下闪一下", 对称性破坏: 下去有漆, 上来无漆)。
修: 真滚动(scroll_dirty 置位, wheel/drag 路径)才走 scroll_frame;
裸 scroll_el(无 scroll_dirty)是纯重绘信号 —— 保留状态标记, 只强制整
视口漆一帧(popup 在树外, 无其他损伤源)。click-flash 用例: 按下蓝、
抬起红, 对称。

(2) mouse-up 的 :active 恢复从无损伤。pressed 在 up 里直接清零, 而
state_pending 只在 focus/pressed 变化时置位 —— 同按钮按下抬起(最常见
的点击)focus 不变, up 那帧 state_pending=0, state 段不跑, 恢复无损伤。
修两处: up 记录 pressed_old_el(=old_pressed); 门条件加
pressed_reverted; state 段对 cascade 未变但身为状态端点的元素钉住
old 标记(hover/focus/pressed 三对, 原来只钉 hover), 于是 partial 分支
必有它的损伤盒。

**性能(同轮, 脏渲染热路径):**
partial 帧的 scrollbar 列拓宽原来是全树 `add_bars` 走访/帧。fix_scroll_max
(写 scroll_max 的唯一写点)顺手建 `scroll_boxes` 注册表; partial 帧经
by_el 直取, 34k 节点页每 partial 帧省一次全树 std::function 走访。

回归: test_scroll [click-flash](plain红/down蓝/up红, 修前 down==plain
FAIL) + 既有 [chunk-sync] 在修偏中间态时抓到一次回退(见下); 10 套件
全绿; 4 张主题与上一轮 0 像素差异; probe_hover 无漂移。

踩坑: 中间把 scroll_frame 判据写成"scrolls[] 非零"—— 陈旧非零滚动位置
让信号帧继续误判; 又试"root 有 range 则滚"—— 同样误判。正确信号是
scroll_dirty(位置本帧真动了), 与 wheel/drag 的写入点配对。chunk-sync
的 pixel 回退(FF0F0F0F)证实判据写错, 改回 scroll_dirty 后恢复蓝。


# <34>: 滚动拖影 —— wheel 逐事件滚动 + scroll 帧误判 + 死代码清理

SOLVED

用户报告: 页面滚动有拖影(渲染不及时或某几帧状态不对)。

**根因(三处, input -> frame -> present 全链):**

(1) wheel 逐事件滚动。MOUSE_WHEEL 到一个处理一个(handle_wheel 直接改
scrolls + 置 scroll 脏): 一次快速拨轮一帧进几个事件, worker 每唤醒只画
一帧, 事件队列排在 live 偏移后面 —— 每帧都漆一个陈旧中间态("拖影/
滞后一节")。motion 早就是合并的, wheel 不是。修(app.cpp/window.h):
事件只累积(wheel_acc/wheel_acc_x/y + wheel_pending), worker 每唤醒应用
一次; 应用后 frame_request=1 保证排空期内帧不断(否则第二波 burst 落在
帧 cap 睡眠里仍漆旧偏移)。

(2) scroll 帧的标记语义(<33> 的延续)。上一轮把"裸 scroll_el(无
scroll_dirty)=纯重绘信号"从 scroll_frame 摘出是对的, 但实现里 scroll
帧清全部 hover/focus/pressed_old 标记 —— hover 卡片那几行就不在损伤集
里(只是整视口漆盖住, 平时看不出; 与动画/hover 帧交错时露出来)。修:
scroll 帧保留标记走 partial 分支(用 live 的 post-scroll bounds), 只跳过
animation 分支(它的 strip 会把漆面缩到动画盒)。

(3) 死代码: scroll_dy 整套移位/条带/memmove 早已 pin 0(注释写明视口模
型), 但 60 行分支还在帧里(含 per-pixel text_layer memmove + 全树 acca
走访/帧)。删; scroll_dy 改 const 0, 相关分支编译期折叠。

验证: 10 套件全绿(scroll-smear 双帧标记位移 + 无残留通过); 4 张主题与
上轮 0 差异; probe_hover 无漂移。

# <35>: 元素特判点统一 + input 三分类 + 字体/字形/级联三处热路径

SOLVED

用户报告(两个, 同一轮): (1) 小窗口 demo(有动画)功耗爆炸, 之前不烧;
要求缓存/少算 + 后台/最小化/遮挡停渲; (2) 不动鼠标纯滚动直接拖影且
永不消失; 然后继续优化。

## A. 功耗: presented 快路径 + 不可见停渲

- presented 快路径(render.cpp): 值不变帧跳过 paint + GPU + present。
  门用 tick 自己的结论(`anim_values_changed==0`: 插值与现样式全同,
  即 held delay/ finished transition) + 外层排除 scroll/layout/DOM/
  edit/popup/hover_recheck/state_pending/partial。无限动画(脉冲/Bar)
  每帧都变值, 永不进此路 —— 只停 HELD 动画与静态唤醒帧。
  证据链: 初版误伤 hover-lift(快照位置放错 + 条件缺 state_pending/
  partial), 用 stash 二分定位到 render.cpp, 逐个补门修绿。
- 不可见停渲(app.cpp/window.h): MINIMIZED/HIDDEN/OCCLUDED + 事件缓存
  `win->occluded`(部分后端 flag 读数滞后) -> 跳 paint+present, 只跑
  `whaleui_render_tick_only`(anim tick, 无 relayout/paint)。
  EXPOSED/RESTORED/SHOWN 清 occluded + 掉 presented 缓存 + invalidate。
  背景节流(失焦多窗口 10fps)做到一半后回退: 单 demo 窗口走不到该分支
  (size==1), 留着是死代码, 删。
- 刻意没做的: demo 的无限 pulse/bar 照常每帧全跑(行为正确, 停它会冻
  画); FSR/swapchain/vsync 不动。

## B. 纯滚动永久拖影: scroll 判定快照

- 根因: 帧头 `scroll_dirty` 清零(供 hover_recheck 用), paint 侧再测
  `r->scroll_el && r->scroll_dirty` 恒为假 -> scroll_frame 永假 ->
  全视口重绘永不执行, 内容冻在旧偏移(拖影永不消失)。
  修: 帧头快照 `want_scroll_frame`, paint 侧用快照判定; fast path 门
  同步改测快照(旧 `!r->scroll_dirty` 同样恒真, 顺手改)。
- 回归: scroll-each(五连单 notch 滚, 每帧断言 marker 到位 + 上方
  25px 外无鬼影; 断言写崩两次: 采样行落在 marker 自身 20px 带内,
  改 25px 外探针后绿)。scroll-smear 原用例同步绿。
  注: f2/f3 行曾出现 Failed to close command list(0x80070057, D3D12
  后端瞬态), 重跑自愈, 与本轮改动无关(同帧前后用例皆绿)。

验证: 全量构建 ok; 11/11 套件 exit=0。

SOLVED

用户要求: 代码优化 + 性能 + 找 bug(尤其元素特判: 名义上不特判, 但特定
元素仍有事件响应/绘制/几何差异, 要仔细检查)。

## A. 全仓特判扫描(结论先行)

扫 `src/layout + src/render + src/style + src/dom + src/core` 全部
`tag_eq/local_name/tag_id` 命中(含 render 七个 paint/control/text/color
文件)。结论: 绝大多数已是通用机制(UA 默认 CSS 声明行为 + C++/脚本配置,
如 `:checked`、`cursor:pointer`、`details/summary` 折叠), 剩余的是**行为
分类**(元素天然有不同的事件/绘制/几何语义, 不是"某个标签走后门"):
select 值渲染/option 列表、`input[type]` 三态、textarea 多行、img 内容盒、
table 行组、details/summary。这是本轮统一的对象 —— 不是删分类, 而是让
每个分类问题只有一个写点(single writer)。

修前的问题恰好证明"散落的特判"才是 bug 之源, 不是分类本身:

- (C1) select 当前值有**两个**写点: 点击写 `select_index`, `set_value()`
  写 DOM `selected` 属性, `paint_select_value` 只读前者 -> API 设值与
  点击永远对不上; option 计数按 DOM 子节点数(把空白/注释也算进去)与
  `select_options`(只收 option)不一致 -> 错位。
- (C2) 单行 input 的判定散落在 12 处, 每处手写一次 `tag_eq(el,"input")`:
  `type=submit/button` 被当成文本框走 caret/换行/横滚/ENTER 换行分支。
- (C3) hover 光标两处手写同样的 a/select/button/summary 名单; UA CSS 只给
  了后三者 `cursor:pointer`, `a` 没有 -> 无样式页面下行为与级联不联动。

## B. 改动

**B1 input 三分类单写点(layout.h/layout.cpp)。** `whaleui_input_kind(el)`
extern "C": 0=text-like(可编辑文本, 缺省/未知也归 0, 与 is_editable 的
"只排除已知非文本"一致), 1=button/file/color/submit/reset/hidden/image/
range(无文本无原生开关), 2=checkbox/radio(原生控件外观)。修掉了 C2 的根:
submit 此前返回 0(被当文本框)。

**B2 render 侧单写点(render.cpp/render_internal.h/render_control.cpp)。**
`is_check_radio` 改调 `whaleui_input_kind==2`(删手写 type 比对);
新增 `is_single_line_input(_el)`(input 且 kind==0; textarea/
contenteditable 是多行, 返回 false), caret/换行/横滚/edit_ensure/
ENTER/wheel/paint_editable 的 12 处 `tag_eq(input)` 全改调它;
新增 `is_password_box_el`/`is_number_box_el`, 掩码/插入过滤/dot 校验
共 7 处手写 type 比对全改调它们。`is_editable(_node)` 的两份手写排除表
(含 "select" 这种根本不该出现在 input 表里的词)删, 改调 kind。

**B3 光标统一 + UA 联动(render.cpp/theme_css.cpp)。** `natively_clickable`
+ `cursor_for_hover` 两个单写点(可编辑->I-beam, `cursor` 样式优先,
原生可点击标签兜底); set_hover 正常段与 popup 段共用。UA 的
`a{cursor:pointer}` 补上, 与 select/button/summary 的既有规则对齐。

**B4 select 值 DOM 同步(render.cpp/render_control.cpp)。**
`select_current_index(r, sel, texts)`: 点击期 `select_index` 优先(含尚无
selected 属性的新值), 否则按 option 顺序读 DOM `selected`(跳过非 option
子节点, 修计数错位), 都没有则 0。`paint_select_value`/`paint_select_list`
共用。点击 pick 时把结果写回 DOM(option 的 selected 增删) + el_ver
bump + scroll 脏重绘(不重建), DOM 成为值的唯一真相, C1 关闭。

**B5 绘制分发单点(render_paint.cpp/render_control.cpp/layout.cpp)。**
`paint_node` 首部 `CtrlKind` 一次分类(checkRadio/progress/editable/
select), 下面各臂只画不再复测标签; `img`/`select` 走 `tag_id`;
`is_select_node` 改 tag_id(O(1) 命名谓词, 点击/popup 共用)。
layout 侧: `tag_id_of` 补 THEAD/TBODY/TFOOT; `is_row_group` 改 tag_id
(仍保留 display 通用回退: div 配 display:table-row-group 同样工作);
value 型控件判定(is_sel/is_inp)改 tag_id + kind; textarea 默认高度去掉
误写的 `input_kind==0` 条件; `append_rows` 三处手写 li 比对收敛为
`is_li_el` lambda(DOM 节点本就没有 tag_id, 这是该路径唯一剩下的裸名
检查, 已注释说明)。

## C. 性能: 三处热路径

- (P1) `style_cache` std::map -> unordered_map + `operator==` + FNV hash +
  `style_cache_by_el` 单元素索引: 帧内两处"清某元素缓存"从 O(cache) 全表
  扫描改 O(条目)擦除。
- (P2) `render_get_font` 每 miss 一次 `split_families`(vector<string>
  分配, 文本页每帧数千次) -> `fam_cache` 按原始 family 串缓存。
- (P3) `fallback_tried` vector 线性扫描 -> 按 family 分桶的 map;
  `glyph_w_cache` map<tuple> -> unordered_map<uint64 打包键>
  (font 低 32 ^ size<<32 ^ cp*FNV); `ensure_font_state` 的同字体刷尺寸
  清键改为低 32 比对一次遍历。
- 另: 全仓 mojibake 注释块改英文(render.cpp/render_control.cpp/
  render_paint.cpp/render_text.cpp, 约 20 处, 注释唯一改动, 零语义);
  其中 render_text.cpp 一处补了缺失的 `*/`(此前 TextLayoutLine 块注释
  未闭合, 顺手关上)。

## D. 回归与备注

构建 ok; **11/11 套件 exit=0**(含新增的 hover/scroll/select/chunk
用例)。`test_style` 中间红过一次: `cs["font-size"]=="20px"` 在 343 行
断言失败 —— 二分证明干净树也红(与本轮无关), 根因是级联的
`RuleTagIndex` 按 rules 数组地址+数量做缓存键, `rules_destroy` 后下一
次 parse 拿到同一地址+同数量就复用旧索引, 上一张表的 class 桶直接把
新表的 `.f` 规则漏掉。修: `whaleui_style_index_drop` +
`whaleui_css_rules_destroy` 首部显式失效。教训: 地址复用做缓存键必须
配显式失效, 不能只靠"指针变了=内容变了"。

遗留(未做, 明确延期): P2 的 UA 默认样式下沉(img/option/table/meta/
控件尺寸)、C3 的 is_editable 合并后续、C4 number 校验强度统一、C5
radio 组漫游改索引 —— 本轮只收敛判定写点, 不动行为。

# <36>: 输入类残留收敛 + UA 默认尺寸下沉 + 热路径去重解析

SOLVED

<35> 的延期项落地 + 热路径一轮。

## A. C3/C4/C5 输入类残留收敛(render.cpp/render_internal.h/layout.*)

- kind 码 0/1/2 -> 0/1/2/3/4: password=3、number=4 独立成码(此前
  `is_password/number_box_el` 各自手读 type 属性, 与 kind 表各说各话)。
  `is_editable(_node)` 改为 kind 0/3/4 可编辑, `is_single_line_input(_el)`
  同步 0/3/4(password/number 是单行, 此前 kind==0 把它们排除在几何分支
  之外是错的 —— 与 is_editable 矛盾)。
- `is_contenteditable` 提升为具名单写点(render.cpp 定义 +
  render_internal.h 声明): input/textarea 永假(属性不能把 input 变成
  多行), 其余看 contenteditable 非 false。`is_editable(_node)` 的属性
  分支改调它, 删第二份手写判定(此前 render.cpp 有两份同名函数, 后一份
  覆盖前一份, 实际靠运气)。
- C4 number 校验合一: `input_insert_filter(el, ins, rep_a, rep_b)`
  吃替换区间, char 过滤 + 全值单点检查一步完成; paste(CTRL+V)与
  handle_text 两个调用点传区间, handle_text 里手写的第二份 dot 检查删
  (此前两份检查并存, 调用点还没传区间, paste 路径实际只走了 char 过滤)。
- C5 radio 组: `radio_uncheck_peers(picked)` 单写点(owner document 为根,
  iterative 栈漫游 —— 旧递归 lambda 深页爆栈; 按 name 精确比对, checkbox
  误伤排除, 每个被清的 peer 补 `whaleui_dom_mark_dirty`)。点击路径删 30 行
  手写递归, 改调它。附带修两处: `label for="id"` 经布局树 `find_el_by_id`
  (新建, render 上下文无 document 句柄, 走布局树迭代查找)真正接通(此前
  注释写"尚未接线"); wrapped label 改深搜(此前只看直接子节点); radio
  判定收敛为 `is_radio_type` 单写点。

## B. P2 UA 默认尺寸下沉(theme_css.cpp + layout.cpp 回退)

- UA 新增(主源): checkbox/radio 16px 无 chrome(并入既有 reset 规则);
  text 类 input 12em + 28px(select 同高, `input[type=text]/:not([type])`
  等文本族精确列出 —— 裸 `input{}` 会误伤 checkbox/radio/submit 等,
  属选择器 + `:not([type])` 均已验证引擎支持); textarea 12em x 60px;
  progress/meter 共用 16em x 6px(关闭了 C++ 16em/6px 与旧 UA 100%/4px
  的分歧, 以此为准); img 300x150。
- C++ 保留 UA-less 回退(裸 layout_compute rules=nullptr, 单测/嵌入方):
  同值镜像, 仅当属性缺席时填。教训三连(全是回退写法误伤正常流, 逐个修):
  (1) 高度回退未限定 select/input, textarea 被盖 28px(应 60px);
  (2) 高度回退写在"缺宽度"大块里, 显式宽度丢默认高度;
  (3) 宽度回退缺控件门, div/body 坍缩 12em(50% 按 192 算)—— 门只认
  select/input/textarea/progress/meter。
- checkbox 绘制跟随布局盒(12~24px clamp + 居中), 命中同步同盒(此前两处
  硬编码 16x16, 与 UA 规则各说各话); hit_test 的 transform 解析由每节点
  两次改为一次(translate/scale 与 rot/skew 逆变换共用)。
- select 值字体跟随元素 font-size(UA --text-base 继承, 无样式 16px),
  popup 行同规则(13px 保留为无样式缺省, 保住 26px 行节奏)。

## C. 热路径去重解析(layout.cpp/render.cpp, 零行为)

- `inset` 5 解析 -> 1: 删 4 次 `len_px(ins)` 预解析, 只留 `sides()` 一次
  TRBL 展开(旧代码先 4 次 len_px 再 sides, 后者覆盖前者, 前 4 次纯浪费)。
- min/max-width 与 min/max-height: compare + assign 各解析一次 -> 解析
  一次存局部(4 处)。
- hit_test 每节点 `whaleui_transform_eval` 2 次 -> 1 次(translate/scale
  分支与 rot/skew 逆变换分支共用 `htf`)。
- 文本 atlas/顶点缓冲经代码审计确认为已最优(常驻 vector + clear 保容量
  + 脏矩形上传): 不动。

验证: 全量构建 ok; 11/11 套件 exit=0(layout/style/scroll/render 四套
回归中修绿: textarea/input 高度门、div 宽度门、select 值字体; scroll
的 check-row/select 像素行仅数值漂移, 断言结构未动)。

# <37>: 小窗口功耗爆炸 + 纯滚动永久拖影

SOLVED

用户报告(两个, 同一轮): (1) 小窗口 demo(有动画)功耗爆炸, 之前不烧;
要求缓存/少算 + 后台/最小化/遮挡停渲; (2) 不动鼠标纯滚动直接拖影且
永不消失; 然后继续优化。

## A. 功耗: presented 快路径 + 不可见停渲

- presented 快路径(render.cpp): 值不变帧跳过 paint + GPU + present。
  门用 tick 自己的结论(`anim_values_changed==0`: 插值与现样式全同,
  即 held delay/finished transition) + 外层排除 scroll/layout/DOM/
  edit/popup/hover_recheck/state_pending/partial。无限动画(脉冲/Bar)
  每帧都变值, 永不进此路 —— 只停 HELD 动画与静态唤醒帧。
  证据链: 初版误伤 hover-lift(快照位置放错 + 条件缺 state_pending/
  partial), 用 stash 二分定位到 render.cpp, 逐个补门修绿。
- 不可见停渲(app.cpp/window.h): MINIMIZED/HIDDEN/OCCLUDED + 事件缓存
  `win->occluded`(部分后端 flag 读数滞后) -> 跳 paint+present, 只跑
  `whaleui_render_tick_only`(anim tick, 无 relayout/paint)。
  EXPOSED/RESTORED/SHOWN 清 occluded + 掉 presented 缓存 + invalidate。
  背景节流(失焦多窗口 10fps)做到一半后回退: 单 demo 窗口走不到该分支
  (size==1), 留着是死代码, 删。
- 刻意没做的: demo 的无限 pulse/bar 照常每帧全跑(行为正确, 停它会冻
  画); FSR/swapchain/vsync 不动。

## B. 纯滚动永久拖影: scroll 判定快照

- 根因: 帧头 `scroll_dirty` 清零(供 hover_recheck 用), paint 侧再测
  `r->scroll_el && r->scroll_dirty` 恒为假 -> scroll_frame 永假 ->
  全视口重绘永不执行, 内容冻在旧偏移(拖影永不消失)。
  修: 帧头快照 `want_scroll_frame`, paint 侧用快照判定; fast path 门
  同步改测快照(旧 `!r->scroll_dirty` 同样恒真, 顺手改)。
- 回归: scroll-each(五连单 notch 滚, 每帧断言 marker 到位 + 上方
  25px 外无鬼影; 断言写崩两次: 采样行落在 marker 自身 20px 带内,
  改 25px 外探针后绿)。scroll-smear 原用例同步绿。
  注: f2/f3 行曾出现 Failed to close command list(0x80070057, D3D12
  后端瞬态), 重跑自愈, 与本轮改动无关(同帧前后用例皆绿)。

验证: 全量构建 ok; 11/11 套件 exit=0。

# <40>: 布局动画帧 20ms→5ms + 失焦 30fps + smooth 门控

SOLVED

bench 实测链(工具 temp/bench_anim.cpp, gitignored, xmake 条件 target 已合入): static 0.006ms(空闲零成本) / tick 微基准 0.01ms(动画 tick 无辜) / 小树 full(pulse+bar) ~1ms / 大树(2000 行静态+bar)62ms。临时 SEG 分段计时(测完即删, 树已干净): relayout_multi 19-22ms 主导, fix_runs 2.1, present 7.6, paint 0.1, tick 0.07, bounds 0.09。

## A. relayout_multi 收缩(大树 56→3.5ms, 小树无退化)

- 尾部全树 deep walk 删除(layout.cpp): root scroll_max 重算。帧内已跳 fix_scroll_max(既有 ponytail 策略: 动画不改滚动范围, 下次全量 relayout 纠正), 该 walk 是纯冗余。
- bounds 改 marked-path(render.cpp): mark_all+全树 compute 改为 compute 直走(early-return 跳干净子树)。rechunk_parent 已标脏路径。
- block 链原地 fast path(layout.cpp): 全 block 祖先 + 常规流 + 高度不变 -> 只 layout 动画子树(旧 flow 偏移播种 cursor, font_px=16 与祖先 pass 逐层透传一致), 跳过祖先 box pass。高度变化/flex 回退旧路(双 layout 幂等, 仅高度变化帧多一次子树 layout)。
- present 7.6ms 归因: 不是 GPU 慢, 是 layout 动画帧误判走全帧(脏区=全视口 -> text composite 全量 dispatch + 全量上传)。bigpulse 同树 present 仅 0.68ms 作证。未动 GPU(present 时钟无关契约保持)。

## B. 失焦 30fps + smooth 门控(上一轮 pending, 本轮合入)

- 多窗口失焦兄弟 100ms->33ms(10Hz 可见抽帧, 30 保顺滑省一半)。单窗口零行为变更。半透明盖住走此档(不 park), 全遮挡 park。
- smooth easing 循环加 scroll_fn==smooth 门: 默认路径 scroll_tgt 永空, 零行为变化, 省每帧 map 检查。

验证: 全量构建 ok; 10/10 套件 exit=0(api/dom/font/fs/layout/style/window/render/scroll/anim); bench big p50 3.5ms(峰值环境抖动除外), full 800x600/1440p/4K 均 ~1ms(无像素缩放问题)。

# <41>: JS API C-port 续: 事件全接入 + C++ 层 + 选择器快路径语义漂移

SOLVED

## A. 事件系统接入完成(more-dom-api §9 落地)

- 非冒泡契约: mouseenter/mouseleave/focus/blur 走 `whaleui_dom_dispatch_nonbubble`(捕获+目标, 无冒泡), 与浏览器的 Event.bubbles=false 一致; 其余走三阶段 `_phased`。`events.h` 提供 `WUI_DOM_FIRE[_NB]` 内部分发宏, app.cpp/render.cpp 各接入点统一用宏, 不手写逐分支逻辑。
- 接入完成: mousemove(每帧命中后, 目标元素)、enter/leave(hover 变化 + 出窗 clear_hover)、wheel(命中元素, preventDefault 真取消滚动)、keydown/keyup(keydown 先派发给编辑键, preventDefault 取消编辑)、focus/blur(set_pressed_ex 焦点移动处, 滚动条拖拽也算)、input(edit_replace 值变化时)+change(失焦/单行 Enter/换选项/checkbox 翻转提交, render.h 的 edit_changed_el 即浏览器 dirty flag)、scroll(do_scroll 实际位移后在容器派发)、load(window_load_html_len 成功后于 documentElement)、resize(RESIZED 分支于 documentElement)、dblclick(第二次 click 后)、contextmenu(右键)。仅 submit 无表单提交逻辑, 不派发。
- 按钮映射修正: 公开文档承诺 0=左 1=中 2=右, 旧代码直传 SDL 枚举(左=1 右=3)漂移了; app.cpp 三处派发改 `button-1`。

## B. C++ API 层(include/whaleui.hpp)

- 澄清的架构定位: **C++ API 才是门面, C ABI 是其导出层**。`whaleui::App/Window/Document/Element/Event/ComputedStyle` 全部 inline 薄封装 C 函数, 无虚表/无异常/句柄按值拷贝(lexbor 节点指针)。所有权: Window 由 App 的 destroy 回收(`Destroy()` 仅提前拆除), ComputedStyle RAII 释放快照(需移动语义, C++14 手写 move)。
- test_cpp 覆盖: 查询/遍历/变更/属性/classList/style 声明式/getComputedStyle/事件/canvas 互操/窗口回调。
- **教训(TIPS)**: 事件接线顺带抓到 `simple_match` 复合类解析 bug —— `.a.b` 的 class 段只停在 `#`, "div.x.y" 把 "x.y" 当单个 class 永不匹配(test_cpp 才发现; test_dom 此前无复合类用例, 已补 `.x.y`/`div.x.y` 回归)。手写选择器子集与 `whaleui_style_match` 的语法必须同步, 改一边先查另一边。

验证: 全量构建 ok(full/lite/minimal); 12/12 套件 exit=0(新增 test_cpp)。

# <42>: document 级事件目标 + activeElement/focus/blur 真实化 + submit

SOLVED

## A. document.addEventListener 语义(事件目标解析)

- 公开 API 的句柄不透明, 但 document 句柄 == lxb_html_document*(dom.h 约定), 所以 `whaleui_dom_add_event_listener((whaleui_dom_element_t*)doc, ...)` 可识别: events.cpp 新增 `resolve_target()` —— 仅当节点自身 type == DOCUMENT 时解析为 documentElement(浏览器的 document 事件目标), 其余走元素路径。add/remove/dispatch 三处统一走它, 行为对齐 document.addEventListener 且不破坏元素监听。
- 意义: 应用挂 document 级 click/load/keydown 不再需要"自己找 body/root"; C++ 侧 `Document.AddEventListener/RemoveEventListener/DispatchEvent` 直接转发同一入口。

## B. activeElement + focus()/blur() 从 no-op 变真实

- DOM 层新增 per-document 活动元素 map(`dom.cpp` 的 `g_active()`, 文档销毁时按 key 清理, 防地址复用脏读);`whaleui_dom_set_active/clear_active`(dom.h)是唯一写点。
- 写点两处: (1) public `whaleui_dom_focus/blur` —— focus 先 blur 旧的、再 focus 新的、无变化时 no-op;blur 仅在当前激活元素上触发事件; (2) render 新增 `render_focus_move()`(点击 / 滚动条拖拽 / Tab 经 begin_editing 三处共用), 把引擎 focus_el 与 DOM 镜像 + 非冒泡 focus/blur 一次性同步 —— 消除了此前三处重复的"手写 blur/focus 派发"。
- 边界: `:focus` 样式仍跟引擎 focus_el;未渲染窗口上 API focus 只动镜像与事件, 不动样式(whaleui.h 注释已写明)。

## C. submit

- 单行输入在 `<form>` 祖先内按 Enter: `render_handle_key` 的 ENTER 分支先 commit_change, 再向上找最近的 `<form>` 派发 submit(冒泡); 多行可编辑仍插换行。引擎不做表单导航, preventDefault 是应用挂点。

验证: 全量三变体构建 ok; 12/12 套件 exit=0(test_dom 新增 document 目标/焦点序列断言)。

# <43>: Canvas 文本(fillText/strokeText/measureText) + 文本状态(font/align/baseline)

SOLVED

## 设计

- 文本光栅放在独立模块 `src/canvas/canvas_text.cpp` 而非 render_text: canvas 可以由应用代码单独填充, 没有页面文字参与, 但必须复用同一字体注册表(`whaleui_font_registry_get`)+ SDL3_ttf, 才与页面文字一致。测量 `TTF_GetStringSize`, 光栅 `TTF_RenderText_Blended` -> RGBA32 -> 直 alpha 0xAARRGGBB; 缓存 key 为 (family, size, style) 的 TTF_Font(与 render_text 的 per-(family) 缓存同理)。
- `font` 简写解析在本模块: 可选 bold/italic 前缀 + 数字尺寸(px, 无级联无法解析 em/%/pt 单位 -> 回退默认) + 剩余串为 family 列表; generic 关键字(sans-serif 等)/未命中回退到首个可加载注册字体。
- 定位: 原点(x,y)过当前变换, 再叠 textAlign(start/end/left/right/center)与 textBaseline(alphabetic/top|middle/bottom/hanging|ideographic); alphabetic 用 `-ascent` 偏移(表面顶在基线上方 ascent 处)。strokeText 近似为字形轮廓(edge-only, 4 邻域 alpha 判边)——非真路径描边, 记为已知近似。

## 变体边界(TIPS)

- **canvas 文本只有 Full 变体可用**: lite/minimal 没有 SDL3_ttf, `whaleui_canvas_text_measure/raster` 直接返回 -2; test_canvas 按 `WHALEUI_BUILD_FULL` 条件断言(Full: 测量/宽度单调/ink>0; lite/minimal: ==-2)。canvas 的其他 API(fill/path/ImageData)三变体一致。
- **lite/minimal 构建的强制重编坑**: 改公共头 canvas.h 后 `xmake b -y` 可能对 lite/minimal 命中缓存假通过——touch 受影响 .cpp(`copy /b file +,,`)确认; 本轮验证 4 个新 API 全绿靠的是显式 touch 后的全量重建。

验证: 全量三变体构建 ok; 12/12 套件 exit=0(test_canvas 新增文本闭环断言)。

# <44>: Canvas drawImage + 文档销毁未清理 canvas 上下文(悬垂修复)

SOLVED

- `whaleui_canvas_draw_image[_data]`: 9 参浏览器签名, 源支持另一 canvas 上下文与 ImageData。目标矩形过当前变换(device bbox -> 逐像素仿射逆变换回 user 空间 -> 命中 dest 矩形则按 u/v 比例取源像素, 最近邻), `blend_px` 叠 globalAlpha; sw/sh<=0 取源剩余、dw/dh<=0 取源尺寸(与头文件契约一致); 奇异矩阵返回 0 不绘制; 自拷贝返回非 0(浏览器抛异常的语义映射)。`put_image_data`(设备像素直贴, 不过变换)保持原样——两者差异已写进头注释与 08 手册。
- **修复(实现顺带发现)**: `whaleui_canvas_drop_all()` 自 614740a 起就没人调用——document 销毁后 canvas 上下文 map 留着悬垂的 `lxb_dom_element*`(下次 malloc 复用地址会给新元素端出旧 backing)。现在 `whaleui_dom_document_destroy` 里与 listeners/active 镜像一起清(dom.cpp include canvas/canvas.h)。教训: 注册表型资源(drop/drop_all 对)在创建时就该接上销毁点, 别只留 API。
- test_canvas 新增: 源 canvas 画红 -> drawImage 到 10x10 目标(恒等变换位置/边界/透明区)、dw/dh 2x 放大、ImageData 源 + globalAlpha=0.5 半合成、自拷贝/null 拒绝。坑记一笔: 前面文本用例把 global_alpha 留在 0.5, 新块要先复位——断言失败先查测试状态残留再查实现。

验证: 全量三变体构建 ok; 12/12 套件 exit=0。

# <45>: classList 扩展 + dataset + append/prepend/before/after + insertAdjacentHTML

SOLVED

## A. 语义要点(浏览器对齐)

- classList: 新增 length/item(index)/replace(old,new)(返回旧 token 是否存在并全量替换)/toggle(cls, force)(force>=0 强制, <0 翻转, 返回最终存在性)。token 以空格/制表分隔, 与既有 class_has/class_set 共用。
- dataset: `whaleui_dom_dataset_{set,get,has,remove}`; camelCase <-> data-kebab 映射(`fooBar` -> `data-foo-bar`, 只在 index>0 且大写字母前插入连字符; 数字/已含连字符不二次拆)。set/remove 走 set_attribute/remove_attribute 并 mark_dirty。
- append/prepend/before/after: 单节点形式, 句柄既可是元素也可是文本节点(`reinterpret_cast<lxb_dom_node*>` —— element/text 首成员均为 node), 移动前先 remove 并 mark 旧父结构脏。before/after 要求 el 已挂载(有 parent)。
- insertAdjacentHTML: 复用 parse_into 的 fragment 解析, 抽 `parse_detach` 返回游离节点向量; 四位置各用一个固定锚点保证片段顺序: beforebegin 在 e 前、afterbegin 在 e 原 firstChild 前、beforeend 追加 e 尾、afterend 在 e 原 next 前(用 lxb_dom_node_insert(parent,node,ref,false) 逐节点插到同一 ref 前即保持顺序, 空锚点退化为 insert_child)。

## B. 坑记一笔(TIPS)

- **匿名 namespace 闭合**: dom.cpp 的匿名 namespace 从文件头延伸到 style 辅助函数之后, 新加的 parse_detach 必须落在闭合 `} // namespace` 之前(否则 extern "C" 定义进匿名 ns 链接性冲突 + 列表结构体在 ns 内导致 header 前向声明的 whaleui_dom_list 视为 incomplete)。移动/重排函数块时先确认 namespace 边界。
- 选择器 `div > p` 在 element-query 语义下应命中(后代), 但把根自身也当匹配会污染 `element.querySelector(div)` —— 已在前一轮修成 descendants-only, 这轮 insertAdjacent/append 测试同样建在独立子文档, 不依赖主文档结构。

验证: 全量三变体构建 ok; 12/12 套件 exit=0(test_dom/test_cpp 新增 classList/dataset/insertAdjacentHTML/append-prepend 断言)。

# <46>: DocumentFragment + insertAdjacentText

SOLVED

- `whaleui_dom_create_document_fragment` -> lexbor `lxb_dom_document_create_document_fragment`(注意 3.0 里的真名带 `_document_` 段, 猜的 `create_fragment` 不存在), 返回 `&frag->node` 作 element 句柄(所有 interface 首成员都是 node, 复用 node_handle 直转)。
- 浏览器语义: appendChild/insertBefore/append/prepend/before/after 遇到 DOCUMENT_FRAGMENT 类型节点时**移入其全部子节点**、fragment 自身变空且不被插入。抽了 `splice_insert(parent, node, ref)` 收敛单节点形式; append_child/insert_before 各自内联同样的分片搬运(它们定义在 splice_insert 之前, 不便前向)。子节点搬运用「先全 remove 收集, 再按序插到同一 ref 前」保序——ref 是 parent 的兄弟不是 fragment 的孩子, 搬运期间保持有效。
- `whaleui_dom_insert_adjacent_text`: 复用 `resolve_adjacent`(四位置 -> parent/anchor/append_mode, 之前只给 insertAdjacentHTML 用, 抽成共享), 造文本节点插入。
- 坑: 句柄统一按 node 首成员 reinterpret 的前提是所有 lexbor dom interface 都把 `lxb_dom_node_t node` 放第一位(document_fragment 已核实)。element_destroy 对游离 fragment 走通用 `lxb_dom_node_destroy`, lexbor 按类型分派正确释放。
- C++ 侧 `Document::CreateDocumentFragment` / `Element::InsertAdjacentText` 镜像; 测试: fragment 装 b/i 再 append 进 div -> 子节点归位、fragment 空、insertAdjacentText 前后文本拼接断言。

Selection/Range 仍不做(引擎文本选择是 render 内部 sel_* 状态, 需专门设计再开一轮)。

验证: 全量三变体构建 ok; 12/12 套件 exit=0。
