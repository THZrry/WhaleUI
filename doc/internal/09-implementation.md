# 实现状态(第三步)

记录第三步「内部实现」的完成情况与已知限制。文档描述的是**已实现**的行为,与代码一致。

## 总体进度

| 子步骤 | 内容 | 状态 |
|--------|------|------|
| 1 | DOM 功能(接入 lexbor) | ✅ |
| 2 | 标签功能(tag→div 适配) | ✅ UA 默认样式全覆盖 + inline 混排 + 交互 tag + tag_id 组件(见 `11-ecs.md`) |
| 3 | 样式处理 | ✅ 解析/匹配/级联/var |
| 4 | 渲染实现 | ✅ CPU 绘制 + SDL_GPU blit,软件回退 |
| 5 | 流程处理 | ✅ 事件循环 + 帧率上限 |
| 6 | 外围功能(应用/窗口 API) | ✅ |
| 7 | 样式(系统风格转换) | 🟡 默认样式 + 深浅色;7 套系统风格未逐一转换 |
| 8 | 测试 | ✅ 11 个测试全部通过(含新增 test_canvas) |
| 9 | JS API C-port(窗口/Style/Canvas/WebGPU) | ✅ 窗口 SDL 全量 + HitTest + per-window 回调 + Style 声明式 + Canvas 2D 核心 + WebGPU stub(2026-10-13,de48f1c/2cea474/614740a;外部手册 02/03/04/08,交接清单 more-dom-api.md 已勾选) |

## 架构修正(相对第二步文档)

