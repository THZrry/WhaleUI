# 超长文本渲染 & 输入响应

两个问题都是 2026-09 发现的：一个 22 万字符的纯文本（agent 对话历史）
直接把引擎打到**段错误退出**；鼠标 hover 差不多每次要等约 1 秒才响应。

## 一、超长纯文本崩溃（已修）

**现象**：`<div>` 里放一段 22 万字符的纯文本（一个 text node），页面卡约
9 秒后**段错误**（`0xC0000005`）。

**定位**：加阶段打印后确认崩在 **paint**：

```
[dbg] layout start
[dbg] layout done
[dbg] fix_run_heights done
[dbg] paint start          ← 之后就是崩溃
```

**根因**（都在 `render_text.cpp` 的 `draw_text_at`）：

1. **`src = &buf` 是悬垂指针**：cache-miss 的 raster buffer 原本是
   **cache-miss 块内的局部变量**，而 blit 在块外 —— 块一结束 `buf` 就析构，
   `src` 指向已释放内存。**每个未缓存的文本 run 都在读已释放内存**
   （只是小 run 恰好没被踩坏）。
2. **缓存逐出会把刚插入的这一条也删掉**：单条 raster（`tw × th`）远超 16MB
   预算时，逐出循环跑的瞬间 map 里往往**只有这一条**，于是"LRU 受害者"就是
   它自己 → `erase` → 紧接着 `src = &e.px` 读已释放内存 → 段错误。

**修法**：

- `buf` 提到函数作用域（`th_buf` / `ty_shift` 一起），miss 与 hit 两条路径
  都安全；
- **单条超过预算的 raster 直接不入缓存**（`e_bytes > kMaxBytes` →
  `erase(ckey_str)` + 用 `buf`），逐出循环另外**跳过正在交付的那个 key**。

## 二、只光栅化可见行（"截取一部分渲染"）

原来 `draw_text_at` 把**整个 run** 光栅化：`tw × th` 的 buffer +
每个字形一次 TTF 取像。22 万字符 ≈ 3400 行 → **240MB buffer + 22 万次
字形光栅化**，而且这个 buffer 还会试图进缓存。

现在先按 `ty` / `th` / framebuffer 高算出**可见行区间**：

```cpp
first_line = ty < 0 ? (-ty) / lh : 0;
last_line  = (ty + th > fh) ? (fh - ty + lh - 1) / lh : L.lines.size();
ty_shift   = first_line * lh;
th_buf     = (last_line - first_line) * lh + lh;   /* +1 行给字形上沿 */
```

只光栅化 `[first_line, last_line)`，字形写进 buffer 的 y 用
`(li - first_line) * lh + baseline_off`，blit 起点用 `ty + ty_shift`。

- **部分光栅的内容依赖滚动位置 → 不进缓存**（`ckey_str.clear()`）；每帧只
  光栅化可见部分，成本 O(可见) 而不是 O(全文)。
- 整 run 可见时与之前行为一致（照旧走缓存）。

**效果**：同一个 22 万字符页面 **9.2s 后崩溃 → 3.4s 正常输出**。

## 三、hover 延迟约 1 秒（已修：输入合并）

**现象**：hover 差不多每次要等 ~1s 才响应，经常"鼠标移走了才反应"。

**先排除了"帧慢"**：`render_shot` 跑 1 / 2 / 5 帧的总耗时几乎恒定
（~2.9s）——那是**进程启动**开销，帧本身不慢。

**真因**：`process_event` 的 `SDL_EVENT_MOUSE_MOTION` **每个事件**都调
`whaleui_render_set_hover`，而那是一次**全树 `hit_test` + `SDL_SetCursor`
系统调用**。快速移动时一帧能产生几十个 motion 事件，队列排空速度赶不上
鼠标移动 → hover 状态滞后约 1 秒，看起来就是"移开才反应"。

**修法**（标准输入合并）：motion 事件只**记录最新位置**
（`window.h` 的 `mouse_pending` / `mouse_x` / `mouse_y`），渲染 worker
**每次唤醒做一次** hit-test；`mouse_pending` 计入 `want`，保证应用了 hover
之后一定会出帧。

> 状态：逻辑上把 hover 命中测试从 O(事件数) 降为 O(1)/帧，**未在真实鼠标下
> 实测**（无头探针模拟不了鼠标轨迹），需要跑一次 demo 确认手感。

## 四、还没做（按优先级）

1. **布局仍是 O(文本长度)**：22 万字符的 3.4s 里**布局占大头**
   （`est_wrap_lines` / 文本测量 / 行切分反复扫描整串）。可见行裁剪只治了
   光栅化，没治布局。
2. **异步首帧布局默认关闭**（`r->async_layout = 0`）→ 长页面首帧仍同步阻塞，
   这就是"有时候还会卡"。worker 路径（`layout_thread` + `layout_pending`）
   已经存在，需要把它作为默认并补齐 resize/hover 与它的交互。
3. **分块 / 断点（用户建议）**：一个元素的子节点过多时，按段插入断点、保存
   长度与纹理信息，使渲染可以只截取其中一段。现有机制只有两个局部特例：
   `whaleui_layout_append_rows`（列表尾追加）与 `<details>` 的
   progressive-expand 预算。要做的是把它一般化：**超长文本 run 先按行/字节
   切成多个 run**，让可见行裁剪之外再多一层"连布局都不用做"。
4. **滚轮 vs 触控板手感**：触控板丝滑、鼠标指针容易卡。注意 `MOUSE_WHEEL`
   **每个事件都该处理**（滚动意图不该合并，与 hover 不同源），引擎里也已经有
   滚动缓动（`step = diff * 0.4`）；所以问题更可能在**单个鼠标 tick 的位移量
   与那一帧的重绘量**（触控板给大量小增量，鼠标给离散大 tick），需要真机
   profile 才能定性。

