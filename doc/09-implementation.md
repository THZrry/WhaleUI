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
3. **渲染统一走 SDL_GPU**:CPU framebuffer 上传离屏纹理后 blit 到 swapchain,底层 D3D12 / Vulkan 由 SDL 自动选择(官方 mingw 预编译包只编译了这两个 backend)。注意 SDL3 3.4.x 起 `SDL_CreateGPUDevice` 必须显式声明 shader 格式(`SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV`),传 0 会被 D3D12/Vulkan 后端直接拒绝。自定义 shader 中间表示(SPIR-V 等)留待后续。
4. **SDL3 采用官方预编译包**(`3rdparty/sdl3/` + `3rdparty/sdl3_ttf/`),因为 `SDL3_ttf.dll` 运行时依赖 `SDL3.dll`。

## CSS 支持矩阵

### 已实现(渲染/布局生效)

```
display, width, height, min/max-width, margin/padding/border(简写+四边,
border 简写含颜色提取), box-sizing, position, top/left, z-index, opacity,
flex 布局(flex-direction, justify-content, gap, flex-grow),
background-color/background(纯色), border-radius 圆角(背景与边框均沿弧线),
color, font-size, font-family(按注册字体), font-weight(bold/≥600 合成粗体),
text-align(left/center/right), line-height(近似), overflow:hidden(裁剪),
cursor(记录)
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

- **文本换行**:布局阶段按字符数近似文本宽度,实际度量在渲染时完成;长文本可能溢出盒子,`overflow` 裁剪尚未实现。
- **文本渲染**:SDL3_ttf 的 `TTF_Text` API(surface engine),自动注册系统 UI 字体(Segoe UI / Segoe UI Emoji / Microsoft YaHei / SimHei / SimSun / Arial)并按注册顺序建立 fallback 链——缺字形时(CJK、emoji)自动回退到后续字体。注意:SDL3_ttf 3.2.2 的 `TTF_SetTextColor` 会使 `TTF_DrawSurfaceText` 静默不绘制,因此文字颜色改为渲染后 tint 混合。
- **lite/minimal 文本渲染**:SDL3_ttf 仅随 `full` 目标;lite/minimal 的文本绘制(stb_font)待实现。
- **font-weight 粗体**:未做字形合成,`strong`/`b` 与普通文本同字形。
- **线程模型**:单线程渲染,`whaleui_app_run` 阻塞;多线程/异步流程留待后续。
- **脏矩形**:当前整帧重绘;脏矩形/遮挡/缓存是下一步性能优化点。
- **主题**:内置默认样式 + 深浅色两套变量;Windows Classic / Aero / Metro / Fluent / GTK / macOS / Material 的系统风格转换尚未逐一落地(可基于 `--*` 变量表扩展)。

## 运行方式

```bash
xmake run demo          # 交互 demo:T 切换深浅色,ESC 退出
xmake run test_render   # 像素级渲染验证(需 SDL_GPU 后端;无 GPU 时跳过绘制部分)
```
