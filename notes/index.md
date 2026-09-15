# 项目进程: WhaleUI

第六步(长期维护): 修复 BUG、补充功能与优化。

<1>(SOLVED): 主题 CSS 按参考重写 —— 编译通过,16 张离屏渲染的深浅配对与 token
对照自检全部通过;引擎缺口的实测结论记在 doc/internal/10-theme-gaps.md
<2>(UNRESOLVED): 主题样式与参考截图的肉眼比对未做(本机无图像理解能力),见 notes/current.md
<3>(TIPS): 判断引擎支持什么不要看文档、要实测 —— 探针页 temp/gaps.html、
temp/gaps2.html 每行一种写法,渲染后用 temp/rect_color.py 采样(本次实测推翻了
"伪元素只是文本 run""相对定位会压成竖线"等文档结论)
<4>(SOLVED): 引擎 CSS 缺口修复(16 项,其中 6 项是清单里没有的或描述错的) ——
水平 padding/margin 取了 right 索引、inline-flex 撑满整行、out-of-flow 子项被当
flex item、text run 继承 overflow 被当容器、多阴影只画第一层(且无 spread)、
outline 不绘制、四边 border-color 退化单色、text-shadow 与**全部彩色文字**
R/B 反转(GPU text layer 双重交换)、UTF-8 BOM 让整页 CSS 失效、
`[attr]`/`:checked`/`:disabled` 对无值属性不匹配、无方向 `linear-gradient()`
不画、`background-size/-position/-repeat` 被忽略、`hsl()` 不认、
`rotate()` 带崩整串 transform、渐变过渡瞬时、checkbox 布局盒被 UA 的
`min-height` 撑成 32px;支持矩阵与逐项证据见
doc/internal/16-engine-support.md,修复前数据留在 temp/engine-gaps.md
<5>(UNRESOLVED): `transform: rotate` 只解析不绘制(需矩阵 paint 路径);
`backdrop-filter` 仅 GPU 路径;`conic/repeating-gradient`、`hwb/oklab/color-mix`、
`attr()/counter()`、`::first-line`、`:has()`、容器查询、`filter`/混合模式未实现 ——
见 doc/internal/16-engine-support.md 与 notes/current.md
<6>(SOLVED): hover 让页面上下跳 / flex 盒子悄悄变长 20px —— 两个根因, 都已修:
(1) state relayout 段与 DOM-dirty 增量重排段跑在全量布局分支的文本度量上下文
(`g_metric_render`)之外, 真实字宽 251px 被算成 0.6em 估计值 231px, flex 的
min-content 跟着变小 -> h1 的可用宽被压到 231 -> 换行 -> header 变高 33px、整页下移;
(2) 重排前的布局键 diff 拿"重新 cascade 的 display"(`inline`)与"节点的归一值"
(`inline-block`, layout.cpp 给 contenteditable/select 做的归一)比对, 于是**每次
hover 都误判为布局变化**, 白跑一次全树盒 pass —— 把 (1) 放大成"hover 必跳"。
修法: 两条重排段各自在段首设 `g_metric_render = r`、段末清空(各段自洽不依赖全局
顺序); display 按布局类别等值比较(`same_layout_display` / `layout_key_differs`),
两条路径共用。复现/验证: `temp/probe_hover.cpp`(整树 xy/wh diff), 修复后 hover 段
只剩进度条动画、`scroll_max` 全程不变; 机制、探针用法、回归测试的覆盖边界见
doc/internal/17-interaction-state.md
<7>(SOLVED): 超长文本闪退 + hover 延迟 ~1s ——
(1) 22 万字符单 text node 段错误: `draw_text_at` 里 (a) miss 路径的 raster buffer
是块内局部变量, `src = &buf` 出了块就悬垂(每个未缓存 run 都在读已释放内存);
(b) 单条 raster 远超 16MB 缓存预算时, 逐出循环在"map 里只有这一条"的情况下把它
自己 erase 掉, 紧接着 `src = &e.px` 读已释放内存。修: buf 提到函数作用域 +
单条超预算不入缓存 + 逐出跳过正在交付的 key。
(2) 同一页面的 raster 从"整个 run"改成"只光栅化可见行"(可见行区间 + 1 行 slack,
blit 起点偏移 ty_shift; 部分光栅依赖滚动位置故不进缓存): 9.2s+崩溃 -> 3.4s 正常。
(3) hover 延迟: `SDL_EVENT_MOUSE_MOTION` **每个事件**都做一次全树 hit_test +
SDL_SetCursor, 快速移动时一帧几十个事件 -> hover 滞后约 1s("移开才反应")。修:
motion 只记录最新位置, worker 每次唤醒做一次命中测试(window.h 的 mouse_pending)。
注意"帧慢"是被排除的(1/2/5 帧总耗时恒定 ~2.9s = 进程启动开销)。
未做: 布局仍是 O(文本长度)(3.4s 的大头); 异步首帧布局默认关; 分块/断点机制;
滚轮与触控板手感差异。见 doc/internal/18-long-text-and-input.md
<8>(SOLVED): 292k 字符单节点滚动卡(46ms/帧 -> 目标 16ms 未达) —— 视口裁剪只省了
光栅化, 行布局表(TextLayout)与字形图像(GlyphImg)仍每帧整串重算: 加两个缓存
(按 text/family/size/weight/lsp/wrap/lh 与 family/size/style/cp/tint 作 key),
命中返回**指针**不拷贝, 条目只增不删(超预算跳过插入, 指针不悬垂)。
215ms -> 46ms/帧(小页面 9ms 不受影响)。剩余: keys+get 15ms(嫌疑在
render_get_font/ensure_font_state)、raster 11ms、paint 其它 14ms。
未做: 子节点按树/分块存储(用户方案: 不设阈值直接按树, 小的不生长, 对外
first_child/next 语义不变) —— 需单独一轮 + 全量验证。见
doc/internal/18-long-text-and-input.md
<9>(SOLVED): UTF-16 文档/样式表只渲染出一个字符 —— 两处缺口缺一不可:
(1) window.cpp 加载路径用 std::strlen(html) 传长度, UTF-16 每个 ASCII 字符后跟
0x00, strlen 立刻截断(45KB -> 3 字节到不了解析器);
(2) 解析入口(dom/css)只跳 UTF-8 BOM, UTF-16 的 FF FE/FE FF 没处理, 按 UTF-8 解码
时 0xFF/0xFE 非法 + 剩余 0x00 当串尾 -> 全文塌成第一个 ASCII 字符。
修: 新增 header-only src/core/utf.h 的 wui_utf8_view() 在边缘统一处理编码;
window.cpp 抽出 window_load_html_len() 让 load_uri 传真实字节数(C API 语义不变)。
回归: tests/test_dom.cpp::test_dom_utf16_and_bom_input(UTF-16LE/BE + UTF-8 BOM)。
未做: 无 BOM 的 UTF-16 嗅探(记事本存 UTF-16 一定带 BOM)。commit b8f3cbf。
<10>(SOLVED): 长文本滚动真正的 5 倍在 xmake.lua —— release 构建从未开优化。
xmake 不会因 -m release 自动加 -O, g++ 停在默认 -O0(demo 也一样)。加
set_optimize("fastest")(-O3)后: 292k 字符单节点 46-54ms/帧 -> 10-16ms/帧
(60fps 以内), showcase 8ms。之前分段测量(entry/tlkey 各 5-6ms 的 FNV、raster 15ms)
全是 -O0 的产物; 基准: 292k 字符 FNV 每遍 -O0 3.85ms / -O2 0.51ms。
叠加 commit 5651aca 的两个缓存, 相对最初 215ms/帧 共约 14-17 倍。
-O3 下 10 项测试全绿 + 14 张主题自检 OK(未暴露 UB)。
教训: 性能测量前先确认优化级别。见 doc/internal/18-long-text-and-input.md 第七节。