1. **布局不由 lexbor 计算**。lexbor 提供 HTML/CSS **解析**与选择器,没有可用的 CSS layout 引擎;`src/layout/` 自研盒模型 + 基础 flex(第二步文档中的假设不成立,已按实际修正)。
2. **CSS 解析自研**,未接 lexbor 的 CSS 语法树(接口复杂且本项目只需子集);解析器位于 `src/style/css.cpp`。
3. **渲染统一走 SDL_GPU**:CPU framebuffer 上传离屏纹理后 blit 到 swapchain,底层 D3D12 / Vulkan 由 SDL 自动选择(官方 mingw 预编译包只编译了这两个 backend)。注意 SDL3 3.4.x 起 `SDL_CreateGPUDevice` 必须显式声明 shader 格式(`SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV`),传 0 会被 D3D12/Vulkan 后端直接拒绝。**Vulkan 已验证**(`WHALEUI_GPU=vulkan`,test_render 像素断言通过):HLSL 经 DXC 交叉编译为 SPIR-V(着色器内用 `[[vk::binding()]]` 把资源钉到 SDL 期望的 set0=采样纹理 / set1=读写 storage / set2=uniform),FSR 使用预编译 SPIR-V;注意本机 DXC 1.9.0.5402 不识别 `-fvk-bind` 命令行选项,所以绑定信息写在 HLSL attribute 里而非编译参数。**Metal 未实现/未验证**(非 Windows 的 shader 编译路径返回 nullptr 并打印提示)。**适配器选择**:`WHALEUI_GPU_LOWPOWER=1` 设置 `SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN` 偏向核显;默认不设——台式机通常该走独显,且 Windows 的按应用图形设置(设置 > 系统 > 显示 > 图形)优先级更高,让系统决定即可。
4. **SDL3 采用官方预编译包**(`3rdparty/sdl3/` + `3rdparty/sdl3_ttf/`),因为 `SDL3_ttf.dll` 运行时依赖 `SDL3.dll`。
5. **blur 类效果 = mipmap 模糊近似**。`box-shadow`/`backdrop-filter: blur()` 不再用逐像素高斯核(大半径下开销随半径线性增长):模糊源(圆角矩形形状 / 已绘制几何)写入半分辨率 `blur_tex`,用 `SDL_GenerateMipmapsForGPUTexture` 生成 mip 链(每级 2x2 box 过滤),compute shader 对每像素在 3 个相邻 mip 级别上 `SampleLevel`(显式 LOD,高斯权重)并叠加回 target——一次模糊 = 3 次采样,小半径(`blur<8`)用预定义 3x3 二项式核作用于单 mip 层(见 notes <15>)。依据 SDL_gpu.h 官方 API(`num_levels`、`SDL_GPUSamplerMipmapMode`、`SDL_GenerateMipmapsForGPUTexture`、`SDL_BindGPUComputeStorageBuffers`)。**为什么走 compute**:SDL 3.4 的 D3D12 后端创建带纹理采样的 graphics pipeline 返回 `E_INVALIDARG`(`kSolidPS` 注释记录),图形管线只能采样 1x1 white 纹理 + 顶点色;compute 的 `SampleLevel` 无此限制(`kTextCompositeCS` 同样避开隐式导数 `Sample`)。box-shadow 因此从每元素 ~blur/2 个同心环 fill 降为 1 次形状绘制 + 1 次 compute;backdrop-filter 为几何拷贝 + mip + compute 区域替换(元素自身 body——纯色/渐变/边框——走 LATE pass 画在模糊之上,不再丢失,区域按圆角 SDF 裁剪),target 需要 `COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE`,其格式随之从 B8G8R8A8 改为 R8G8B8A8。**compute 全部区域化**(notes <29>,高帧率 GPU 占用的主修):五个区域 pass(shadow/backdrop/inset/filter/mixblend)的 dispatch 只覆盖当帧记录的包围盒(偏移经 uniform 传入,dispatch 取整到 8x8 组,边缘线程按 hdr 的 fb 尺寸丢弃);shadow 的目标清零由全屏 RMW compute 改为固定功能 clear pass;backdrop/filter 的模糊源 blit 也只拷记录盒 + mip 足迹外扩(blur≥8 外扩 520px、小模糊 16px,源坐标偶对齐保证与半分辨率 blur_tex 1:1)。像素输出与全屏版逐字节一致(主题对照实测),4k 单个小元素模糊的工作组数从 ~13 万降到 ~千级。partial(hover/动画)帧的主 raster pass 另有条带 scissor(固定功能丢弃条带外片段,全页背景大四边形不再逐像素跑 SDF PS);composite 的 uniform zw 分量改为携带区域尺寸作越界守卫。纯滚动帧(复用上一帧 target)跳过模糊,滚动停止后的全量重绘恢复(见 `gpu_flush` 的 `do_shadow`)。**inset 阴影**复用同一金字塔:mask 源是 spread-收缩的白盒,`kInsetCS` 对每像素算 `1 - blurred_mask(px - offset)`(offset 通过移采样点精确实现,无四象限近似、无接缝),再按元素圆角 SDF 裁剪,blend 到已绘制的背景上(inset 在元素背景之上);几何 + backdrop pass 之后顺序是 late-body → inset,所以 inset 仍盖在 body 上。compute param buffer 按记录跨度独立定尺寸(shadow/backdrop 12 floats,inset 16 floats)——混用一个尺寸曾让 `UploadToGPUBuffer` 越界,D3D12 直接整帧黑屏(见 notes <16>)。
6. **标签功能(子步骤 2)**:所有 tag 是"带默认样式的 div"——
   - **UA 默认样式全覆盖**(`src/style/theme.cpp` 的 `base_css`,参照 WHATWG HTML 标准 UA stylesheet):`display:none`(元数据/媒体源)、`display:block`(分区/表格/表单容器等)、`display:inline`(a/em/strong/span 等 23 个)、`display:inline-block`(img/input/button/select/textarea/output/progress/meter);h1-h6/small/s/u/sub/sup/code/kbd/samp/pre/var/cite/dfn/blockquote/dl/dt/dd/figure/figcaption/mark 的默认排版;外加通用 class(.card/.row/.column/.center/.app/.header/.badge/.hidden/.muted/.select-open)。项目本身不内置具体样式,由该根 CSS 指定,页面 `<style>` 可覆盖。
   - **inline 混排**(`src/layout/layout.cpp`):块流中连续的文本 run 与 inline 元素(inline/inline-block,非 absolute/fixed)在同一行按 DOM 顺序水平排列、行满折行;`<p>a <em>e</em> b</p>` 渲染为一行。文本 run 与元素子节点按 DOM 顺序交替构建(元素边界空格合并)。
   - **交互/特殊 tag 的 C++ 行为**:`details/summary` 折叠(点击 summary 切换 `open` 布尔属性,折叠时只渲染第一个 summary,run 注入 ▸/▾ marker);`input[type=checkbox/radio]` 原生勾选框渲染 + 点击切换 checked(radio 按 name 组互斥);`ul/ol` 列表项注入 •/序号 marker;`progress/meter` 绘制轨道+填充;`select` 下拉(原有)、`input/textarea/contenteditable` 编辑(原有)、`img` 固有尺寸(原有)。
   - **tag_id 组件**(ECS 第一步,见 `11-ecs.md`):布局时每元素算一次 `WUI_TAG_*` 类别,绘制/命中热路径用 O(1) 整数比较替代 tag 字符串比较。
