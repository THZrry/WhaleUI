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

# <47>: Selection API 子集(window.getSelection)

SOLVED

- 选区状态本就存在 render 上下文(`sel_anchor_el/sel_focus_el` + 字节偏移, 拖选/双击三击/插入符写入), 缺的是对外读取。新增:
  - render 侧只读访问器(`whaleui_render_sel_anchor_element/focus_element/anchor_offset/focus_offset`)与 `whaleui_render_selected_text`。
  - `selected_text` 跨元素拼接严格对齐高亮覆盖规则: 同元素取 [lo,hi) 切片; 跨元素 = 锚点元素尾部 + 中间完整元素(布局树前序, 与绘制同序) + 焦点元素头部。布局序走完没找到两端点时**回退 DOM 前序**(端点可能已脱离渲染树), 两遍都失败返回空。
  - 变更器: `clear_selection`(removeAllRanges) 与 `select_all_of(el)`(selectAllChildren, 0..text 全长); 二者都置 `scroll_dirty` 触发一次重绘(高亮消失/出现), 否则帧的 idle 判定会跳过绘制。
- 公开层: `whaleui_window_*` 六个函数(window 持有 render), `selected_text` 用 per-window `sel_text_cache` 保证"指针下次调用前有效"的 C 契约(不能返回 render 的临时 string 的 c_str)。
- 头文件顺序坑: selection 段插在 window 段末尾, 但 `whaleui_dom_element_t` 要到 DOM 段才 typedef —— 段前补 `struct whaleui_dom_element; typedef ... whaleui_dom_element_t;`(重复 typedef 合法)。
- C++ 侧 `Window::SelectedText/SelectionAnchorElement/ClearSelection/SelectAllOf` 镜像。
- 测试: test_render 三击全选后断言端点相等、偏移 0..11、`selected_text == "hello world"`, 再 select_all_of 复选、clear_selection 清空。
- 未做: Range 对象(createRange/startContainer/surroundContents 等)与 setBaseAndExtent 式程序化选区; 需要时再开一轮(引擎有足够状态支撑)。

验证: 全量三变体构建 ok; 12/12 套件 exit=0。

# <48>: Range 对象子集(document.createRange)

SOLVED

- 端点模型与引擎选区**刻意保持一致**: 容器 = 元素(文本节点容器解析为其父元素), 偏移 = 该元素文本的 UTF-8 字节偏移。理由: 引擎用 (element, byte offset) 寻址文本, 若改成浏览器真模型(node, offsetInNode)就得给每条 text run 建独立偏移空间, 与 sel_*/caret 全链冲突。代价已在头注释与 04 手册写明。
- API: create/destroy、set_start/set_end、start/end_container+offset、collapsed、to_string、select_node_contents、delete_contents。
- `to_string` 跨元素拼接与 <47> 的 selected_text 同规则(端点尾部/头部 + 中间完整元素), 但这里走 DOM 前序(不依赖布局树): range 是纯 DOM 概念, 不该要求已渲染。因此抽了本地 `edit_value_of/edit_set_value_of`(input 取 value 属性, 否则 textContent), 不去碰 render 侧的静态 helper。
- `delete_contents`: 同元素切片删除; 跨元素裁两端(完整结构性删除需文本节点模型, 记为取舍)。两者都 mark_dirty_struct。
- 头文件: `whaleui_dom_range_t` 是不透明 struct(不像 element 那样直通 lexbor), 因为需要 4 个字段 + 文本缓存。
- C++ 侧 `whaleui::Range`(RAII 销毁 + move)与 `Document::CreateRange()` 镜像。
- 坑: `range_owner` 参数类型写成 `lxb_dom_element_t*` 编译不过(公开层句柄是 `whaleui_dom_element_t*`), 公开函数参数必须用公开类型再 reinterpret。

验证: 全量三变体构建 ok; 12/12 套件 exit=0(test_dom 新增 Range 端点/toString/deleteContents 断言)。

# <49>: Web 平台 API 补齐(clipboard / 修饰键 / 定时器 / rAF / 视口 / 音频占位 / 手柄)

SOLVED

自查结论(用户点名 + 顺带): clipboard 完全没做; 事件对象只有 key_code/mouse/wheel 标量, **无修饰键**, 也无 key/code/repeat/detail/movement/relatedTarget; 定时器/rAF/visualViewport/audio/gamepad 全无。

## A. 事件载荷(修饰键 + 富字段)

- 新增 `whaleui_event_init_t`(key_code/key/code/repeat/mods/mouse/detail/wheel/movement/related/clipboard)与 `whaleui_dom_dispatch_input[_nonbubble/_clipboard]` 作为引擎内部统一入口; 旧的标量 `_phased`/`_nonbubble` 保留为兼容 shim(内部构造最小 payload), 老调用点不用改。
- 事件对象加字段: mods/key/code/repeat/detail/movement/related/clipboard(+clipboard_set)。访问器: mods/ctrl/shift/alt/meta、key、code、repeat、detail、movement_x/y、related_target、clipboard_text/set_clipboard_text。
- app.cpp: `mods_from_sdl`(SDL_KMOD_* -> WHALEUI_MOD_*)、`key_name_strings`(SDL keycode -> KeyboardEvent.key/.code: 可打印 ASCII -> "a"/"KeyA"、"5"/"Digit5"; 命名键表 + F1-F24; Space 的 code 特例)。keydown/keyup/click/dblclick/contextmenu/mousedown/mouseup 全改走全载荷派发。
- render 侧(无 SDL 事件上下文): app 每次唤醒把当前 modifier 写进 `render->last_mods`; mousemove/enter/leave 的 movement 用 set_hover 内缓存的上一位置算(pre-fb 缩放坐标, 与 mouse_x/y 同域)。relatedTarget 在 enter/leave 里互为对方。

## B. Clipboard

- `src/core/clipboard.cpp`: SDL_Get/Set/HasClipboardText; 异步回调**排队**到 `whaleui_clipboard_pump`(app 每轮调用)而不是内联 —— 模拟 promise 的"稍后 resolve"时序, 避免监听器被重入。get 的返回指针用静态 cache 保 C 契约。
- copy/cut/paste DOM 事件接入 render 的 Ctrl+C/X/V: 先派发(带 payload 文本), 监听器 preventDefault 可取消引擎默认行为, `set_clipboard_text` 可替换写入/插入的文本。这是"引擎默认行为可被 DOM 取消"契约的延伸。

## C. 定时器 / rAF

- `src/core/timers.cpp`: 全局注册表(mutex 保护, 可从任意线程创建)+ 循环线程 pump。`whaleui_timers_next_due_ms` 让 worker 的 wait_for 精确睡到到期(不再等 100ms 轮询, 也不空转); `whaleui_timers_pending` 让"有 setTimeout 就保持唤醒"(浏览器语义)。interval 从 due 重排避免回调耗时漂移; 0ms interval 每轮一次。
- rAF 独立: **帧后** pump(浏览器语义: 回调看到被调度那帧), 一次性, 有未触发 rAF 时 `want` 门为真保持出帧。
- 坑: 定时器 pump 放在 process_event 之后、帧之前 —— 回调里的 DOM 改动才能被本轮的 frame_request 带出去。

## D. 视口 / 音频 / 手柄

- visualViewport: 无捏合缩放 => scale=1、offset=0, width/height 跟窗口; devicePixelRatio 走 SDL_GetWindowDisplayScale。resize 事件已在 documentElement 上派发(前几轮就有)。
- Web Audio: 纯占位 —— 有 context 对象与状态机, `is_supported()==0`, resume 恒失败且不建设备; 接口形状冻结, 接真后端不改签名。
- Gamepad: SDL3 真实现。`SDL_INIT_GAMEPAD` 加入 platform init; `whaleui_gamepad_sync` 在 GAMEPAD_ADDED/REMOVED 事件上重开/关闭设备; poll 返回 W3C 顺序的轴(4)+按钮位掩码(15)+扳机; rumble 透传 SDL_RumbleGamepad。索引**稠密**(与 W3C 稀疏索引不同, 已在头/文档写明)。连接事件派发 gamepadconnected/gamepaddisconnected。

