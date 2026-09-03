# 15. WhaleUI 引擎架构总览

> 状态：维护期总览（2026-06，基于当前实现）。本文讲**宏观架构**、
> **自研与第三方分工耦合**、**已落地的优化技术**三部分；单循环/单步骤
> 内部逐指令级优化不在此展开（见 12/13/14 与代码注释）。
> ECS 化的历史设计见 11-ecs.md（本文按当前 AoS 布局树描述实际实现）。

## 一、宏观架构

### 1.1 分层与数据流

```
HTML/CSS 文本
   │  whaleui_window_load_html / load_file
   ▼
[dom]  lexbor 解析 → 标准 DOM 树（元素/文本/属性）
   │  自研封装: 事件 dispatch、变更脏标记(dirty 队列/struct 标记)
   ▼
[style]  CSS 收集(文档 <style>/<link> + 主题默认样式) → 规则表
         媒体查询过滤(@media 宽/主题/减弱动效) → 级联(规则索引)
   ▼
[layout] Builder(逐元素 build: 级联+继承+run 文本) → 布局树
         布局: 块/行内/换行/估高(文本度量经 renderer hook)
   │  追加式: arena 只增、节点先构造后链入
   ▼
[render] 帧循环(状态机: 交互/动画/DOM/滚动) → paint
         CPU: text_layer 光栅(字形) + pixels; GPU(SDL_GPU/D3D12):
         上传/合成; FSR 可选上采样
   ▼
[animate] @keyframes/transition: 每帧 tick → paint-only 或布局键变更
```

公开面：单一 `include/whaleui.h`（C API，不透明句柄）。模块：
`src/dom`（lexbor 封装 + DOM 事件）、`src/style`（css 解析/级联/匹配）、
`src/layout`（布局树/Builder/relayout/追加式存储）、`src/render`
（帧调度/paint 分文件：text/color/control/fsr/gpu）、`src/core`
（app/window 生命周期 + 线程）、`src/animate`、`src/font`、`src/fs`、
`src/platform`（SDL 后端/主题）。

### 1.2 线程模型（多线程分工）

- **主线程**：SDL 事件轮询（PollEvent/WaitEventTimeout）→ 事件仅入队
  （`input_queue`）+ 设 `frame_request`；空闲按 alive/frames_alive 决定
  park 或续帧。
- **渲染 worker 线程**（全局单 worker，`app->render_thread`，处理所有
  窗口）：消费 `input_queue`（process_event：点击/滚动/hover/键盘 →
  状态变更 + DOM 变更标记）→ 渲染帧（状态 relayout → DOM 增量 →
  paint）。主线程只投递不碰渲染态，慢帧不阻塞 UI。
- **后台 fill 线程**（details 渐进展开，M2）：每次持 `tree_mx` 追加一批
  行（append_rows），渲染帧空闲时全速、交互帧让位；批间短睡让调度。
- **同步原语**：
  - `render_lock`（app）：主线程投递 vs worker 消费（输入与渲染串行）。
  - `tree_mx`（render 成员，`std::recursive_mutex`）：布局树访问串行化
    ——渲染帧**整帧持锁**（state relayout/DOM relayout/paint 全在锁内，
    保证与 fill 批不交错），事件处理器（hit-test 读树）入口也持锁，
    set_css/展开点击写共享态持锁。
  - `frame_request / frame_cv`：worker 唤醒乒乓；worker 渲染后
    `alive`（动画/fill 活跃）会**自续 frame_request**——OS 模态循环
    卡住主线程时动画仍按 vsync 自驱。
  - SDL 事件 watch（模态 resize/拖动）：零锁写 atomic 尺寸槽 +
    notify（不在 watch 内拿 render_lock——避免与 SDL 内部锁交叉）。

### 1.3 渲染表示（DOM/布局/状态）

- **DOM**：lexbor 标准树（外部所有权）；引擎 DOM 层维护脏集合。
- **布局树**：`whaleui_layout_tree`——arena（deque）稳定存储 + `by_el`
  元素映射（O(1) 查找）+ 文本 run 节点；AoS 结构（style map 随节点）。
  交互状态（hover/focus/pressed）不存树内，由统一状态机（12）驱动。
- **paint**：CPU text_layer（字形 alpha）+ pixels；GPU 目标合成；
  部分重绘（dirty strip）与 bounds 缓存（子树包围盒）裁剪视口外。
- 样式表深拷贝进 render；媒体过滤缓存；动画 keyframes 独立。

## 二、自研与第三方分工与耦合

| 第三方 | 承担 | 自研 | 承担 |
|---|---|---|---|
| lexbor | HTML 解析、DOM 树/遍历/属性 | dom 层 | DOM 封装、事件、脏标记、文档装载 |
| SDL3 | 窗口/输入/事件、GPU（SDL_GPU/D3D12 后端） | style | CSS 解析、级联、选择器匹配、媒体查询、规则索引 |
| SDL3_ttf | 字体装载/字形度量/光栅 | layout | 布局引擎（块/行内/换行/估高/增量 relayout） |
| SDL_image / stb（Lite） | 图片解码资源 | render | 帧调度、paint、text_layer、GPU 合成、FSR（自带 shader） |
| utf8proc | Unicode 处理（宽度等） | animate | @keyframes/transition 引擎 |
| | | core/platform | 线程模型、主题、平台适配 |