7. **DOM API 全量补齐**(`src/dom/dom.cpp` + `src/dom/events.cpp`):查询/导航/结构/classList/innerHTML/outerHTML/表单 value/title/focus/几何(经 render 最近布局树);事件系统(listener side map,目标元素派发,app 事件循环接入 click/mousedown/mouseup/keydown/keyup,无冒泡/捕获)。
8. **lexbor 布尔属性注意**:`lxb_dom_element_get_attribute` 对无值属性(`<details open>`、`<input checked>`)返回 NULL——必须用 `has_attribute` 判存在性(engine 内与 DOM API 均已处理)。

## CSS 支持矩阵

### 已实现(渲染/布局生效)

```
display, width, height, min/max-width/height(vh/vw 可解析), margin/padding/border
(简写+四边,border-block 简写), box-sizing, position(static/relative/absolute/
fixed/sticky;right/bottom 定位;inset 简写), z-index, opacity,
flex 布局(flex-direction, flex-wrap, justify-content, align-items, gap,
flex-grow/flex 简写), grid 布局(grid-template-columns px/fr/auto/repeat(),
grid-column 整行跨越, column/row-gap), clamp()/min()/max() 数学函数, vw/vh 单位,
background-color/background(纯色+线性/径向渐变), background-image 渐变,
border-radius 圆角(背景与边框均沿弧线), box-shadow(GPU:mipmap 模糊近似;
CPU:同心环;inset GPU:收缩盒 mask 反演+offset/blurl:0 实环/CPU:带 offset 的同心内缩环
与硬环四条边), backdrop-filter: blur()(GPU:mipmap 近似,body 走 late pass 保住渐变
与边框,圆角裁剪), text-shadow, rotate(背景/边框真实旋转+渐变轴同转+命中逆旋转,
文本/子树直立),
-webkit-text-stroke, text-decoration(underline/line-through),
color, font-size, font-family(按注册字体), font-weight(bold/≥600 合成粗体),
font 简写, text-align(left/center/right), line-height, letter-spacing,
text-transform, white-space(nowrap), overflow:hidden/auto/scroll(裁剪,
auto/scroll 额外支持滚轮滚动), cursor(记录),
选择器: 标签/#id/.class/组合/后代/子代/相邻兄弟, :hover/:active/:focus,
:first-child/:last-child/:nth-child(An+B), ::before/::after(content+独立样式),
@media(max-width/min-width/prefers-color-scheme/prefers-reduced-motion),
CSS 变量 var(--x, fallback), @keyframes + animation, transition,
img(本地图片 + object-fit cover/contain, 远程/缺失占位)
```

### 已解析但渲染未生效(引擎可计算,绘制暂未消费)

```
filter(saturate/blur/drop-shadow 等), clip-path, background-image url(本地文件/
data:), outline, float/clear, transform-origin(仅默认中心)
```

### 未实现(解析器/引擎未处理)

```
媒体查询其余条件, grid 显式行轨道/命名区域/dense 打包, position:sticky 的
容器底部钳制, backdrop-filter/inset 的 CPU 模糊路径, 多阴影列表(按规范倒序多层绘制)
```

> 布局相关属性的完整清单见 `css-priority.md`;渲染未生效项集中在 `src/render/render.cpp` 的绘制阶段,后续增量实现。