## E. 测试

`tests/test_web.cpp`(新, 13/13 套件): clipboard 同步 + 排队回调时序; 定时器一次性/清除/interval 自重清/clearTimeout 清 interval/未来到期时间; rAF 一次性/取消/与定时器互不干扰; 视口与 DPR; 音频占位状态机; gamepad 越界安全; 事件载荷全字段(修饰键四种组合、key/code/repeat、detail/movement、relatedTarget、clipboard 文本)。坑: 测试里 SDL 未初始化时 clipboard 调用失败 —— app_create 必须先行(它做 SDL_Init)。

验证: 全量三变体构建 ok; 13/13 套件 exit=0。

# <50>: 拖放(DnD) —— DataTransfer/File + OS 拖入 + 页内 draggable

SOLVED

用户点名 DnD 未做, 顺带全量自查了 web api 缺口(见 <51> 待办): matchMedia / MutationObserver / ResizeObserver / IntersectionObserver / localStorage / sessionStorage / location / history / Blob / FileReader / PointerEvent / TouchEvent / WAAPI(Element.animate) / CSS.supports / structuredClone / userAgent / requestIdleCallback / queueMicrotask 均无。

## A. DataTransfer / File(src/dom/data_transfer.cpp)

- 类型化 item 袋(format -> data, 插入序保序, set 同 format 就地替换)+ 文件列表 + effectAllowed/dropEffect。File 带 name + VFS uri, **字节惰性读**(拖入大文件零成本; size 首次调用测量并缓存)。
- 句柄是不透明 struct(不是 lexbor 直通), 因为要装 vector/string。
- 事件对象新增 `data_transfer` 借用指针 + `whaleui_dom_event_data_transfer`; `whaleui_event_init_t` 加 `data_transfer` 字段。

## B. 页内 HTML5 拖放(render 侧)

- 按下时若 hit 元素 `draggable="true"` 记录候选(`press_hit_el`); 指针过 6px 阈值(`kDndThreshold`)才 `dnd_begin` —— 与文本选择同样的门控, 保证"可拖元素上的普通点击仍是点击"。**拖放优先于文本选择**(draggable 元素拖拽不选文本)。
- `dnd_begin` 建 transfer, 预填 textContent 为 text/plain(浏览器用选区, textContent 是最接近等价物), 派发 dragstart; **dragstart 里 preventDefault 取消整个会话**(浏览器契约)。`dnd_update` 命中变化时 dragleave/dragenter, 每次 dragover 按 preventDefault 设 dropEffect。`dnd_finish` 在 accepting 目标上派发 drop, 源上总是 dragend, 最后销毁 transfer。
- `draggable` 只认显式属性: 浏览器里 img/a 默认可拖, 本引擎 UA 样式不表达, 已在头注释/文档写明。

## C. OS 拖入(SDL_EVENT_DROP_*)

- SDL 交付一组事件: BEGIN -> POSITION/FILE/TEXT* -> COMPLETE。窗口侧 `drop_dt/drop_active/drop_x/drop_y` 累积: BEGIN 建 transfer、POSITION 派发 dragover、FILE/TEXT 追加数据并派发 dragenter、COMPLETE 在最后位置派发 drop + dragleave 再销毁。COMPLETE 自身不带坐标, 所以用最后记录的 POSITION(注释写明)。
- 没有 BEGIN 的平台兜底: FILE/TEXT 事件里若 drop_dt 为空就地建一个。
- `whaleui_render_dispatch_os_drag` 做命中测试 + 派发(与页内会话无关, OS 拖入不建 dnd_active 会话)。
- 引擎对两者都不做默认行为(浏览器会导航到拖入的文件), 页面自己处理 drop —— 与既有"preventDefault 取消默认行为"契约一致。
- 窗口销毁时释放 drop_dt(否则泄漏)。

## D. 测试(test_web 追加)

DataTransfer 类型化 item 保序/就地替换/清空/effect 存取/File 惰性 size 与 VFS 读取/越界与坏参; 页内事件序列(dragstart -> dragenter -> dragover(prevented=1) -> drop(读到 dragstart 改写的文本) -> dragend); drag 事件冒泡到祖先; 非 drag 事件 transfer 为 NULL。坑: drop 读到的是 dragstart 监听器**改写后**的文本(不是初始 payload), 断言要跟着改 —— 这正验证了 dragstart 填 transfer 的语义。

验证: 全量三变体构建 ok; 13/13 套件 exit=0。

# <51>: API 挂载的 CSS 不生效 —— 采集器只跑在 load/resize/换主题

SOLVED

用户报告两件事: (1) C++ 挂载的 CSS 动画没效果; (2) lexbor 直出 CSS 与调用
API 效果不一样。两条都复现了, 而且根因是同一个结构问题 + 一批连带缺口。

## A. 根因: 文档 CSS 只在三处重收

`window_reload_css` (core/window.cpp) 把「主题默认表 + 文档 <style>/<link>」
拼成一张表交给 render。它只在 **load_html / resize / 换主题|accent** 时被调。
于是:

- 用 DOM API 追加一个 `<style>`(含 @keyframes)—— 采集器从不重跑, 规则**从未
  进入引擎**。同一个 `<style>` 写在标记里就正常, 因为第一次解析就有它。
  这就是「lexbor 直出 vs 调 API 不一样」的字面含义。
- 改写已有 `<style>` 的文本、换 `<link href>`、`<head>.innerHTML`、删 `<style>`
  同理。
- 内联 `style="animation:..."` 一直能用(它不走采集器), 所以现象看起来像
  "只有 @keyframes 不生效", 而不是"CSS 不生效"。

**修法**: 不再逐 API 打补丁, 而是在 `mark_dirty_ex` 里判定"这次编辑动到了
样式表来源没有"——判据只有两个节点类: 来源节点本身(`<style>`/`<link>` 的文本/
属性编辑), 以及 `<head>`/`<body>` 的**结构性**编辑(采集器读的就是这两个节点的
直接子节点; 所有插入/删除/移动 API 都会 mark 它碰过的父节点)。文本/属性编辑
在普通子节点上**不算**(否则每次按键都要重收整张表)。命中就置 `g_css_dirty`
(按 document 记, 见下)。

重收的触发点放在**帧内**(render.cpp 顶部), 不是只在 worker 循环里: DOM 契约
是"改动下次重绘生效", 直接调 `whaleui_render_frame` 的测试/离屏工具必须看到
同样行为。render 侧不认识"主题默认表"这层, 所以走一个 `css_reload_fn` 钩子
(window 安装), 帧发现标志就回调重收 + `has_dirty=1`。worker 循环里另有一处
`has_css_dirty` 查询, 用来在空闲时把这一帧**叫醒**。

标志按 `lxb_dom_document*` 记(不是全局 bool): 多窗口时一个窗口改样式不该让
别的窗口重解析整张表。document 销毁时清(与 css_apply 的文档表一起, 都是
地址复用风险的同类)。

## B. `whaleui_css_apply` 是个文档记载的空操作

`doc/external/05-api-css.md` 写着"将一组规则应用到文档", 实现是遍历全树算一遍
`whaleui_style_compute` 然后把结果**丢掉**——没有任何渲染路径读它, 改不动一个
像素。实测 `probe_css4` 的 E 场景: `.box{background:#00ff00}` apply 之后采样
仍是 `#ff0000`。

**修法**: 规则深拷贝进 document 的**自有表**(style.cpp 的 `g_doc_rules`),
`window_reload_css` 把它**追加**在文档 `<style>` 之后一起解析。追加而不是单独
第二张表, 是因为级联的源序 tiebreak 要求它们在同一数组里。`apply(count=0)`
= 清空(文档里的"换主题表"用法)。销毁 document 时释放。

## C. 顺带查出的同类缺口(都有实测)

`probe_css2/4` 逐条对照"标记里 vs API", 结果分成三类:

- **body 里的 `<style>` 不生效**(两条路都不生效): `collect_doc_css` 只扫
  `hd->head` 的直接子节点。HTML 允许 body 里放 style, 浏览器会应用。修: 两个
  区段按文档序都扫。
