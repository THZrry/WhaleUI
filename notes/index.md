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
<11>(SOLVED): 滚动帧里最后三处 O(文本长度) 哈希 —— text_cache 键 / 行表 tl_key /
度量 textw_key 都在对整段文本做 FNV(292k 字符约 1.5ms/帧), 正是"视口截取开销可
忽略"不成立的原因。加 render.h 的 text_ver: 每帧 whaleui_dom_take_dirty() 非空就
++, 三处键改为 (buffer 指针, size, text_ver) -> O(1)。292k 单节点滚动
10-16ms -> 7-10ms/帧, 与 showcase(7ms) 持平, 即与文档长度无关。
回归: tests/test_render.cpp 断言 text_ver 双向(空闲不变 / DOM 变更后必变)。
相对最初 215ms/帧 共约 21-30 倍。见 doc/internal/18-long-text-and-input.md 第八节。
<12>(SOLVED): DOM 变更的文本缓存失效粒度过粗 —— <11> 用全局 text_ver, 任何 DOM
变更都失效所有文本缓存; 但 DOM 变更很高频(动画/编辑/动态列表), 没改的内容不该失效。
改: whaleui_dom_take_dirty_typed 带出"是否结构性改动"(_DirtyEntry.structural, 同
元素样式->结构会升级); 渲染侧 r->el_ver 只给结构性改动的元素 ++版本, hover/class/
attribute 不失效; text_cache 键用该元素版本; 行表/度量的键改用 (文本缓冲指针,size)
(布局重建会换 text_arena 条目, 指针随内容变)。回归: test_render 断言空闲不版本化/
结构改动恰好版本化该元素/样式改动不版本化。见 doc/internal/18 第九节。
<13>(SOLVED): 子节点分块(chunk)落地 —— WUI_CHUNK=64 合并盒, paint/hit_test 按块
剔除(100k 兄弟 11-14ms -> 2-4ms/帧), first_child/next 语义不变。保留结论: 分块
收益在"遍历次数"不在内存布局(deque 顺序已足够); bounds 失效机制以 d8cbcfe 的
修正版落地(标脏冒泡 + 全量帧 mark_bounds_dirty_all + 根重算早退), 铁律:
bounds_valid="本帧不重算"的语义动前必须过 test_scroll 的 hover 用例。见
notes/current.md
<14>(SOLVED): xf 预计算标志与动画逐帧写 style 脱节 —— transform 过渡注册时
style 被钉在中性值 "none", 插值只经 whaleui_anim_apply_ov 逐帧写入(不经
build), xf 停在 0 -> paint_node 永不位移(transform 过渡不上浮)。修: 标志块抽成
whaleui_layout_node_refresh_paint_flags 单一写点(build 与 apply_ov 后共用);
回归 tests/test_scroll.cpp 的 hover-transform-transition(修前 FAIL 修后 PASS,
均实测)。见 notes/current.md
<15>(SOLVED): backdrop-filter 采样错位(自引入即坏 —— uv 误用 shadow 的 *0.5
约定, 全屏 blit 应为 px/fb, 覆盖盒采到帧左上四分之一) + blur 双档近似落地:
blur>=8 mip 阶梯(原样), blur<8 预定义 3x3 二项式核作用于单 mip 层、octave 内
t=log2(blur)-L 从恒等升到全核。同轮 rotate 的背景/边框改为真实旋转(GPU 顶点
旋转, SDF 仿射不变故圆角精确), <5> 范围缩小为文本/子树/渐变/命中不转。见
notes/current.md
<16>(SOLVED): backdrop body 走 LATE pass 保住渐变/边框 + 圆角裁剪; inset 改
mask 反演(1-blurred_mask(px-off),offset 精确,blur:0 实环); 外阴影 uv 采样
位移修复; param buffer 上溢致 D3D12 整帧黑屏修复; rotate 渐变轴同转 +
hit_test 逆旋转。见 notes/current.md
<17>(SOLVED): P0 四项 —— transform-origin(scale/rotate/hit 锚点);
:not() 单层+状态取反; ::selection-fg(逐字形换色); 伪元素去名单(全量合并)。
清单 temp/css-todo.md。见 notes/current.md
<18>(SOLVED): CSS 缺口四批 —— play-state/placeholder/stroke分写/overflow分写/
object-position(+fit轴选反/RGB24修); :enabled等+attr六档+attr(); rem/vmin/
calc乘除/radial关键字/focus-within/is-where; 多过渡timing/sticky远边/
scrollbar/aspect-ratio/marker。见 notes/current.md
<19>(SOLVED): filter(元素滤镜) —— GPU pass E(mip blur+4x5矩阵,记录外扩)+
CPU颜色矩阵; 附带恢复被插桩关掉的外阴影 pass。见 notes/current.md
<20>(SOLVED): transform skew/matrix/3D —— 顶点剪切+hit逆剪切+渐变协变;
matrix分解; rotateX/Y透视压缩; 3D消费忽略。见 notes/current.md
<21>(SOLVED): 混合模式 —— background-blend折角颜色+mix-blend pass F;
11种精确+4种亮度近似。见 notes/current.md
<22>(SOLVED): clip-path/mask —— inset/circle/ellipse 顶点SDF+CPU镜像+全发射器;
mask linear-gradient逐角alpha, CPU/GPU双路径, 热路径门控。见 notes/current.md
<23>(SOLVED): 颜色与渐变值 —— hwb/lab/oklab/lch/oklch→sRGB + color-mix(srgb/
srgb-linear/hsl精确, oklab近似) + conic(CPU精确/GPU留底) + repeating取模
(CPU精确, GPU轴向条带/10层)。见 notes/current.md
<24>(SOLVED): 计数器 —— reset/increment/set作用域栈(RAII) + counter()/
counters()取值(decimal/alpha/roman) + content混合token。见 notes/current.md
<25>(SOLVED): 权重与媒体 —— :is/:not取参最大/:where零 + 选择器逗号括号感知切分
+ @media高/方向/比例/hover/pointer/分辨率 + contain:paint裁剪。见 notes/current.md
<26>(SOLVED): 首行/首字 —— match_pseudo 4/5 + fl/flt位 + paint首行wrap拆分/
首字形独立绘制。见 notes/current.md
<27>(SOLVED): :has() —— 后代/子/兄弟相对选择器求值 + 取参最大权重。见 notes/current.md
<28>(SOLVED): hover 失效的链路级根因四处(hit/paint 不一致×3: fixed 偏移/
translate 子树/兄弟优先序 + hover 只由 motion 驱动补帧内重命中) +
动画帧 3-4 次全树走访改 O(动画元素) + 值不变跳帧 + 动态刷新率帧率策略 +
tl 缓存超预算无界生长/reset_dom 指针键缓存漏清 —— 见 notes/current.md
<29>(SOLVED): GPU 高帧率占用 —— 五个全屏区域 compute(shadow/backdrop/
inset/filter/mixblend)改记录包围盒 dispatch(uniform 偏移, 输出逐字节
一致) + shadow 清零 RMW 改 clear pass + backdrop/filter 源 blit 区域化
(mip 足迹外扩/偶对齐) + partial 帧主 raster 条带 scissor + 边缘线程
越界守卫 —— 见 notes/current.md
<30>(SOLVED): hover 延迟真因(布局动画与 paint-only 过渡不组合, 通用
apply_paint_only_anim) + select 三连(开关去全量重建走 scroll 帧/点选
bump el_ver/scroll_el 配 scroll_dirty/值文本基线居中) + DOM 三阶段事件
(捕获/目标/冒泡 + currentTarget/phase + preventDefault 取消按下链) ——
见 notes/current.md
<31>(SOLVED): checkbox 不同行(inline_member 误杀 relative, 统一在流判定)
+ toggle 去全量重建 + splice 不同步 chunk 链表(rechunk_parent 两处 +
state 分支 root 重算 + hit 位移块守卫) —— 见 notes/current.md
<32>(SOLVED): select 文字两遍(箭头命中值文本光栅, 短文本内容哈希进键) +
下拉全同字(popup 行禁用缓存) + popup 的 CSS 影子树匹配 + 打开过渡 ——
见 notes/current.md
<33>(SOLVED): 点击闪一下(scroll_el 信号误判真滚动吞损伤 +
mouse-up 的 :active 恢复无损伤) + scrollbar 列全树走访改注册表 ——
见 notes/current.md
<34>(SOLVED): 滚动拖影(wheel 逐事件滚动改合并 + 帧请求续帧 +
scroll 帧保留状态标记 + scroll_dy 死代码清理) —— 见 notes/current.md
<35>(SOLVED): 元素特判点统一(input 三分类单写点 + select 值 DOM 同步 +
绘制 CtrlKind 单分发 + thead/tbody/tfoot/li 标签号) + 三处热路径
(style_cache/fam/glyph 哈希化) + RuleTagIndex 悬空复用回归修复 ——
见 notes/current.md
<36>(SOLVED): 输入类残留收敛(kind 3/4 + contenteditable 单写点 + number
过滤合一 + radio 组迭代漫游 + label for 接通) + UA 默认尺寸下沉
(checkbox/input/textarea/select/progress/meter/img) + 热路径去重解析
(inset/min-max/transform) —— 见 notes/current.md
<37>(SOLVED): 小窗口功耗爆炸(presented 快路径: 值不变帧跳 paint+GPU,
后台/遮挡/最小化停渲+tick 保活) + 纯滚动永久拖影(scroll 判定快照,
消费后 paint 侧恒假) + scroll-each 五帧回归 —— 见 notes/current.md
<38>(SOLVED): CPU 占用三板斧 —— (1) caret 跳帧(500ms 相位门, 同相位停
paint 保 alive, caret 画不出 strip 故永不 partial); (2) 自驱/事件双帧率门
(事件帧 60fps 保输入, 无输入自驱跟 display_refresh, 高刷屏省 CPU);
(3) 多窗口后台节流(失焦兄弟窗口 10Hz paint + tick 保活, 单窗口零行为
变更) + caret-blink 回归 —— 见 notes/current.md
<39>(SOLVED): 双轨 pacing(动画 60fps / 滚动显示刷新率, 输入唤醒即切高速)
+ 遮挡按可见性分级(全遮挡全暂停, 半透明盖住走后台 10Hz)+present 路径
时钟无关(WebGPU 继承策略不变) —— 见 notes/current.md
