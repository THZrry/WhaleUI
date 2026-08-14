# 实现状态(第三步)

记录第三步「内部实现」的完成情况与已知限制。文档描述的是**已实现**的行为,与代码一致。

## 总体进度

| 子步骤 | 内容 | 状态 |
|--------|------|------|
| 1 | DOM 功能(接入 lexbor) | ✅ |
| 2 | 标签功能(tag→div 适配) | ✅ 基础(computed style 按 tag 预设) |
| 3 | 样式处理 | ✅ 解析/匹配/级联/var |
| 4 | 渲染实现 | ✅ CPU 绘制 + SDL_GPU blit,软件回退 |
| 5 | 流程处理 | ✅ 事件循环 + 帧率上限 |
| 6 | 外围功能(应用/窗口 API) | ✅ |
| 7 | 样式(系统风格转换) | 🟡 默认样式 + 深浅色;7 套系统风格未逐一转换 |
| 8 | 测试 | ✅ 8 个测试全部通过 |

## 架构修正(相对第二步文档)

1. **布局不由 lexbor 计算**。lexbor 提供 HTML/CSS **解析**与选择器,没有可用的 CSS layout 引擎;`src/layout/` 自研盒模型 + 基础 flex(第二步文档中的假设不成立,已按实际修正)。
2. **CSS 解析自研**,未接 lexbor 的 CSS 语法树(接口复杂且本项目只需子集);解析器位于 `src/style/css.cpp`。
3. **渲染统一走 SDL_GPU**:CPU framebuffer 上传离屏纹理后 blit 到 swapchain,底层 D3D12 / Vulkan 由 SDL 自动选择(官方 mingw 预编译包只编译了这两个 backend)。注意 SDL3 3.4.x 起 `SDL_CreateGPUDevice` 必须显式声明 shader 格式(`SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV`),传 0 会被 D3D12/Vulkan 后端直接拒绝。**Vulkan 已验证**(`WHALEUI_GPU=vulkan`,test_render 像素断言通过):HLSL 经 DXC 交叉编译为 SPIR-V(着色器内用 `[[vk::binding()]]` 把资源钉到 SDL 期望的 set0=采样纹理 / set1=读写 storage / set2=uniform),FSR 使用预编译 SPIR-V;注意本机 DXC 1.9.0.5402 不识别 `-fvk-bind` 命令行选项,所以绑定信息写在 HLSL attribute 里而非编译参数。**Metal 未实现/未验证**(非 Windows 的 shader 编译路径返回 nullptr 并打印提示)。**适配器选择**:`WHALEUI_GPU_LOWPOWER=1` 设置 `SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN` 偏向核显;默认不设——台式机通常该走独显,且 Windows 的按应用图形设置(设置 > 系统 > 显示 > 图形)优先级更高,让系统决定即可。
4. **SDL3 采用官方预编译包**(`3rdparty/sdl3/` + `3rdparty/sdl3_ttf/`),因为 `SDL3_ttf.dll` 运行时依赖 `SDL3.dll`。

## CSS 支持矩阵

### 已实现(渲染/布局生效)

```
display, width, height, min/max-width, margin/padding/border(简写+四边,
border 简写含颜色提取), box-sizing, position, top/left, z-index, opacity,
flex 布局(flex-direction, justify-content, gap, flex-grow),
background-color/background(纯色), border-radius 圆角(背景与边框均沿弧线),
color, font-size, font-family(按注册字体), font-weight(bold/≥600 合成粗体),
text-align(left/center/right), line-height(近似), overflow:hidden/auto/scroll(裁剪,
auto/scroll 额外支持滚轮滚动), cursor(记录)
```

### 已解析但渲染未生效(引擎可计算,绘制暂未消费)

```
box-shadow, background-image, transform, transition, animation(@keyframes 已解析)
```

### 未实现(解析器/引擎未处理)

```
grid 布局, float/clear, text-overflow, flex-wrap(子项换行),
backdrop-filter, clip-path, 媒体查询的其余条件
```

> 布局相关属性的完整清单见 `README-css.md`;渲染未生效项集中在 `src/render/render.cpp` 的绘制阶段,后续增量实现。

## 已知限制