- **`media="print"` 与 `@media print` 在屏幕上也生效**: `whaleui_style_media_ok`
  从不看**媒体类型**, 只认括号里的特性; 于是任何无法识别的条件都算匹配。
  修: 先剥掉括号组(避免把 `(pointer: fine)` 里的词当类型), 再检查裸词里的
  类型, 非 `all`/`screen` 一律不匹配。`<style media>` 也按同样方式拼成
  `@media <cond>{...}` 走同一个求值器(属性以前完全被忽略)。
- **只加 @keyframes 的差异被"规则表相同"判定吃掉**: `window_reload_css` 的
  跳过逻辑只比规则数组, 不比 keyframes; 一张只改了 keyframes 的表会被当成
  "没变"丢弃。修: keyframes 也逐项比。

## D. `CSS.supports` 对一切都答 1

原实现"把 `x{prop:value}` 丢给解析器, 声明活下来就算支持"。但本项目的 CSS 解析
器**有意保留未知声明**(页面可能带厂商前缀/未来语法, 不该因此整条列表被拒),
所以 `CSS.supports("no-such-property","1px")` 返回 1——一个永远说 yes 的支持
查询比没有更糟(实测 12 个用例里 6 个错)。

**修法**: 拆成两个问题。
1. **属性**: 显式表 `whaleui_style_property_supported`(取自 layout/paint/animate
   真正读的属性名, 100+ 条; `--*` 恒真)。偏保守——错答 yes 正是让它失效的原因。
2. **值**: `whaleui_style_value_supported` 用**引擎自己的解析器**判——
   颜色走 `whaleui_render_parse_color`, 长度走 `whaleui_style_parse_len`,
   枚举属性(display/position/overflow/... 20 组)走关键字集合; `var()/calc()/
   gradient()/url()` 与"引擎尚无值解析器"的属性一律放行(那是未知, 不是不支持,
   与属性侧相反的偏置)。属性名与关键字都按 ASCII 小写比较(`DISPLAY: FLEX`)。

## E. MutationObserver 三处缺陷(新增 API 里最严重的)

- **disconnect 后仍被回调 → 跳地址 0(SIGSEGV)**: `disconnect` 只把 `cb` 置空,
  观察者仍留在注册表里; pump 只检查 `delivering/doc`, 于是下一次 mutation
  调用空函数指针。test_web 从"断言失败"变成"无输出崩溃"(exit 139), 用 gdb 定位。
  修: pump 跳过 `!cb` 的观察者(仍留在表里, 可重新 observe)。
- **一次属性编辑投递两条记录**: 属性 API 先 `notify_attr`(带名字)再
  `mark_dirty`(通用无名字), 两条都进队列。修: 名字作为 `mark_dirty_ex` 的
  参数, 一次编辑一条记录。
- **classList/dataset/style 编辑的记录没有属性名**: 它们走通用 `mark_dirty`,
  attr 为 NULL, 按名字过滤的监听器(常见写法)完全收不到。修: 把这些 API 全部
  改走 `mark_dirty_attr(el, "class"/"data-*"/"style"/<name>)`。

## F. C++ 门面缺口(用户点名: 头文件要 C 和 C++ 都要)

`whaleui.hpp` 是 C++ 程序实际用的 API, C ABI 是其导出层; 但 <49>~<50> 新增的
Web 平台 API **只有 C 版**——C++ 用户等于没有。用脚本对了一遍两边的符号:
377 个 C 函数里 148 个在 hpp 里毫无引用。

补齐: `StyleSheet`(RAII + Parse/Load/Apply, 包 `whaleui_css_apply`)、
`MediaQueryList` + `MatchMedia()`、`Navigator::*`、`Storage` +
`LocalStorage()/SessionStorage()`、`QueueMicrotask/RequestIdleCallback`、
`MutationObserver`/`ResizeObserver` 类 + `CreateMutationObserver/ResizeObserver`、
`CssSupports` + `CssSelector/CssProperty/CssHasProperty`(规则结构体在 C 头里是
不完整的, 只能走访问器)、`App::ThemeCount/Name/Label`, 以及 Window 上此前缺失的
一批: ScreenSize/ScreenWorkArea/DisplayId/DevicePixelRatio/VisualViewport、
min/max size、mouse/keyboard grab、SetMousePosition/SetFocusable/SetParent/
SetModal/SetIcon、DragActive/DragSource、IsFullscreen。
`test_cpp` 新增一整块用例覆盖每条。

## G. 测试

- `test_style`: `whaleui_css_apply` 挂表/替换/清空 + 文档表可读; 样式表来源
  失效标志(append head/body style、改 style 文本/属性、head.innerHTML、删
  style 都要置位; 普通文本/class 编辑**不置位**); media 类型矩阵。
- `test_web`: CSS.supports 的属性+值矩阵(含大小写、var/calc、自定义属性);
  MutationObserver 的 disconnect 后不再投递/重新 observe/一次编辑一条记录/
  各 API 的属性名。
- `test_cpp`: 上述全部 C++ 门面。
- **修 `test_dom` 一处过期断言**: range deleteContents 期望 `"adef"`, 但
  range 端点是**左闭右开**(同文件上面的 toString 用例 `[6,11) -> "world"`
  已经确立), 删 `[1,4)` 应得 `"aef"`。原断言从 <48> 起就是错的, 与本次改动无关。
- **test_web 剪贴板改为环境探测后跳过**: 本机 `SDL_SetClipboardText` 直接失败
  ("Couldn't open clipboard: 操作成功完成。"), 是环境没有窗口站的事实, 不是引擎
  缺陷; 改为先探一次, 不可用就打印原因跳过整块(原来必崩)。

## H. 复现/回归工具(temp/, 已 gitignore)

- `temp/repro_css_api.cpp` — 8 个场景的 PASS/FAIL(标记 vs API 挂载 @keyframes/
  改 style 文本/内联 animation/css_apply/css_apply 带 keyframes/替换表/对照)。
- `temp/probe_css4.cpp` — "标记里 vs API"逐条对照, 三态判定: OK / AGREE(共同
  限制) / DIVERGE(真正的 API 差异)。区分"两条都不支持"与"只有一条生效"是关键,
  否则会把共同的能力缺口误判成 API bug。
- `temp/probe_supports.cpp` — CSS.supports 用例表。
- `temp/probe_css2/3.cpp`、`temp/probe_clip.cpp`、`temp/probe_range.cpp` —
  分区段探针(样式表来源各路径 / 剪贴板环境 / range 端点语义)。

验证: 全量三变体构建 ok; 13/13 套件 exit=0; `repro_css_api` 8/8 PASS;
`probe_css4` 7/7 OK。

# <52>: flex 项主轴尺寸用 vh/vw 或百分比时塌成 1px

SOLVED

来源是 `temp/incorrect1/`(FlashSight 接 WhaleUI 时应用根容器被算成 `1280x1`,
窗口只画出 40px 标签条)。复现程序逐变体列出盒子,修复前 15 个变体里 5 个 BUG。

## 触发条件(实测)

一个 **flex 项**,它的**主轴**尺寸写成**视口单位**(column 父里的 `vh`,
row 父里的 `vw`)或**百分比**。与嵌套层数无关(单层即触发),与父容器自身高度
无关(auto 高度同样触发),`flex:none` 也不管用(它仍是 flex 项)。交叉轴不受
影响,`rem`/`px` 不受影响。

## 根因(三处,同一个"主轴解析没有上下文"的问题)

1. **`layout_flex` 的主轴长度走 `len_or_auto`**,它转发到 `len_px`,而 `len_px`
   把视口宽高**硬编码成 0**(`len_px_impl(v, base, em, 0, 0)`)。于是该调用里所有
   `vh`/`vw` 都算成 0。带视口的 `len_or_auto_vp` 早就存在、元素自身的
   width/height 也早就在用,只有 flex 项这条路径没用。
