# 渐进渲染 / 异步分块布局设计（方向确认，实施按里程碑拆）

> 状态：方向已确认（2026 维护轮）。实现拆里程碑，每步独立可测可回滚。
> 目标：点击展开 20k 节点 `<details>` 组不再同步卡 ~3.7s——**布局完成前就允许渲染**，
> 视口先出、滚动时后续内容逐步就绪。

## 0. 为什么现有模型不行

- relayout 一次性 build（新子树 style 级联 ~1.7s）+ **全树 box pass（写 34k 节点几何 ~2s）**。
- box pass 是整树写 → 任何"渲染线程与 worker 并发读同一棵树"的方案（读写锁）在 box 期间
  都必须让读方停（写覆盖全树时不存在一致读）→ 纯 busy/锁方案 = 渲染停 3.7s，与同步无异。
- 双树整树替换：拷贝 ~34k 节点可接受（~30ms），但"旧树继续交互、新树完成后切换"意味着
  用户在展开后 3.7s 内看到的是**未展开的旧视图**（无渐进感），且切换瞬间仍需一次性 paint。

## 1. 目标形态（浏览器同款渐进）

1. **点击 summary 立即**：折叠 summary 就地切换成"展开中"指示；事件循环继续（滚动/悬停
   旧内容可用）。
2. worker 在**原树**上以追加方式扩展 details 子树：逐块（如每块 256 行 li）构建节点并
   挂到 ul 链尾。**已链入部分不再改动** → 渲染线程读"已链入且视口内"的部分是安全的
   （单写者、先构造完整节点再单指针链入、链尾读者最终一致），无需整树锁。
3. 每块就绪 → 局部 box（该块行的 y 定位）+ **后续兄弟整段 y 平移**（diff：delta 平移，
   比全树 box 快几个量级；块级流场景 y += delta 正确）→ 标记该区域脏。
4. 渲染每帧 diff：只 repaint 脏区域（新出现的行/几何），视口内新增内容立即可见；滚动到
   未就绪区域时显示占位/等待该块（块就绪后自动补画）。
5. box pass 与级联的**常数优化**继续有效（style cache、文本度量缓存、节点位）。

## 2. 并发安全规则（追加式树的只读约定）

- **单写者**（layout worker）追加；渲染线程只读已链部分。
- 节点**先完整构造**（style/text/几何初值），最后一步才链入父 `next`（单指针写）。
- 读方遍历到链尾（next == nullptr）即停——本帧看不到未链入节点，下一帧可见（最终一致）。
- `arena`（deque）只 push_back，不移动已有节点：读方持有的节点指针永不失效。
- `by_el` / `style_cache`：layout 期写、几何查询读 → 短临界区 mutex，或几何查询仅在
  "非 worker 期间"（同步路径）用——按访问函数逐个定（12-state-mechanism.md 检查清单）。
- **绝不**在 worker 期间修改已链入节点的几何/样式（否则读者看到撕裂态）。任何需要改写
  已链节点的工作（如 hover style-only、动画插值）必须等 worker 完成或走同步路径。

## 3. 里程碑（每步独立提交+测试+验证）

- **M1 并发安全树基础**（**已验证，temp/stream_probe.cpp**）：写线程 2s 追加 17,317
  节点（先完整构造后单指针链入），读线程 15,278 次遍历（链尾即止）：无崩溃无撕裂。
  追加式约定成立（arena 只 push_back 不移动、节点先构造后链入、读方链尾即止）。
  待办：by_el / style_cache 的读写访问点逐个定锁或调度约束（几何查询与 layout 分时）。
- **M2 分块 build + 局部 box + y 平移**（预研结论）：单 li 的几何可直接复用
  `Builder::layout(li_node, ...)`（run 分支 `n->border.y = *cursor_y` + cursor 累加，
  行高 `line_height_px`）——**不必手算行高**。做法：首块先建 ul 节点并链入 details
  （y = summary 底），对块内每个 li：`build(li_el, ul_node)` 建子树 → 链入 ul →
  `layout(li, ...)` 独立定位（局部 cursor）。块完成 delta_y 平移 details 后续兄弟
  子树。布局树最终高度 = box 估高，与同步 relayout 一致（同步路径同样不跑
  fix_run_heights，仅首帧/全量后跑）。待验证：layout() 元素分支对 li 的定位细节
  （inline/块级分支、cursor 语义）。
  **实现发现（2026 轮，temp/append_probe/append_min）**：在**已布局树**上对单个新
  `<li>` 调 `Builder::build(li_el, list_node)` 崩溃（build 假定"整树构建"上下文——
  崩在 build 内、子元素构建前；anim=null 与 anim 路径同样崩；用 DOM create_element
  新建的 li）。下一轮先诊断 build 单元素复用的崩溃点（create_element 元素与解析元素
  的差异 / build 对旧 parent 节点的假设 / new_node + by_el 在已建树上的行为），再做
  whaleui_layout_append_rows。期间 DOM 尾部 append 仍走全 relayout（结构 dirty 路径）。
- **M3 diff paint**：脏区 = 每块新增行视口交集；帧循环每块就绪触发一次局部 repaint
  （复用现有 partial strip）。滚动到未就绪区显示占位。验收：点击后 <100ms 视口出现首块，
  滚动流畅，最终画面与同步一致。
- **M4 交互期事件语义**：展开进行中 hover/click/滚动旧内容（已链部分）可用；对未链区
  域的事件排队到块就绪。验收：展开中滚动不冻结，hover 旧行正常。

## 4. 明确不做（YAGNI，记录）

- 不做 JS 引擎、不做虚拟滚动复用（树仍全量持有，只是分块就绪）。
- 不做 layout 的任意点中断（只做"块边界"分块，块内仍同步）。
- flex/grid 容器内的大子树展开首版回退同步（块级流先覆盖）。

## 5. 长期原则引用

- 本设计与 doc/internal/12-state-mechanism.md 一致：统一底层机制（树增长/脏区/diff paint
  对一切大子树变更生效，不特判 details）、不做元素特判、检查清单逐条过。