- **文本换行**:布局阶段按字符数近似文本宽度,实际度量在渲染时完成;长文本可能溢出盒子,`overflow: hidden` 裁剪已支持。
- **文本渲染**:SDL3_ttf 的 `TTF_Text` API(surface engine),自动注册系统 UI 字体(Segoe UI / Segoe UI Emoji / Microsoft YaHei / SimHei / SimSun / Arial)并按注册顺序建立 fallback 链——缺字形时(CJK、emoji)自动回退到后续字体。注意:SDL3_ttf 3.2.2 的 `TTF_SetTextColor` 会使 `TTF_DrawSurfaceText` 静默不绘制,因此文字颜色改为渲染后 tint 混合。
- **lite/minimal 文本渲染**:SDL3_ttf 仅随 `full` 目标;lite/minimal 的文本绘制(stb_font)待实现。
- **线程模型**:单线程渲染,`whaleui_app_run` 阻塞;多线程/异步流程留待后续。
- **脏矩形**:动画与滚动走部分重绘(动画只重画动画元素包围盒,滚动只重画露出的 strip);布局树带**子树包围盒**(`bounds`,布局后惰性计算),部分重绘时跳过与脏区不相交的整棵子树(`paint_node` 与选择序号遍历 `sel_seq` 用同一剔除规则,前序序号保持一致;transform/fixed 子树不剔除)。
- **交互**:已支持 `<select>` 下拉(点击展开、选择、回调)、鼠标点击、`:hover/:active/:focus`、**滚轮滚动**(`overflow:auto/scroll` 的固定高度容器与整页——内容超出视口时 html 根自动可滚动;滚轮滚最近的可滚祖先(跳过继承 overflow 的文本 run),否则滚页面;滚轮刻度按 40px、触控板像素增量原样传递;方向经真实滚轮模拟实测:滚轮向下 → 滚动位置增大(看下方);**限位**:滚动位置夹在 `[0, scroll_max]`——顶部无内容时向上滚不动、底部无内容时向下滚不动;**滚动行为可替换**(`whaleui_render_set_scroll_behavior`:默认按像素增量直接累加并夹取,自定义 hook 可实现基于速度/加速度的平滑滚动——平滑滚动曾以内置 target+逐帧缓动实现但因滚轮输入不可靠而回退,留待后期按正确速度模型重做);滚动不重建布局树,绘制与命中测试在渲染期应用滚动偏移;滚动与拖选仅触发重绘(不重布局);box-shadow 随滚动偏移移动,不会残留在原地;**拖动滚动条**时跳过 hover 更新(避免内容滚动使 hover 变化触发每帧重布局,这是拖动卡顿的主因),布局节点在拖动态缓存(树稳定)免去每次 motion 的全树查找)、**文本选择**(抬起时未拖动(阈值 6px)即清空选择,单击与按下时的手部微动都不会产生选择;跨元素选择基于与绘制完全一致的布局树前序序号(同一可见性/裁剪/滚动规则) O(1) 判定;高亮矩形扩展到行盒外再裹 2px 边距)、**文本编辑**(`input[type=text]`/`textarea`/`contenteditable`:光标闪烁、方向键/Home/End/Backspace/Delete/Enter、Ctrl+A 全选;textarea/contenteditable 的光标与高亮跟随布局文本 run 几何,与字形完全对齐)、**输入法**(焦点进入可编辑元素时 `SDL_StartTextInput`,`SDL_EVENT_TEXT_INPUT` 上屏、`SDL_EVENT_TEXT_EDITING` 组合文本显示在光标处)。剪贴板(Ctrl+C/V)未实现。
- **性能**:文本按元素缓存 `TTF_Text` + 栅格化位图(内容/字号/字体变化才重建);**全量重绘也剔除视口外子树**(`bounds` 与视口求交,滚动后的页面大部分内容在屏外,不生成绘制命令);部分重绘只上传 text_layer 的脏区(strip),不整层上传;**滚轮突发命中缓存**(同一鼠标位置不重复全树 hit_test);**滚动是截取而非重绘**:scroll-shift 快路径在 GPU 侧 ping-pong blit 平移上一帧、CPU 侧 memmove 文本层,只重画露出的窄条,滚动条拖动与滚轮一致;**idle 帧完全跳过绘制与 GPU 提交**(仅在布局脏、滚动/拖选、过渡动画运行中、或编辑光标闪烁时绘制);**最小化窗口完全跳过渲染**(`SDL_WINDOW_MINIMIZED` 时不产生 GPU 工作,恢复时强制重绘);box-shadow 视口外提前剔除、层数步进 2px;不透明像素走快速路径。实测:21KB/497 节点外部页面滚动帧 6ms(约 166fps),文本重页面 idle 0ms;交互时主要成本是样式级联重布局(大页面约 30ms),后续做规则缓存。动画接入时 `render_frame` 已保证动画期间持续绘制(`anims` 非空即每帧重绘);若将来动画/布局上多线程,`text_cache`、布局树等渲染状态需加同步。
- **省电(电池感知)**:默认 `battery_saver=1`,但**是否生效由系统电源状态决定**——`SDL_GetPowerInfo` 返回 `ON_BATTERY` 时帧率上限 60 且 FSR auto 开启(半分辨率渲染),插电时帧率不限、FSR 按屏幕大小判定。事件循环每 ~2s 轮询一次电源状态(SDL3 3.4 无电源变更事件,轮询开销为一次系统调用),拔插电源无需重启或手动切换;用户仍可用 `WHALEUI_RENDER_BATTERY_SAVER=0` / `MAX_FPS` 显式覆盖。
- **resize/全屏**:窗口尺寸变化后布局重建,所有活跃滚动位置**clamp 到新内容范围**(`scroll_max`),滚动条与内容不会停在越界位置;相关缓存(命中/scroll_max/拖动态)同步失效。
- **lite/minimal**:滚动/选择/编辑/IME 与 full 功能一致;文本度量与命中测试在 full 用 `TTF_Text`,lite/minimal 用 stb_truetype 逐字度量(无 kerning/连字,精度近似)。
- **编辑键简化**:Up/Down 按行首跳转(未保持列位置);未处理 Ctrl 组合除 A 外的快捷键。
- **主题**:内置 7 套主题样式(Fluent / Metro / Material / Classic / Aero / GTK / macOS),各含深浅色变量,全局生效(包括未自定义样式的标签);通过 `whaleui_app_set_theme_style` 或页面 `<select>` 切换。

## 运行方式

```bash
xmake run demo          # 交互 demo:T 切换深浅色,ESC 退出
xmake run test_render   # 像素级渲染验证(需 SDL_GPU 后端;无 GPU 时跳过绘制部分)
```