2. **百分比基数两轴都用 `inner_w`**。column 容器的主轴是竖直的,`height:50%`
   应基容器**高度**:820 高的 column 里 50% 得 410,实测得 640 = 1280 的 50%。
3. **`est_node_height` 也用 `len_or_auto`**(vp 0 + 宽度基数),所以
   `height:50%` 的"内容高度"估计值是 640;而它同时是 flex 主轴的
   min-content 地板(`min_main`),地板 640 把正确解析出的 410 又顶回去了 ——
   这是第 2 条修完仍显示 640 的原因,两处必须一起改。

视口单位为 0 之后的观测结果是 **1px** 而不是 0,因为有条"避免零尺寸主轴项"的
地板:`if (main_size[i] < 1 && grow[i] <= 0) main_size[i] = 1;`。`flex:none`
(默认)grow 为 0 所以命中地板;`flex:1` 的项跳过地板,它的 0 是**跟随**父容器
算出来的 —— 这解释了为什么塌的是那个 100vh 盒子、而 `flex:1` 的子项只是跟着变 0。

## 修法

- `layout_flex` 里先算出主轴上下文:`main_base`(column = 容器内容高,
  row = 内容宽)、`vp_w/vp_h`(tree->viewport_*)、`main_definite`(容器主轴
  尺寸是否确定)。主轴长度一律走 `len_or_auto_vp(main_v, main_base, em, vp_w,
  vp_h, &is_auto)`。
- **主轴尺寸不确定时百分比退化为 `auto`**(CSS 语义),不是 0、也不是回落到另一轴:
  auto 高度 column 里的 `height:50%` 子项按内容定高。
- `flex_shorthand` 多收 `main_base/vp_w/vp_h`:`flex-basis` 与 `flex: 1 1 50%`
  的 basis 走同一条 `len_px_vp`(原来用 `len_px`,vp=0 + base=0)。
- `est_node_height` 加 `pct_base_h` 参数(默认退回 inner_w 以保持旧调用点语义),
  flex 主轴调用点传 `main_base`;它内部改用 `len_or_auto_vp`。

## 验证

- `temp/incorrect1`:15/15 变体 ok(修复前 5 个 BUG)。
- `test_layout` 新增回归:flex column 的 `height:100vh` / `calc(100vh)` /
  `50%` / `20rem` / `820px` 五种主轴写法逐个断言盒子尺寸;flex row 的
  `width:100vw`;`flex:0 0 100vw` 的 basis 路径。
- **回归面**:16 张主题 × 深浅离屏渲染与 6 个 test_html 页面在修复前后
  **逐像素完全一致**(0 diff),即这次改动只在"主轴是视口单位/百分比"这条
  以前算错的路径上生效,没有动到任何原本正确的布局。用一张专门的
  `temp/flex_main_axis.html`(嵌套 100vh flex column + `height:50%`)对照:
  修复前 `#win` = 920x1、`#half` = 460,修复后 920x720 / 360(19.3% 像素不同)。
- 13/13 测试套件 exit=0;三变体构建 ok。

## 工具(temp/, 已 gitignore)

- `temp/incorrect1/` — 15 变体矩阵(修复后即回归工具)。
- `temp/probe_flexbox.cpp` — 按 id 打印整棵布局盒(定位这类问题比读代码快)。
- `temp/probe_pct.cpp` — 百分比主轴的单点探针。
- `temp/flex_main_axis.html` — 渲染对照页。
- `temp/cmp_ppm.py` / `cmp_all.py` / `cmp_html.py` — PPM 逐像素对比
  (确认改动没有波及无关页面)。
## 附带:支持矩阵里三行过期条目

修完主轴后顺手按"实测而非文档"复核了 `doc/external/html-css-support.md` 的
❌ 行(探针 `temp/probe_flexfeat.cpp` / `probe_flexfeat2.cpp` / `probe_misc.cpp`,
全部读布局盒与坐标,不看代码):

- **flex-shrink / flex-basis 实际已实现**(文档写"❌,flex 简写仅消费 grow"):
  两个 100px 项在 120px 行里各得 60px(收缩生效);`flex-basis:200px` 覆盖
  `width:50px`(basis 生效)。
- **align-content / place-items / align-self / order 已实现**:
  `align-content:space-between` 两行贴在 0 与 180;`place-items:center` 居中;
  `align-self:flex-end` 覆盖 `align-items:center`;`order:-1` 真的把 b 排到 a
  前面(b.x=0, a.x=50)。
- **grid-template-rows 已实现**:`30px 60px` 两行,第二行 y=30。
- **list-style-type 已实现**(`none` 去标记;`list-style` 简写仍未解析)。
- 确认**仍未实现**的:`visibility`(盒子照画)、`pointer-events`、`user-select`、
  `text-indent`、`vertical-align`、`word-break`、`overflow-wrap`、
  `justify-self`/`justify-items`。

已把这些行改正(并把 flex 主轴的单位/百分比解析写进单位表与 flex 行)。
教训与 `<3>` 一致:**判断引擎支持什么要看实测**,文档里的 ❌ 会长期滞后。

# <53>: 并列标签同字 / tab 切换闪退 / div 放大 / 换行差一字 / DOM 操作 O(n^2)

SOLVED

用户一次报了五个现象, 逐个复现定位。其中三个是同一个根因家族(缓存键与
悬垂指针), 两个是布局上下文缺失。

## A. 多个并列标签全显示第一个的字符串(critical)

**根因: 行布局表缓存键用了 SSO 缓冲区地址。** `tl_key` 用
`(text.data(), text.size())` 做键(为了不哈希超长文本), 但 `std::string` 的短串
存在**对象内部的 SSO 缓冲**里, 而 paint 传下来的是一个**栈上局部副本** `shown`。
于是同一行里所有**字节长度相同**的短文本得到完全相同的键: 第一个标签的行表
(含字形序列)被后面所有同长度标签复用, 画出来就是第一个的字。

复现: 四个同长度标签(Alpha/Bravo/Chair/Doubt)横排, 修复前四个格子像素完全
一致; 长度不同的标签没问题 —— 这就是"有时候"的原因。

**修法**: `tl_key` 对 <=4KB 的文本按**内容**哈希(一个标签几十字节, 成本可忽略),
超长文本才保留指针身份(它们必然堆分配且唯一, 且文档重置时整表清空)。两条
分支用不同 tag 常数, 不会互相别名。raster 缓存(`draw_text_at` 的 key)同样
把 256 字节以上的长文本改成带 `text.data()` —— 同一元素下两个长 run 以前会
共享一个 entry, 第二个画出第一个的字形。

回归: `test_render::same_length_labels`(四个同长度标签必须给出 >=3 种墨迹)。

## B. tab 切换(display:none / class 切换)闪退(fatal)

两条悬垂指针路径, 都在"文档销毁"这一侧:

1. **待处理脏队列不清**: `whaleui_dom_document_destroy` 没有丢弃该文档在
   `g_dirty` 里的元素指针。之后新文档分配到同一地址时, 队列看起来"有脏元素",
   renderer 去解引用已释放的节点。修: `dirty_purge_doc(doc)`。
2. **id 索引不清(更严重)**: 新增的 id→元素哈希索引同样以 document 地址为键,
   销毁时不清就被下一个文档继承, `getElementById` 直接返回**已释放的元素**。
   这个 bug 是本次新增索引时自己引入的, `probe_idlookup` 第二个文档立刻 SIGSEGV,
   已修(销毁时 `g_id_index().erase(...)`)。

顺带修掉 `probe_tabs`(200 轮 class/display 切换 + details + select 弹层)与
`probe_tabcrash`(200 次"带未提交编辑的 load_html")两个压力探针。

## C. DOM 操作越用越慢(O(n^2))

两处:

1. **`mark_dirty_ex` 的去重是线性扫描**: 每帧编辑 N 个不同元素就是 O(N^2)。
   实测 n=1600 时批量改 class 花 22.8ms(200 个元素 0.08ms → 完全平方增长)。
   修: 脏队列加 `element -> 下标` 哈希索引(重建只在 drain 后一次, 均摊线性)。
