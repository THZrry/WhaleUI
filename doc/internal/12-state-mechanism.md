# 交互状态机制与底层优化原则（长期遵循）

> 本文档是架构原则的扩展，与 `temp/ARCHITECTURE-DEFECTS.md` 第 0 节并列为**必须一直遵守**的
> 评判标准。任何交互/渲染/性能改动先对照本文档，违反处需在 commit message 里说明理由。

## 0. 架构总原则（ARCHITECTURE-DEFECTS.md 第 0 节，引用）

1. **底层只做通用机制，不做元素特判**：分层绘制按 z-index + 文档顺序统一处理；cursor 由
   命中元素查样式统一决定；hover/滚动/transition 是底层能力，差异由页面 CSS 表达。
2. **行为差异用 C++ 配置/回调补全，样式差异用 CSS（含 UA 默认样式）补全**，避免在渲染/
   布局/事件主路径堆 if-else。
3. 能下沉到通用机制的不做局部特调。

## 1. 交互状态统一机制（本文件核心新增）

**问题来源**：`:hover`/`:active`/`:focus` 等伪类状态各自修改会逐个打补丁（hover 修好
active 又出问题），优化只覆盖被改的那个状态，其余状态仍走旧的全量重建路径。

**原则：所有伪类交互状态走同一个状态机，差异只在"哪些伪类参与匹配"。**

- **状态切换检测统一**：hover / focus / pressed（对应 `:hover` / `:focus-visible` /
  `:active`）的**进入与离开**都由状态机记录受影响元素（old + new），进入一个统一的
  "待处理状态元素集合"。引擎不针对 hover 写一套、针对 focus 另写一套。
- **树优化决策统一**：对集合里每个元素，统一执行「重算新级联 → 布局键 diff」：
  - 布局键未变（color/transform/background/underline 等 paint 属性）→ **style-only
    relayout**（重建子树样式 + 拷贝几何，跳过全树 box pass）+ partial repaint；
  - 布局键变化 → 单子树增量 relayout + box pass；
  - 与 DOM 变更叠加 → 并入 DOM-dirty 增量路径。
- **轻量差异化**：hover/focus/active 的差异只是 `whaleui_style_state` 里
  `hover/focus/pressed` 字段谁被更新，选择器匹配照常按 `:hover`/`:focus`/`:active`
  各自工作——不复制处理逻辑、不产生一致性问题、不膨胀体积。
- **repaint 统一**：状态变化帧统一走 partial（状态元素 box strip）；idle 判定统一把
  "有待处理状态元素"视为需要一帧。

**落地位置**：`whaleui_render.state_pending` + render_frame 的状态处理块（布局键 diff →
`whaleui_layout_relayout_style` 或 DOM-dirty）。hover 已接入；focus/pressed 必须接入同一
入口（编辑控件 caret/选区、select popup 等"有自身行为"的路径可保留原同步路径，但它们
的状态样式应用仍走统一机制）。

## 2. 树/几何优化原则

- **重建范围最小化**：style-only（重建子树、几何不变，跳过全树 box）→ 单子树增量
  relayout → 全树重建，按需取最小档。
- **paint bounds 与新子树一致**：任何重建子树的节点 bounds 是 0，paint cull 会裁掉它们。
  重建后必须（a）重算该子树 bounds（祖先几何未变则祖先 bounds 仍有效，不必整树重算），
  或（b）paint cull 豁免该元素。**禁止**两者都不做（= hover/滚动后元素消失的根因）或
  无脑整树失效（= 每 hover 40ms 的浪费）。
- **paint 遍历零 map 查找**：树遍历命中每个节点都做的属性判定（transform/position/z）
  必须在 build 时算成节点位（`xf/fx/sk/hz`），paint 读位。34k 节点下每帧全树遍历的
  `sget` 是 ~30ms 级成本。
- 展开/收起等**大子树重建**仍会同步阻塞数秒（22k 节点 ~3.7s：级联 1.7s + 文本首次度量
  1.1s + box 0.9s，都是新内容的固有成本）——正解是**异步分帧**（relayout 上 worker，
  帧循环不阻塞），不是继续压缩单帧常数。

## 3. 体积与流程原则

- 每个交互状态一套逻辑 = 重复代码膨胀；统一机制一份逻辑覆盖全部状态 = 不膨胀。
- 死代码即删（编译器 -Wunused 扫描）、release strip（demo 5.4MB → 2.85MB 实测）、
  大 shader/资源数组只保留在用版本。
- 性能改动必须留探针数据（temp/perf_wpt.cpp 模式），不得凭感觉。

## 4. 检查清单（改交互/渲染代码前）

- [ ] 这个行为是某个 tag 特判，还是能由 CSS/通用机制表达？
- [ ] hover/focus/active 是否都经过统一状态机？还是又开了一条新路径？
- [ ] 状态/几何变化是否产生 bounds=0 的新节点？bounds 处理选了什么（子树重算/豁免）？
- [ ] paint 每节点路径是否新增了 style map 查找？
- [ ] 重建范围是否最小（style-only < 单子树 < 全树）？
- [ ] 大文档交互是否仍同步阻塞？是否该走异步分帧？
- [ ] 测试是否覆盖该状态的像素/几何断言（如 test_scroll hover 断言）？