## 已知限制

- **文本换行**:布局阶段按字符数近似文本宽度,实际度量在渲染时完成;长文本可能溢出盒子,`overflow: hidden` 裁剪已支持。
- **文本渲染**:SDL3_ttf 的 `TTF_Text` API(surface engine),自动注册系统 UI 字体(Segoe UI / Segoe UI Emoji / Microsoft YaHei / SimHei / SimSun / Arial)并按注册顺序建立 fallback 链——缺字形时(CJK、emoji)自动回退到后续字体。注意:SDL3_ttf 3.2.2 的 `TTF_SetTextColor` 会使 `TTF_DrawSurfaceText` 静默不绘制,因此文字颜色改为渲染后 tint 混合。
- **lite/minimal 文本渲染**:SDL3_ttf 仅随 `full` 目标;lite/minimal 的文本绘制(stb_font)待实现。
- **inline 混排的近似**:inline 元素与文本 run 同行动态按"估计宽度"折行,实际字形度量在渲染期;行内元素顶部对齐(无基线对齐);inline 元素内的 block 子元素按块处理。
- **列表 marker**:`ul` 用 •、`ol` 用 "N. ",嵌套列表不换 marker(统一 •);`summary` 的 ▸/▾ 在文本 run 内注入(不是独立 ::marker)。
- **checkbox/radio**:无 label 关联(点击 label 不切换);radio 互斥按 name 属性全文档扫描。
- **progress/meter**:meter 的颜色区间(optimum/low/high)不区分,统一 accent 填充。
- **表格**:`table`/`tr`/`td` 默认 block 垂直堆叠,无表格布局(display:table 未实现)。
- **DOM 事件**:三阶段派发(捕获/目标/冒泡;mouseenter/mouseleave/focus/blur 非冒泡);`app.cpp` + `render.cpp` 已接全部类型(click/mousedown/mouseup/dblclick/contextmenu/mousemove/mouseenter/mouseleave/wheel/keydown/keyup/focus/blur/input/change/scroll/resize/load),仅 submit 无表单提交逻辑暂不派发;preventDefault 真实取消默认行为(mousedown 按压链、keydown 编辑键、wheel 滚动)。窗口事件(SHOWN/HIDDEN/RESIZED/MOVED/MINIMIZED/RESTORED/MAXIMIZED/FOCUS/ENTER/LEAVE/CLOSE 可取消)走 per-window 回调;key/select 回调按窗口优先、应用兜底。
- **窗口**:SDL 全量薄包装已实现(is/min/max/focused,最小/大/还原/置顶,像素尺寸,位置/居中,min/max 约束,边框/可调/置顶/透明度/图标/全屏/grab/焦点/parent-modal/显示器/flash) + `set_hit_test`(无边框默认拖拽/缩放) + per-window `display_refresh`。
- **Style**:声明式 API 已实现(set/get/removeProperty,cssText,length/item,getComputedStyle 快照)。
- **Canvas/WebGPU**:`<canvas>` 内在尺寸 300x150 + CPU backing + 2D 核心(fill/stroke/path/transform/ImageData/text fillText/strokeText/measureText/drawImage) + paint 合成;WebGPU 最小闭环 stub(Minimal 不可用)。文本走 SDL3_ttf(Full),lite/minimal 文本返回 -2;drawImage 源为 canvas/ImageData(变换映射 + 最近邻 + alpha 合成),`<img>` 解码源与 arc 弧段/贝塞尔未实现。文档级 `document.destroy` 会同步 drop canvas 上下文与监听器/焦点镜像(元素指针防悬垂)。
- **线程模型**:单线程渲染,`whaleui_app_run` 阻塞;多线程/异步流程留待后续。
- **脏矩形**:动画与滚动走部分重绘(动画只重画动画元素包围盒,滚动只重画露出的 strip);布局树带**子树包围盒**(`bounds`,布局后惰性计算),部分重绘时跳过与脏区不相交的整棵子树(`paint_node` 与选择序号遍历 `sel_seq` 用同一剔除规则,前序序号保持一致;transform/fixed 子树不剔除)。
- **交互**:已支持 `<select>` 下拉(点击展开、选择、回调)、鼠标点击、`:hover/:active/:focus`、**滚轮滚动**(`overflow:auto/scroll` 的固定高度容器与整页——内容超出视口时 html 根自动可滚动;滚轮滚最近的可滚祖先(跳过继承 overflow 的文本 run),否则滚页面;滚轮刻度按 40px、触控板像素增量原样传递;方向经真实滚轮模拟实测:滚轮向下 → 滚动位置增大(看下方);**限位**:滚动位置夹在 `[0, scroll_max]`——顶部无内容时向上滚不动、底部无内容时向下滚不动;**滚动行为可替换**(`whaleui_render_set_scroll_behavior`:默认按像素增量直接累加并夹取,自定义 hook 可实现基于速度/加速度的平滑滚动——平滑滚动曾以内置 target+逐帧缓动实现但因滚轮输入不可靠而回退,留待后期按正确速度模型重做);滚动不重建布局树,绘制与命中测试在渲染期应用滚动偏移;滚动与拖选仅触发重绘(不重布局);box-shadow 随滚动偏移移动,不会残留在原地;**拖动滚动条**时跳过 hover 更新(避免内容滚动使 hover 变化触发每帧重布局,这是拖动卡顿的主因),布局节点在拖动态缓存(树稳定)免去每次 motion 的全树查找)、**文本选择**(抬起时未拖动(阈值 6px)即清空选择,单击与按下时的手部微动都不会产生选择;跨元素选择基于与绘制完全一致的布局树前序序号(同一可见性/裁剪/滚动规则) O(1) 判定;高亮矩形扩展到行盒外再裹 2px 边距)、**文本编辑**(`input[type=text]`/`textarea`/`contenteditable`:光标闪烁、方向键/Home/End/Backspace/Delete/Enter、Ctrl+A 全选;textarea/contenteditable 的光标与高亮跟随布局文本 run 几何,与字形完全对齐)、**输入法**(焦点进入可编辑元素时 `SDL_StartTextInput`,`SDL_EVENT_TEXT_INPUT` 上屏、`SDL_EVENT_TEXT_EDITING` 组合文本显示在光标处)。剪贴板(Ctrl+C/V)未实现。
- **性能**:文本按元素缓存 `TTF_Text` + 栅格化位图(内容/字号/字体变化才重建);**全量重绘也剔除视口外子树**(`bounds` 与视口求交,滚动后的页面大部分内容在屏外,不生成绘制命令);部分重绘只上传 text_layer 的脏区(strip),不整层上传;**滚轮突发命中缓存**(同一鼠标位置不重复全树 hit_test);**滚动是截取而非重绘**:scroll-shift 快路径在 GPU 侧 ping-pong blit 平移上一帧、CPU 侧 memmove 文本层,只重画露出的窄条,滚动条拖动与滚轮一致;**idle 帧完全跳过绘制与 GPU 提交**(仅在布局脏、滚动/拖选、过渡动画运行中、或编辑光标闪烁时绘制);**最小化窗口完全跳过渲染**(`SDL_WINDOW_MINIMIZED` 时不产生 GPU 工作,恢复时强制重绘);box-shadow 视口外提前剔除、层数步进 2px;不透明像素走快速路径;tag 判断走 `tag_id` 组件(ECS 第一步,见 `11-ecs.md`)。实测:21KB/497 节点外部页面滚动帧 6ms(约 166fps),文本重页面 idle 0ms;交互时主要成本是样式级联重布局(大页面约 30ms),后续做规则缓存。动画接入时 `render_frame` 已保证动画期间持续绘制(`anims` 非空即每帧重绘);若将来动画/布局上多线程,`text_cache`、布局树等渲染状态需加同步。
- **省电(电池感知)**:默认 `battery_saver=1`,但**是否生效由系统电源状态决定**——`SDL_GetPowerInfo` 返回 `ON_BATTERY` 时帧率上限 60 且 FSR auto 开启(半分辨率渲染),插电时帧率不限、FSR 按屏幕大小判定。事件循环每 ~2s 轮询一次电源状态(SDL3 3.4 无电源变更事件,轮询开销为一次系统调用),拔插电源无需重启或手动切换;用户仍可用 `WHALEUI_RENDER_BATTERY_SAVER=0` / `MAX_FPS` 显式覆盖。
- **resize/全屏**:窗口尺寸变化后布局重建,所有活跃滚动位置**clamp 到新内容范围**(`scroll_max`),滚动条与内容不会停在越界位置;相关缓存(命中/scroll_max/拖动态)同步失效。
- **lite/minimal**:滚动/选择/编辑/IME 与 full 功能一致;文本度量与命中测试在 full 用 `TTF_Text`,lite/minimal 用 stb_truetype 逐字度量(无 kerning/连字,精度近似)。
- **编辑键简化**:Up/Down 按行首跳转(未保持列位置);未处理 Ctrl 组合除 A 外的快捷键。
- **主题**:内置 7 套主题样式(Fluent / Metro / Material / Classic / Aero / GTK / macOS),各含深浅色变量,全局生效(包括未自定义样式的标签);通过 `whaleui_app_set_theme_style` 或页面 `<select>` 切换。
- **DOM API 剩余未实现**:`whaleui_dom_get_bounding_client_rect` 依赖最近渲染帧(未渲染返回失败);`submit` 已接入(单行输入 Enter 向 `<form>` 派发,无表单导航)。选择器查询已与级联同语义(`whaleui_style_match`:后代/子/相邻/兄弟/`[attr]`/伪类/逗号分支)。focus/blur 已真实化(DOM 镜像 + 非冒泡事件;`:focus` 样式仍跟引擎焦点)。classList 扩展(length/item/replace/toggle-force)、dataset(camelCase<->data-kebab 映射)、append/prepend/before/after(单节点)、insertAdjacentHTML/insertAdjacentText(四位置固定锚点保序)、DocumentFragment(append/insert_before 移入其子节点)已补齐。Selection 以 window 级子集暴露(getSelection 的端点/偏移/选中文本/清除/全选),Range 以 document.createRange() 子集暴露(端点/折叠/to_string/selectNodeContents/deleteContents;元素+字节偏移模型);完整 Range 对象(文本节点级插入、surroundContents)未做。完整迁移蓝图见 `11-ecs.md`。
- **C++ API**:`include/whaleui.hpp`(namespace `whaleui`)是 C++ 门面,`whaleui::App/Window/Document/Element/Event/ComputedStyle/Range` 为 C ABI 的 inline 薄封装(C ABI 是其导出层);所有权与语义同 C 契约,`tests/test_cpp.cpp` 覆盖。
- **Web 平台 API**(`src/core/clipboard.cpp` / `src/core/timers.cpp` / `src/core/extras.cpp` / `src/dom/data_transfer.cpp`):clipboard(SDL 系统剪贴板 + 排队回调 + copy/cut/paste DOM 事件与 `set_clipboard_text`)、setTimeout/setInterval(定时器保持循环唤醒,从到期时刻重排)、requestAnimationFrame(帧后回调)、visualViewport/devicePixelRatio(无捏合缩放)、Web Audio 占位(全部不支持)、gamepad(SDL3 真实实现:稠密索引 + 轴/按钮位 + 震动 + 连接事件)、拖放(DataTransfer/File + OS 拖入 SDL_EVENT_DROP_* + 页内 draggable 会话)。事件载荷含修饰键(`WHALEUI_MOD_*`)、`key`/`code`、repeat、detail、movementX/Y、relatedTarget、clipboard 文本、DataTransfer;`whaleui_dom_dispatch_input[_nonbubble/_clipboard]` 与 `whaleui_dom_dispatch_drag` 是引擎内部全载荷派发入口。见 `doc/external/09-api-web.md` 与 `tests/test_web.cpp`。

## 运行方式

```bash
xmake run demo          # 交互 demo:T 切换深浅色,ESC 退出
xmake run test_render   # 像素级渲染验证(需 SDL_GPU 后端;无 GPU 时跳过绘制部分)
```