2. **`getElementById` 是 lexbor 的全树遍历**: 3200 个元素时单次 25.9µs,
   循环里查 N 次就是 O(n^2) —— 应用里"事件处理器按 id 找元素"正是这种写法。
   修: 每文档一个 id 哈希索引, O(1)。正确性靠三件事: 结构性编辑与 `id` 属性
   编辑都递增全局 generation(命中时还要验证元素仍挂在本文档上, 失效则重建);
   append/insert 路径就地打补丁(逐行建列表保持线性); 文档销毁时清索引。

实测(修复后): n=3200 时单次查询 0.08µs(快 300 倍), 批量编辑线性。

## D. div 尺寸放大(约 1.6x)

`estimate_content_width`(flex 项的 min-content 地板)算了 padding, 但**没算
边框宽度**。一个 `padding:6px 10px; border:1px solid` 的 flex 项, 地板比真实
需求小 2px, 于是它自己的文本被判"刚好放不下"而折成两行: 实测高度 52px(应为
33px) —— 正好是用户说的"放大了"。

修: 用 `box_border_widths`(解析 longhand + `border` 简写, 与盒模型 pass 同源)
把左右边框加进估算; `padding` 简写缺失时也读 `padding-left/right`。

## E. 换行差一个字(溢出一字)

layout 与 paint 的**换行宽度必须一致**, 但两边各写了一份推导:

- layout 走 `display_kind(...) == 2` 判断是否跳过 inline 祖先, paint 用
  `display == "inline"`; 且 layout 读的 `anc->content.w` 在子节点布局**之前
  还没赋值**(只在 pass 末尾写), 于是行首 run 拿到的是 0 或上一轮的值。
- 修: layout 在解析出 `inner_w` 后立刻写 `n->content.w`; 跳过 inline 祖先的
  条件与 paint 对齐(只跳 `display:inline`, 并带上 parent 非文本的守卫)。

验证: `temp/probe_wrap` 五个场景(混合 inline 行/普通段落/flex 长标签/
inline-block 按钮/CJK 段落)逐个检查"墨迹是否越出盒子底边或右边", 全部 ok。

## 验证

- 13/13 测试套件 exit=0。
- `probe_ssolabels` 四格墨迹不再相同; `test_render` 新增同长度标签回归。
- `probe_tabs` 400 轮、`probe_tabcrash` 200 轮无崩溃。
- `probe_idlookup`: 3200 元素单次 0.08µs(原 25.9µs);`probe_perf2` 批量编辑线性。
- `probe_sizes/sizes2/sizes3`: 边框+padding 的 flex 项 52→33;grid 3×1fr、
  inline-block、button、textarea、img 等盒子尺寸逐个核对。
- 16 张主题渲染 + 6 个 test_html 页面与修复前逐像素比对: 最大差异 3.0%
  (material 主题的按钮因 min-content 修正而不再多折一行), 主题亮度/对比自检仍
  通过(浅色 0.98 / 深色 0.07~0.23 / spread 0.66~0.71)。

# <54>: incorrect2 六项缺陷 + 动态编辑性能 + "卡在错误位置等 hover 才回正"

SOLVED

来源: `temp/incorrect2/` 用 FlashSight 的真实 `web/style.css` 渲染设置页/探索页,
与浏览器逐屏对比; 以及用户报告 "做一些反应后会卡在错误的位置, 鼠标动一动
(触发 hover) 才好"。

## A. 性能: 一次宽度编辑 98ms -> 0.04ms (计算层, 不是渲染层)

`probe_perf7`(800x600, flex-column 应用壳, N 行): 一次 `width` 编辑的
`relayout_multi` 从 98ms(n=3200) 降到 **0.04ms**, 与 N 无关;整帧 111ms -> 3.3ms。
两个独立原因:

1. **flex column 的 auto 高度项被当成 definite 主尺寸赋值**。主轴尺寸来自
   `est_node_height`(纯 CONTENT 高度), 却被直接写成 `k->border.h`(BORDER
   高度), 每行少掉 padding+border(18px 的盒子装 23px 内容)。于是任何宽度
   编辑引起文本重排 -> 高度"变化" -> 快路径拒绝 -> 整个 3200 行容器重排。
   修: 只有 definite 项(flex-basis / 显式主轴长度)才强制写入解析出的主尺寸;
   auto 项保留自己布局出的高度; `content_main` 对 column 项补上 padding+border。
2. **`splice_child_link` 漏更新 `parent->first_child`**。它从 chunk 的首成员
   起走, 替换的节点恰好是父节点第一个孩子时只改了 chunk 指针, 父节点的
   `first_child` 仍指向旧节点 —— 重建的子树从树上不可达, 编辑看起来"没生效"
   (随后又按旧节点重排)。修: 替换后显式 `if (parent->first_child == old)
   parent->first_child = fresh;`。

## B. 覆盖样式不对 + 必须外部触发才回正 (用户报的现象)

复现: `probe_desc` — `.box.big .row{height:80px}`, 给 `#box` 加 class。
切换后 `#row` 仍是 20px, **直到鼠标移动触发一次 hover 才变成 80px**。
两个独立 bug:

1. **后代复合选择器上的状态伪类被完全跳过**。`whaleui_style_match` 在匹配
   祖先复合时 `continue` 掉, 于是 `.card:hover .acts{display:block}` 恒真
   (加载时就显示), 任何 `A:hover B` 规则无条件生效。修: 祖先复合也要测
   :hover/:active/:focus/:focus-within/:not(...)(对 `cur` 而非目标元素)。
2. **属性/class/状态编辑只重算元素自己**。`kLayoutKeys` 差异只看被编辑元素,
   后代选择器命中的子元素拿不到新级联; 且 layout 的 `style_cache` 按元素身份
   缓存, 重建子树时会复用旧级联。修: 按规则集缓存"祖先前缀"列表
   (`whaleui_style_ancestor_prefix`), 编辑元素命中任一前缀就重建整个子树并
   清掉子树的 style_cache。只在真的有后代选择器命中时才走重路径。

验证: `probe_desc` 切换后同帧 `row=80`, `after=80`; `probe_hoverdesc`
`.card:hover .acts` 初始隐藏、hover 显示。

## C. incorrect2 六项

| # | 用例 | 修前 | 修后 |
|---|---|---|---|
| 1 | `overflow-span-flex` | 1x342 | **228x18** |
| 2 | `button-explicit-size` | 34x32 | **34x19**(UA 改 `height`, 不再 `min-height`) |
| 3 | `settings-grid` field1 | 122x64 | **484x46** |
| 4 | `settings-grid` lbl2 | 118x36(折行) | **118x18** |
| 5 | `settings-grid` tog | 8x32 | **34x19** |
| 6 | `flex-fill-select` | 866 | **866**(= `flex-fill-div` 的 918 差在 `.field` 的 gap, 非 select 特例) |

grid 的两个根因: `repeat(` 在 `minmax(...)` 的 `)` 处提前闭合; `auto_repeat`
未初始化。另: `content_main` vs `min_main` 分离(overflow:hidden 只降低收缩
地板, 不降低 hypothetical size), flex 主轴百分比/视口单位上下文, 见 note 52。

## D. 其它修复

- `win->doc_bytes` 未初始化 -> 异步切片首布局误判, 编辑永远落不到树上。
- `getBoundingClientRect`/`getComputedStyle`/ResizeObserver/Tab 焦点原用
  arena 扫描, 会读到被替换的旧节点 -> 改走 `by_el` 活节点表。
- `kLayoutKeys` 补齐 border 简写/字体族/字重/word-spacing/text-transform/
  aspect-ratio/flex-grid 对齐/grid 轨道/content/list-style-type。
- `est_node_height` 的 `pct_base_h`(百分比高度基数不再用宽度)。
- 每元素 relayout 改为批量 `whaleui_layout_relayout_multi`(原 N 次全树盒 pass)。
- `rechunk_parent`(O(children) + 每次泄漏 chunk)改为 `splice_replace_child`。

## 验证

- 13/13 测试套件 exit=0; 新增回归: test_layout(首子节点 relayout 链、
  flex-column auto 高度含 padding/border)、test_style(祖先 :hover 复合、
  前缀助手)、test_render(后代选择器 class 编辑同帧生效)。