**耦合点（自研 ↔ 第三方）**：
- lexbor DOM ↔ 自研：`tag_id`/选择器匹配直接读 lexbor 元素（本地名/
  属性/类）；元素句柄跨层传递（`lxb_dom_element*` 与 C API 不透明指针
  互转）；**所有权在 lexbor**（引擎不释放 DOM 元素，只标记脏）。
- 文本度量：layout 估宽走"renderer 安装的 metric hook"（真字形度量），
  纯 layout 测试走内置估算——hook 是全局上下文（`g_metric_render`），
  只在树锁内读写。
- SDL_GPU：paint 产出的 text_layer/pixels 上传为纹理合成；分辨率/
  FSR 决定 fb 尺寸与坐标换算（`fb_coords`）。
- CSS：默认样式表是自研主题 CSS（`theme_default_css`）与页面样式合并。

## 三、优化技术（已落地，宏观视角）

1. **追加式布局树（并发安全的读基元）**：arena 只 push_back 不移动；
   节点**先完整构造后单指针链入**；读方链尾即止——后台 fill 追加与
   渲染帧 paint 并发无撕裂（M1 验证）。by_el/style_cache 等共享表
   全在 `tree_mx` 下访问。
2. **DOM 变更锚点增量（属性/结构分流）**：统一脏标记（普通 =
   属性/style/class 编辑；`mark_dirty_struct` = 子树形状变化）：
   - 属性编辑 → 布局键（kLayoutKeys）diff：未变 → style-only
     relayout_style + 局部 bounds + partial repaint；变了 → 全 relayout。
   - 结构编辑 → 全 relayout + wide repaint；**列表尾部 append** 例外：
     `append_rows` 只建新行（逐行独立布局续 cursor）+ 平移后续。
3. **渐进渲染（M1-M4，13）**：`<details>` 展开首帧只建视口行
   （`pending_budget`），剩余由后台 fill 线程分批追加、`fill_dirty`
   节流唤醒 repaint；交互/动画帧让位（补块只在空闲大步）。事件在
   worker 与 fill 解耦，未渲染完也完全可交互。
4. **交互状态统一机制（12）**：hover/focus/active 同一状态机，差异仅
   style_state 字段；状态 relayout 先比 cascade（无规则命中的大容器
   跳过重建，防 hover 移走整树重建秒级卡顿）。
5. **样式缓存与索引**：compute 结果按 (元素, hover, focus, pressed)
   缓存（style_cache，DOM/规则变更局部失效）；规则 tag/class 分桶索引
   只匹配候选规则；伪元素规则索引带锁。
6. **文本度量多级缓存**：整串宽缓存 + 逐字形 advance 缓存（ASCII
   数组/非 ASCII 表），layout 与 paint 同源。
7. **部分重绘（dirty-rect/strip）**：hover/焦点/动画/补齐用 strip 只
   重绘变化盒（视口裁剪 + 子树 bounds 缓存）；几何全变帧强制全量。
8. **字体惰性**：按 family 缓存 TTF_Font、尺寸/样式按需应用；CJK
   回退链按缺失字形懒开；ascii 快路径。
9. **resize 合并与免全量**：resize 事件 80ms coalesce；样式未变时
   **geometry-only 重排**（保留树只重算几何，不做级联/重建）；CSS
   规则未变跳过 set_css（保 style_cache）。
10. **font-scale 原地缩放（±）**：改 font-size 值 + 几何重排，免整树
    级联重建（4.9s → 0.4s）。
11. **动画帧成本控制**：paint-only 动画（opacity/transform/color）tick
    直贴树不 relayout；布局型动画（宽/高）只重建动画元素子树 + 一次
    box pass（batch relayout_multi）；opacity 链复用。
12. **worker 帧乒乓与模态穿透**：frame_request/alive 驱动；动画自续；
    SDL watch 在 OS 模态 resize/拖动期间转发尺寸/唤醒——拖窗/缩放
    全程渲染与输入不断。
13. **并发安全约定汇总**：树锁（recursive）+ 帧整持锁 + fill 批间
    让位；主线程零渲染态访问；watch 零锁原子槽；fill 退出 join 回收。

## 四、备注（暂缓项）

- 布局行成本精确分段（build 结构 ~150us/行、layout ~0.07ms/行的真实
  构成）与后续提速：见 14——已实测 run style 拷贝仅 3us（1.5%），
  StyleHandle 重构叫停。**当前性能已达标，此项后置**，不再主动展开。
- 视口驱动补块（滚到哪补哪）为 M2 后续可选项（现顺序补 4.5s 内全
  量，后台不阻塞交互）。
