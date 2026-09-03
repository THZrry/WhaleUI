# 14. 布局提速（style 共享重构）规划与风险预案

状态：规划（下一轮实施）。目标：行 build 成本 0.2ms -> 0.08ms 以下，
补齐 4.5s -> ~2s，整树 rebuild（媒体跨界 resize / 全量 DOM 变更）同步受益。

## 0. 已测数据（micro 插桩，临时已还原）

- style_compute 单次 ~25us（specificity 0.2us + decls 解析 6us +
  selector match + out 构建 ~19us）——**已近下限**（tag/class 索引齐备）。
- 行 build 0.2ms 的大头是 **style map 深拷贝 + 继承 merge（build 层）**：
  每元素 n->style = compute 返回值（~50 键 map 值拷贝 ~20-25us）
  + cache 命中 `n->style = sit->second`（同价拷贝）
  + 父链继承键 merge 插入
  每行 li + a 两次 -> ~0.15ms/行的拷贝墙。
- 受影响路径：fill 补齐（行 build）、± 之后的整树 rebuild（现已走
  scale_fonts 免 rebuild）、DOM 结构变更全 relayout、resize 媒体跨界。

## 1. 需改文件与函数（初步统计）

| 文件 | 函数/位置 | 改动 |
|---|---|---|
| src/style/style.h | `WhaleUIComputedStyle`（typedef map） | 改共享句柄类型（shared_ptr<const map> + COW） |
| src/style/style.h | `whaleui_style_compute` 声明 | 返回共享值 |
| src/style/style.cpp | `whaleui_style_compute` | 返回共享；末尾 var/shorthand 展开（改 out）需 COW detach 隔离 |
| src/style/style.cpp | `expand_shorthands`/`resolve_var` | 只改调用方 detach 后的副本 |
| src/layout/layout.cpp | `Builder::build`（~1300-1810） | n->style 赋值/特判修改 ~20 处（img/table/head/option/ctrl/contenteditable/…）——写前 detach |
| src/layout/layout.cpp | `Builder::add_run` | run->style 赋值/拷贝 |
| src/layout/layout.cpp | build 内继承 merge（font-size/color 等） | 决定：写进 node style（detach）或改"继承差异层" |
| src/layout/layout.cpp | `relayout_impl` cache hit 路径 | n->style = 共享 cache（免拷贝） |
| src/layout/layout.cpp | layout()/布局读 style（只读） | 共享安全，确认所有读点 const |
| src/render/render*.cpp | paint/几何 sget/get | 只读，确认 const 化 |

## 2. 方案 A：共享不可变 cascade + 继承差异层（收益最大，风险最高）

- node->style 拆两层：
  1. cascade 结果：**共享 const map**（compute / cache 命中共享，零拷贝）
  2. 继承/特判层：只存本节点新增/覆盖键（查询时先查本层再沿父链）
- 收益：行 build 省 compute 返回 + cache 拷贝（~50us/行）
- 风险最高点：sget/几何查询语义（全引擎几十处 get/find——漏一处 =
  继承键缺失 bug）
- 反弹：合并查询沿父链 vs 单 map 查找（每属性 O(深度)）——若 build 时
  物化继承键又回到拷贝。需覆盖层判空短路（size==0 免额外 find）。

## 3. 方案 B：COW 共享（改动小、收益中、反弹风险低）

- `WhaleUIComputedStyle = shared_ptr<map>`；读 *ptr 零改；写前 detach
  （use_count>1 才拷贝）。
- 收益：cache 命中（整树 rebuild 大部分命中）免拷贝。
- **风险**：继承 merge 若每元素必写 -> 每元素 detach 一次（拷贝 50 键）
  -> 收益被抵消（0 收益甚至反弹）。需先确认 build 继承逻辑是否为
  "仅差异写入"。

## 4. 先决小实验（实施第一步，决策 A vs B）

## 4. 先决分析结论（2026-06 静态审查 + 前几轮插桩数据）
- **继承 merge 是"仅差异写入"**（layout.cpp ~1596：只在 n->style 缺键且
  parent 有键时写 font-size/color/font-family/line-height/
  letter-spacing/cursor）——每元素实际 ~2-3 键，非全量。
- compute 返回值经 NRVO/move 赋给 n->style（O(1) 窃取，无拷贝）。
- **主拷贝源 = add_run（layout.cpp 1315）`t->style = style`**：每个文本
  run 深拷贝 node 的整个 style map（~50 键，~20-25us/run）；fill 每行
  li>a 一个 run → ~25us/行。
- cache 命中路径 `n->style = sit->second`（const 引用 → 深拷贝 ~20us）
  只影响整树 rebuild（fill 行是新元素走 compute/move，无此拷贝）。