- `probe_parity` 20 项 HTML/C++ 一致; `probe_layout` 0 bad case。
- `probe_perf7`: relayout_multi 0.04ms / edit_frame 3.3ms(n=3200, 原 98/111)。
- `probe_perf8`(宽度编辑导致重排换行): 3.3ms, 与 N 无关。

# 55: 结构性问题 —— DOM/渲染竞态、tab 切换卡顿、文本编辑丢失、标点换行

SOLVED

用户反馈: "同样的操作，有时整页渲染消失，有时卡顿不动，有时 css 不更新";
"tab 切换极其卡顿，卡到无法正常渲染"; "小文字和标点结尾还会换行或截断";
"动画时对预渲染帧进行变换（目前实现严重怀疑会重建树）"。

## A. DOM 与渲染线程完全无同步 (根因: 结果不可复现)

`src/dom/dom.cpp` 里 **没有任何 mutex/atomic**。渲染 worker 线程遍历 lexbor
文档（级联、重建、命中测试、文本 run），应用在主线程/事件回调里改 DOM。
lexbor 本身不加锁，两者交错就损坏树: 同一串 API 调用，一次渲染正确，下一次
整页空白或卡死。

修: 全局 `std::recursive_mutex`（`whaleui_dom_lock/unlock` + `WhaleUIDomLock`）。
- 106 个公开 DOM 入口统一加锁（脚本批量插入，跳过内部 mark_/take_ 辅助）。
- `whaleui_render_frame` 在 `tree_mx` 之后持有同一把锁（递归，帧内派发监听器
  仍可改 DOM）。
验证: `probe_race` 在第二个线程里并发编辑 40 帧期间完成 **79,671 次编辑**、
活节点 156,762，无崩溃无卡死。

## B. tab 切换: 42ms 的级联 + 20ms 盒 pass -> 0.03ms + 20ms

`probe_tab`(1280x820, 1600 行/面板) 一次 display 切换 **243ms** -> **20.5ms**。
三段修复:

1. **隐藏子树不建文本 run**（`layout.cpp` build 里 `n->visible &&` 条件）。
   于是 `display:none -> block` 必须重建整棵子树（1600 行）。改为始终构建
   run（`layout()` 仍对不可见节点立即返回，不算几何）。
2. **`can_restyle` 形状比较漏掉文本节点**。它只对齐「元素」子节点，所以
   `set_text` 到无元素子节点的 div 被判为「形状相同」，跳过重建 -> run 从未
   创建 -> **盒子高度 0，文字消失**（用户报的"整页渲染消失"之一）。改为
   同时对齐文本节点（连续文本合并成一个 run，只比较 text/non-text 交替）。
3. **浅 restyle 判断错了**: 拿新级联直接和节点 style 比，而节点 style 含继承
   与动画值，必然不等 -> 每次全量级联 42ms。改为只比较会**向下继承**的键
   （font-size/family/weight/style、line-height、letter-spacing、word-spacing、
   color、text-align、text-transform、white-space、cursor、visibility、
   list-style-type），并先按 build 的规则做一次父级继承合并。
   另有 descendant-selector 例外: `.box.big .row` 命中时必须重建子树，
   否则后代保留旧级联（上一轮修过的 bug 会回归）。

结果: restyle 42ms -> 0.03ms；剩余 15-20ms 是真正必要的盒 pass（面板此前
从未布局）。

## C. 文本: 标点不能出现在行首 (UAX #14 子集)

`"value."` 自然宽 33px，在 31px 容器里被断成 "value" + "."（2 行）；
`“ok”` 在 20px 下同理。实现禁则处理:
- `no_line_start`: `, . ; : ! ? ) ] } %` + 全角/中文标点 `、。，．：；！？）」
  』】` + `” ’ »`
- `no_line_end`: `( [ {` + `（「『【` + `“ ‘ «`
断点字符属于前者时**前移**（把标点留在本行，允许溢出）；属于后者时把断点
推过它。修 `br` 到达末尾后外层又 push 一个空行导致高度翻倍的重复行。

验证: `value.` 在 avail 30/31 下 1 行、12px 下仍正常换行。

## D. CSS API 挂载的 @keyframes 从不生效

公开 `whaleui_css_parse` 解析后**丢弃** keyframes（签名没有返回位），所以
`StyleSheet::Parse` / `whaleui_css_apply` 无论怎么写都产生不了动画。
新增 `whaleui_css_apply_text(doc, css, len)`（内部走 `whaleui_css_parse_full`）
+ `whaleui_style_doc_rules_set_full` / `whaleui_style_doc_keyframes_get`；
`window_reload_css` 把文档自有 keyframes 追加进同一份注册表。C++ 门面加
`StyleSheet::ApplyText`。

## E. 动画不播（启动帧 alive=0）

`alive` 用帧首的 `animating` 快照。动画在这一帧的 relayout 里才注册，帧尾
仍按旧值算出 `alive=0`，worker 立即 park——改窗口大小或动鼠标才恢复。
两处门控（正常尾部 + swapchain 失败分支）都改为
`animating || whaleui_anim_active(r->anim)`。

## F. 着色器缓存（启动 1.2s -> 0.7s，第二个窗口 1.1s -> 0.28s）

`whaleui_gpu_create` 用 DXC 运行时编译 10 个着色器，**每个窗口重编一次**。
加两层缓存: 进程内 memo + 磁盘条目（`SDL_GetPrefPath("WhaleUI","shader-cache")`，
`WHALEUI_SHADER_CACHE=0` 关闭 / 设为目录则覆盖）。
版本安全: 键 = HLSL 全文 + entry + profile + spirv 标志 + 额外参数 +
`WHALEUI_VERSION` + 缓存格式版本 + **dxcompiler.dll 的大小与 mtime**。
任一变化都产生新文件名，旧条目永不误用（`probe_verify_cache` 实测改一个
shader 只新增 1 个 key，其余复用）。写入用 tmp+rename，避免并发读到半个文件。

## G. 其它结构修复

- `whaleui_window_load_html_len`: **先在旧文档上 reset_dom，再销毁文档**。
  原顺序反了，异步首布局 worker 在文档释放后继续读 lexbor 节点
  （`probe_startup_stress` 去掉这个 join 后 40 轮即 0xC0000005，加上后全通过）。
- `whaleui_render_reset_dom`: 先 join 异步布局 worker 并丢弃其结果。
- `whaleui_app_create`: `key_cb`/`key_ud`/`frame_request`/`frame_done`/
  `frames_alive`/`watch_generation` 从未初始化（`new` 不置零），垃圾函数指针
  在首次按键时会被调用。
- `whaleui_window_create`: `last_mouse_x/y` 未初始化 -> 首个 mouse 事件的
  movementX/Y 是垃圾。
- `<style>`/`<script>`/`<head>` 里的文本 run 现在是 invisible（`add_run` 继承
  父节点 `visible`）。此前 `<style>` 的 CSS 文本被当作页面可见文本，是"页面
  出现奇怪文字/首行错位"的来源之一。
- 块流增量重排: 纯 block 容器里子节点高度变化只移动后续兄弟
  （`propagate_block_delta`），不再整容器盒 pass。

## 验证

- 13/13 测试套件 exit=0。新增回归: 文本编辑建 run（test_layout）、
  禁则换行（test_render）。
- `probe_tab`: 1600 行 tab 切换 243ms -> 20.5ms；restyle 42ms -> 0.03ms。
- `probe_parity`: 19 项 HTML vs C++ 行为一致。
- `incorrect3`: 全部用例 ok（grid 绝对子元素、max-width 居中、
  spread-only box-shadow 环形、绝对定位不吃格）。
- `probe_race`: 并发 79,671 次编辑无异常。
- 绘制成本实测: 整窗重绘 0.1ms（3200 行），瓶颈一直在布局而非绘制。

# 56: 线程解耦 —— 帧不再长时间独占文档锁，写入可排队

SOLVED