## 相关文件

- `src/render/render_text.cpp`：`draw_text_at`（raster / 缓存 / blit）、
  `layout_text` / `layout_wrap`（行切分）
- `src/core/app.cpp`：`process_event`（motion 记录）、worker 的
  `mouse_pending` 应用与 `want` 判定
- `src/core/window.h`：`mouse_pending/mouse_x/mouse_y`、`resize_pending`
  （同样的"事件只记录、worker 每帧应用一次"模式）

## 五、视口截取之后：滚动性能（本轮继续）

用逐步屏蔽绘制的办法（`WUI_NO_PAINT` / `WUI_NO_DRAWTEXT` 探针开关）量化了
292k 字符单节点的**每帧**成本（`temp/probe_hover.cpp` 的滚动计时段）：

| 阶段 | 耗时 |
|------|------|
| 非绘制（布局/滚动/修正） | 5ms |
| `paint_node` 其它 | 14ms |
| `draw_text_at` | 35ms |

`draw_text_at` 内部再分（`WUI_DBG_TIME`）：

| 段 | 首次 | 之后每帧 |
|----|------|----------|
| 键哈希 + 取缓存 + `render_get_font` | 216ms | 15ms |
| 可见行光栅化 | 11ms | 11ms |

修掉的两处，都是"每帧做整串的活"：

1. **行布局表缓存**（`TextLayout`）：`layout_text` 会把整串的换行点与每行宽度
   算一遍（292k 字符 → 2000 行、几十万次字形度量）。视口裁剪只省了**光栅化**，
   行表仍每帧重算 → 215ms。现按
   `(text, family, size, weight, letter-spacing, wrap width, line height)`
   缓存，并**返回指针而不是拷贝**（表约 5MB）。
2. **字形图像缓存**（`GlyphImg`）：每字形一次 `ensure_fallback` +
   `ensure_font_state` + `TTF_GetGlyphImage` + 逐像素预乘。现按
   `(family, size, style, codepoint, tint)` 缓存，命中返回**指针**（无锁
   `unordered_map`；条目只增不删、超预算的插入直接跳过，所以交出去的指针不会
   悬垂）。

**结果**：292k 字符单节点 **215ms/帧 → 46ms/帧**；小页面仍是 9ms，未受影响。

**还剩 46ms**（下一步，已定位）：
- `keys+get` 仍 ~15ms：表拷贝已排除，嫌疑落在 `render_get_font` /
  `ensure_font_state`（每帧对字体做 size/style 同步 + `TTF_GetFontHeight` 校验）；
- 可见行光栅化 ~11ms；
- `paint_node` 的其它固定开销 ~14ms。

## 六、还没做的结构优化：子节点按树/分块存储

用户给的方案（本轮未落地）：**不做阈值判断，直接按树组织子节点；子节点少的时候
树不生长**（一层就够），对外遍历 API 保持 `first_child` / `next` 不变。

- 现状：`whaleui_layout_node_t` 用 `first_child` + `next` 单链表，遍历散落在
  layout / hit_test / paint / events / animate（field 访问，无函数调用）。
- 目标：一个节点的子节点存进**数组块**，块之间连成树；小的时候就是一个根块。
  遍历仍提供 `first_child`/`next` 的语义（用游标模拟），**调用方不用改**。
- 收益：子节点极多的元素（10k 行列表、几百个 card 的容器）定位/插入从 O(n)
  降到 O(log n)，块还能整体跳过（配合视口裁剪）。
- 代价：`first_child`/`next` 现在是**字段访问**，改成游标后要么保留字段（同步
  成本），要么全量改调用点 —— 需要单独一轮 + 全量测试，本轮没动。

## 七、真正的 5 倍：release 构建从来没有开优化

上面所有分段数字（entry 5.5ms、raster 15ms）都是 `-O0` 的产物。查编译命令行发现
`xmake.lua` 里从头到尾没有 `set_optimize`, 而 xmake 不会因为 `-m release` 就替你
加 `-O` —— g++ 停在默认的 `-O0`。demo 也一样, 所以长文本滚动的卡顿是真实的, 不是
探针 artifact。

一行修复(xmake.lua):

    if is_mode("release") then
        set_optimize("fastest")   -- -O3
    end

单独基准(292k 字符的 FNV, 10 遍取均值):

| 优化 | 每遍 |
|------|------|
| -O0 | 3.85ms |
| -O2 | 0.51ms |

292k 字符单节点的实测(同一台机器, 同一探针):

| 阶段 | -O0 | -O3 |
|------|-----|-----|
| entry(FNV + 缓存查找) | 5.5ms | 0.51ms |
| tlkey(FNV + 行表缓存) | 5.9ms | 0.51ms |
| raster(39 行可见字形) | 15.4ms | 3.5ms |
| **整帧** | **46-54ms** | **10-16ms** |
| showcase 整帧 | 9-14ms | 8ms |

**结论**: 长文本滚动从最初的 215ms/帧 降到 **10-16ms/帧(60fps 以内)**。
其中"行布局表缓存 + 字形图像缓存"约 4.7 倍(commit 5651aca), release 优化再约 4 倍;
两者叠加。`-O3` 下全量测试与主题自检均通过(没有暴露 UB)。

**教训**: 做性能测量之前先确认编译优化级别。逐段计时得到的是"未优化代码里哪段
最重", 与用户实际运行的构建完全可能不是一回事。