**修正后的主攻点**：
1. run style 拷贝（fill 补齐 ~25us/行）——paint/布局读 style 全部经
   `nd->style`（sget/get 通用，run 与元素同路径），run 的 style 语义与
   node 相同 → 可共享（run 只读；唯一写 run style 的是 scale_fonts，
   与 node style 同步写同值）。实现选型：
   a. run->style 存"空 + 读时回退父节点 style"（sget 侵入小改：is_text
      且 style 空 → 用 parent->style）——改动最小（add_run 一处 +
      sget/读点兜底），风险：任何绕过 sget 直读 nd->style 的地方。
   b. style 表示共享（shared_ptr）——改动面大（全引擎读点）。
2. cache 命中深拷贝（整树 rebuild）——共享 cache 值（若 n->style 表示
   允许共享）。

方案 B（COW 全 map detach）因继承写仅差异而**不必要**（detach 拷贝 50
键只会抵消收益）——放弃。

## 4.3 量化推翻（2026-06, 插桩实测后）: StyleHandle 重构叫停

add_run 拷贝实测: 平均 3.1us/次, WPT style 仅 8 键(map 小) - 占行成本
0.2ms 的 ~1.5%。StyleHandle 全引擎语义重构(shared cascade + own 差异,
~30-100 访问点)收益 <5% 风险全引擎 -> 按反弹预案叫停, 不改 style 表示。
行 build 0.2ms 的真实构成(compute 50us + run 3us + 结构 ~150us)仍需
精确分段插桩(下轮第一动作) - 若结构 150us 属实, 优化点在 build 结构
(new_node/特判 get/继承 find 次数), 非 style 拷贝。

## 4.2 实施路径选型（2026-06 定稿, 已被 4.3 叫停 - 仅存档）

- 4.1a（run 空 style + 读点回退父）**受阻**：style 读点 ~103 处且
  get/sget 签名只收 style 引用（无节点/parent 上下文），逐点改造
  风险与工作量不成比例。
- **选定：WhaleUIComputedStyle 改共享句柄 struct**：
  `struct StyleHandle { std::shared_ptr<const map> shared; /* cascade */
   std::unique_ptr<map> own; /* 特判/继承差异写 */ }`——
  - get/sget 等**集中访问器签名不变**（仍收 style 引用），内部改为
    "own 优先 + shared 兜底"（own 空时一次 shared find）——~103 读点
    中走 get/sget 的**零改动**；直读（`.style.find`/`[k]`）逐点改访问器
    （数量先 grep 确认，估计 <30）。
  - node style 与 run style 共享同一 shared cascade（compute/move 一次、
    add_run 传引用共享），特判/继承差异写进 own（每元素 ~2-3 键，
    own 分配按需）——run 拷贝墙（~25us/行）消除。
  - cache 命中直接共享 shared（整树 rebuild 免深拷贝）。
  - 风险与缓解见 §6；**实施需完整轮 + 全量回归**（继承键一致性测试、
    像素断言、fill/±/hover/stress）。

## 5. 方案 C（零 style 改动，并行兜底）

fill 建行无行间依赖 -> 多线程并行 build 行（共享 tree 追加式已安全，
行产出各自独立再链入）——4 线程行成本/4 -> 补齐 ~1.2s。
风险：style_cache/by_el 并发（fill 线程已锁内跑 append——多 fill 线程
需分锁或批内并行 build 再串行链入）。这是不碰 style 表示的兜底。

## 6. 预评估 bug 清单（按概率）

1. 【高】继承键缺失/错值（合并查询漏路径）-> 字体/颜色/行高错乱。
   检测：test_render 像素断言 + demo 目测 + 新增继承链 font-size 测试。
2. 【高】特判写共享 map 污染（未 detach）-> 同 tag 元素 style 串扰。
   检测：同构多元素像素断言。
3. 【中】cache 与覆盖层一致性（hover 状态 key 含 st）-> hover 样式错。
4. 【中】run style 共享后 paint 读 run style 与 node 覆盖层不一致。
5. 【低】shared_ptr 生命周期（环/悬挂）——无环设计规避。
6. 【低】fill 线程写（build detach/覆盖层）与渲染帧读共享 map——共享
   不可变则安全；写是替换指针（锁内）需确认。

## 7. 性能反弹预案

- 反弹 A（查询合并变慢）：覆盖层判空短路；仍慢 -> 回退物化（白名单）。
- 反弹 B（detach 拷贝频繁）：停止重构（git 还原点），改走方案 C 并行。
- 兜底：重构前先提交插桩与基线测试；重构分多个可回滚提交。

## 8. 实施顺序（下一轮）

1. 插桩统计继承/特判实际写键数（决策 A/B/C）
2. 若共享：style.h 改 shared_ptr + 全引擎读点 const 化编译
3. build 特判/继承写改覆盖层 + 查询合并封装
4. cache 命中免拷贝
5. 回归：全测试 + perf（补齐/±/hover）+ stress + 像素目测