用户: "卡住动不了，干什么都没有反应，说明到现在 worker 线程还依赖 render 线程...
这不只是渲染更新跟不上的问题，更是线程解耦没做好。我原本对于多线程访问 dom
ecs-tree 的设计是只有管布局的线程可以写，其他线程可以无延迟的只读或队列操作
的修改（正好可以做上百个元素的重建合并）"

## A. 帧整段持锁 -> 分段持锁（卡顿的直接原因）

上一轮给 DOM 加了全局递归锁，但 `whaleui_render_frame` 从帧首持到帧尾。
一个 3200 行的重帧（全屏 resize）会独占 39ms，期间应用侧任何 DOM 调用都被
阻塞 —— 在用户看来就是"卡住动不了"，与 UI 线程死锁无法区分。

改为**按阶段持锁**:
- 持有: 样式表重收、脏队列排空、级联 + 子树重建、文本测量（这些真的读 lexbor）
- 释放: 盒 pass、bounds 遍历、绘制（只操作已建好的布局树；style 和 text 都已
  拷贝进节点）

实测 `probe_domblock`（3200 行，worker 连跑重帧，另一线程调 `set_style`）:
最坏延迟 **39.5ms -> 0.0ms**。

## B. 排队写入 (whaleui_dom_post)

新增零延迟写入路径，对应用户的设计意图"其他线程可以无延迟的队列操作":

```c
void whaleui_dom_post(whaleui_dom_mutation_fn fn, void* userdata);
int  whaleui_dom_pump_posted(void);   /* 帧在布局前调用 */
```

`whaleui_dom_post` 只推一个函数指针进队列（不碰 lexbor），调用方立即返回。
帧在布局之前、持锁状态下按提交顺序执行它们，所以同一帧内上百个编辑被合并成
一次重建。

实测 `probe_post`: 200 个 `set_style` 以 **0.41ms** 提交，一帧 11.3ms 全部生效。

## C. 批量编辑确实被合并

`probe_batch`（800 行页面，N 个元素同一帧内改 width）:

| N | 编辑耗时 | 一帧耗时 |
|---|---|---|
| 1 | 0.42ms | 2.68ms |
| 10 | 0.03ms | 2.13ms |
| 50 | 0.06ms | 4.71ms |
| 200 | 0.29ms | 13.73ms |

200 个元素只花一帧（13.7ms），不是 200 次重建 —— 合并路径工作正常。

## D. 增量渲染与全量重建逐像素一致

`probe_partial2`: 改一个元素高度后，增量帧与强制全量重建的 `text_layer`
**diff = 0 px**，兄弟节点几何也正确（y=200）。用户观察到的"先出现内部元素"
不是增量绘制留下的残影。

## E. 重建成本分布（全屏 resize, 3200 行）

| 阶段 | 耗时 |
|---|---|
| 盒 pass | 28.5ms |
| fix_run_heights | 6.7ms |
| fix_scroll_max | 0.9ms |

视口尺寸变化必须重算所有宽度，盒 pass 是必要的。全屏一帧 83ms 属合理范围
（低频操作），不再是"没动 DOM 也极高重建开销"的情况。

## 验证

- 13/13 测试套件 exit=0。
- `probe_domblock`: DOM 调用延迟 39.5ms -> 0.0ms。
- `probe_post`: 排队写入 200 项 0.41ms 提交、单帧合并生效。
- `probe_partial2`: 增量 vs 全量 diff=0。

# 57: DOM 编辑不唤醒渲染线程（"改 display 后十几秒不动"）

SOLVED

用户实测（前一轮代码库）: "进入新的页面（C++ 控制 display 做的），鼠标不动，
等了十几秒完全没有变化，鼠标移到有 hover 的元素卡死 2s 后全部画出来了"。

## 根因: 帧请求条件里没有 DOM 脏队列

`app.cpp` 的主循环决定是否渲染:

```c
bool want = !evs.empty() || frame_request || frames_alive || hover_dirty ||
            wheel_dirty || timer_dirty || animation_frame_count() > 0 ||
            resize_observers_pending();
```

**没有 DOM 脏队列。** 应用侧 `whaleui_dom_set_style` 只是把元素推进
`g_dirty()`，不设置 `frame_request`。于是 worker 继续 park，屏幕保持编辑前的
像素，直到某个无关事件（鼠标移动产生 hover）才把 worker 叫醒 —— 一次把所有
积压的编辑一起画出来，看起来就是"卡死后突然全刷新"。

修: 新增 `whaleui_dom_pending_any()`，加入 `want` 条件。同时给排队写入加
`whaleui_dom_posted_pending()`，避免"已排队但还没跑"的帧被 park 掉。

验证 `probe_wake2`: 用 C++ 改 display 隐藏 a、显示 b，**不注入任何输入事件**，
循环自己渲染了 1 帧，布局树正确（a vis=0、b vis=1 且 h=50）。

## 顺带: 增量帧与全量重建在 GPU 上逐像素一致

`probe_one2` 用 `SDL_DownloadFromGPUTexture` 读回 `target2`:
- display 切换后，增量帧 vs 强制全量重建: **diff = 0 px**
- 中心像素绿色 (expected)，布局树 bounds 也正确

`probe_round`/`probe_round2` 同样确认圆角在首帧就正确绘制，且增量与全量
一致。用户报告的"只有一个元素画出来、圆角是方块"在这条修复后无法复现 ——
最可能的解释就是上面那个不唤醒：只有最先完成编辑的那个元素赶上了某次
真实帧，其余都停在上一个画面，而圆角"方块"是被部分覆盖后的残影。

## 边界脏标记: 只标真正移动的子树

几何变化时原来对整棵树 `mark_bounds_dirty_all` + 全树
`compute_paint_bounds`。改为对每个被编辑的元素 `mark_bounds_dirty`（标记向上
冒泡到根），只有 `struct_dirty`（增删节点）才保留全树标记 —— 增删会移动它之后
的所有兄弟。

## 批量编辑的增长率（实测，800 行页面）

| N | edit | relayout_multi | frame |
|---|---|---|---|
| 1 | 0.29ms | 0.15ms | 2.22ms |
| 5 | 0.02ms | 0.28ms | 1.23ms |
| 20 | 0.05ms | 1.50ms | 3.41ms |
| 100 | 0.09ms | 3.84ms | 5.73ms |

`relayout_multi` 每元素约 33-38µs（线性、斜率稳定）。20 个元素的整帧约
3.4ms，仍在 60fps 预算内。用户看到的"20 个元素就危险"来自真实页面更重的
样式级联，不是重排本身。

## 验证

- 13/13 测试套件 exit=0。
- `probe_wake2`: 无输入自动重绘 1 帧，状态正确。
- `probe_one2`: GPU 读回，增量 vs 全量 diff=0 px。
- `probe_partial2`: 同样 diff=0。

# 58: relayout_multi 每元素重复级联（200 元素批量 12.4ms -> 2.9ms）

SOLVED

用户: "我打错了是 200，200 已经到 13ms 了那才是危险。...因此你还是把 tree pass
再检查一下吧"

## A. 每元素做了两次完整级联

给 `whaleui_layout_relayout_multi` 加分段计时（build / restyle / loop / box）：

```
n=1    build=72us   restyle=24us   loop=53us
n=20   build=365us  restyle=204us  loop=196us
n=100  build=2190us restyle=1010us loop=866us   (总计 4.16ms)
```

`build` 段最大，但 `can_restyle` 对 126/126 全部通过——根本没走 `build()`。
那这个时间是什么？是判断本身：

```cpp
WhaleUIComputedStyle fresh = whaleui_style_compute(el, ...);  // 一次完整级联
...
restyleFull.push_back(display_only ? 0 : 1);
```

然后 `restyle_subtree()` 里又做了一次：

```cpp
WhaleUIComputedStyle s = whaleui_style_compute(el, rules, rule_count, vars, &st);
```

**每个被编辑元素跑两遍完整级联。**

修: `restyle_subtree(n, full, root_style)` 接受已经算好的 cascade，调用方把
`display_only` 判断里那次的结果传进去。

| 指标 | 修前 | 修后 |
|---|---|---|
| build 段 (n=100) | 2190µs | 1716µs |
| restyle 段 (n=100) | 1010µs | 169µs |
| relayout_multi (n=100) | 4.16ms | 2.86ms |
| 每元素 | 41.6µs | 28.6µs |

100 个元素 2.86ms，200 个约 5.5ms。首次调用（n=1）受冷缓存影响，稳态每元素
约 28µs。

## B. 单元素编辑与页面规模无关

`probe_paintpay`（同一页 400~3200 行）：

| rows | colour_edit_frame | width_edit_frame |
|---|---|---|
| 400 | 3.42ms | 4.16ms |
| 800 | 4.87ms | 6.91ms |
| 1600 | 3.40ms | 4.13ms |
| 3200 | 3.97ms | 4.56ms |

单元素 width 编辑 4-7ms，且**不随页面行数增长**——增量路径确实局部化了。
（200 元素批量 13.7ms 中 relayout 只占 3.2ms，其余是 200 个元素各自的局部
盒 pass，属于必要工作。）

## C. 边界失效只标被编辑的子树（前一轮遗留）

几何变化时原来 `mark_bounds_dirty_all` 标记整棵树再全树 recompute；改为对每个
被编辑元素 `mark_bounds_dirty`（冒泡到根），只有 `struct_dirty`（增删节点）才
全树标记——因为插入/删除会移动它之后的所有兄弟。

## 验证

- 13/13 测试套件 exit=0。
- `probe_partial2`: 增量 vs 全量 diff=0 px。
- `probe_one2`: GPU 读回，display 切换后增量 vs 全量 diff=0 px。
- 所有临时计时插桩已移除（`WUI_PROF_MULTI` / `WUI_PROF_FRAME` 计数为 0）。

# 59: 批量编辑的 O(n^2) 高度位移 + paint-only 编辑仍在重建子树

SOLVED

用户: "我打错了是 200，200 已经到 13ms 了那才是危险" 以及指向真实项目
`C:/aaa/FlashSight`（src/ui 是响应式框架 + ui_dom_whale.cpp 适配层）。

用 `probe_fsmode` 复现适配层的调用模式（每元素多次 set_style）发现:
- `set_style` 本身极便宜（0.9µs/次，200 元素 × 5 属性 = 0.88ms）
- 但**后续那一帧 35.29ms**

`probe_display` 逐属性隔离（200 元素各改一个属性）:

| 属性 | 修前 | 修后 |
|---|---|---|
| width | 10.10ms | 9.97ms |
| **height** | **33.00ms** | **19.95ms** |
| color | 9.91ms | 9.75ms |
| padding | 8.78ms | 9.45ms |
| display | 9.02ms | 9.00ms |

## A. height: O(n^2) 的兄弟位移

`relayout_multi` 的块流快路径对每个高度变化的元素调用
`propagate_block_delta` → `shift_siblings_after`，把该元素**之后的所有兄弟**
整棵子树下移。200 个元素各改高度 = 200 × 800 次子树位移。

修: 收集到 `pendingShift`（按容器分组），循环结束后**每个容器只走一遍**:

```cpp
int acc = 0;
for (c = par->first_child; c; c = c->next) {
    if (acc != 0) shift_tree_down(c, acc);
    if (auto it = deltaOf.find(c)) acc += it->second;
}
```

每个编辑过的子节点先按"它之前所有编辑的累计位移"移动，再加自己的增量。
容器 auto 高度按总量增长；若祖先不是纯块流则回退到祖先盒 pass。
33ms → 20ms。

## B. paint-only 编辑仍在重建子树

color/opacity 这类不改布局的编辑走 `whaleui_layout_relayout_style`：它
**重建整棵子树**再把旧几何 copy 回去（`copy_geo`），每个元素 O(subtree)。
200 个元素因此要 9.4ms。

修: 新增 `whaleui_layout_restyle_inplace(tree, el, ...)` —— 只做形状校验 +
`restyle_subtree(n, true)`，不重建、不做几何 pass。返回 0 成功 / 1 元素不在树
/ -1 形状不匹配（调用方回退到重建路径）。render.cpp 的 paint-only 分支改用它。

200 元素 color 编辑: 9.44ms → **6.97ms**。

## C. 正确性

- 13/13 测试套件 exit=0。
- `probe_partial2`: 增量 vs 强制全量重建 **diff=0 px**。
- `probe_one2`: GPU `target2` 读回，display 切换后增量 vs 全量 **diff=0 px**。

## 备注

FlashSight 自身当前链接失败（`build_p2r.txt` 里大量 mbedtls undefined
reference），无法直接端到端复现，因此本轮用 `probe_fsmode` / `probe_display`
复刻其调用模式。剩余 ~9-10ms 的基线（与属性无关、随 N 线性）是每个元素一次
完整级联 + 局部盒 pass 的成本，下一步可考虑级联结果复用。

# 60: 丢渲染的真正原因 —— 移动过的节点没有标记 paint bounds 失效

SOLVED

用户: "先提交，但别急着结束，因为现在还是卡，而且丢渲染我不确定你能一次性
解决。我建议你还是对代码继续审查，先解决掉过多的树重建，然后看看哪些代码会
阻止元素被渲染，然后看它们的触发场景是否有误——前面丢渲染大部分是这种场景"

## A. 根因: 移动了子树但没置 bd

paint 侧的裁剪条件只有三个（`paint_node` 的全部提前返回）:
1. `!n->visible`
2. `paint_cull(...)` —— 用 `n->bounds`（绝对视口坐标）判断是否在重绘区域内
3. `chunk_culled(...)` —— 用 chunk 的合并 box

`compute_paint_bounds()` 的第一行是:

```cpp
if (!n->bd) return;   // 认为"这棵子树没动过，bounds 仍然有效"
```

而 `shift_tree_down` / `shift_subtree` 直接改 `border.y/content.y`，**没有置
`bd`**。于是节点移动后 bounds 还停在旧位置，`paint_cull` 用旧矩形判断 ——
移进视口的元素被裁掉（不渲染），移出视口的元素留着旧像素。

这正是用户说的"hover 有时候会让部分元素消失，不切换一下状态就不显示"。

修: 两个 shift 函数在移动前 `n->bd = 1`（bounds 是绝对坐标，整棵子树都失效）。
其余原地几何修改处（`propagate_block_delta` 的祖先增高、批量增量的容器增高、
grid 单元格拉伸、行盒定位、append_rows 的 list/祖先增高）此前已分别置位，
现在共 13 处。

### 消融验证

`probe_lost`（400px spacer 把 target 顶到视口下方，再把 spacer 缩到 40px）:

| | target y | bounds | 结果 |
|---|---|---|---|
| 有 bd 标记 | 40 | `0,40 100x50` | 在视口内，正常绘制 |
| 去掉 bd 标记 | 40 | `0,400 100x50` | **"NO (will be culled)"** |

几何正确但 bounds 陈旧 —— 元素被静默丢弃，与用户现象完全一致。

## B. specificity 缓存

`sel_specificity()` 是 selector 文本的纯函数，但级联对每个 (元素, 候选规则)
组合都重新解析一次字符串。加了按 selector 文本的缓存（带 8192 条上限）。
级联 11.8µs → 10.7µs/元素。

## C. 一次失败的尝试（已回退）

把 selector chain 缓存从 `std::map<std::string, ...>` 改成按 selector
**指针**键（规则里的 selector 是 strdup 的，生命周期稳定）。测试立刻挂了 4 个
（`test_dom` querySelector、`test_style` `:has`、`test_layout` 文本合并、
`test_scroll` select 行）——分配器把释放掉的 selector 地址复用给了下一个
stylesheet，新 selector 继承了旧 chain。已回退为文本键。

## 验证

- 13/13 测试套件 exit=0。
- `probe_lost`: 移动后 bounds 同步更新。
- `probe_partial2`: 增量 vs 全量 **diff=0 px**（CPU text_layer）。
- `probe_one2`: GPU `target2` 读回，display 切换后增量 vs 全量 **diff=0 px**。
