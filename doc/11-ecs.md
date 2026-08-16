# ECS + 逻辑树架构(第三步标签功能的内部表示)

本文件描述 WhaleUI 内部渲染表示的 ECS(Entity Component System)化设计、
已落地部分与完整迁移方案。目标:**让每个"div"的占用足够小,遍历/绘制
走缓存友好的 SoA 组件数组**,达到最佳渲染性能(README 第三步第 2 点)。

## 现状(迁移前)

- **DOM**:lexbor 树(解析/查询/选择器)。每个元素是 lexbor 的 struct
  (AoS:属性和指针内联),修改其内部表示不现实,lexbor 本身是高效的 C 库。
- **布局树**(`src/layout/layout.h` 的 `whaleui_layout_node`):每个节点一个
  struct,包含 el / border / content / margin / padding / border_w /
  visible / is_text / z / opacity / bounds / scroll / style(std::map)/
  text(std::string) / parent / first_child / next —— 典型的 AoS 布局。

布局树是"所有 tag 视为 div"后的**内部渲染表示**,也是渲染/命中的热数据
结构,是 ECS 化的主战场。

## 已落地:tag_id 组件(第一步)

- `WUI_TAG_*` 枚举(`layout.h`):常用 tag 的类别 id(约 50 个)。
- `tag_id_of()`(`layout.cpp`):布局时对每个元素计算一次 tag 类别。
- `whaleui_layout_node::tag_id`:节点上的 ECS 组件字段。
- 渲染热路径使用:
  - `is_editable_node()`(render.cpp):`INPUT/TEXTAREA` 先经 tag_id 判定,
    避免每个绘制节点都调用 `lxb_dom_element_local_name`。
  - `paint_node`:checkbox/radio、progress/meter 用 `tag_id` 预过滤,再查
    属性(checkbox 的 type 等仍须查属性,不能只靠 tag)。

收益:绘制/命中循环里对每个节点的 tag 字符串比较(memcmp 链)变为一次
整数读取。这是 ECS 组件模式的第一处落地,后续组件沿用同一模式。

## 完整方案:布局树 SoA 化(后续步骤)

把 `whaleui_layout_node` 的字段拆成组件数组,节点变成轻量实体 id:

```
实体:uint32_t id(布局树 arena 的下标)
组件(SoA,每组件一个 vector / 稀疏存储):
  el[]          lxb_dom_element*   (仅元素节点)
  border[]      whaleui_rect_t     (热:绘制/命中每帧读)
  content[]     whaleui_rect_t     (热)
  opacity[]     float              (热:合成链)
  flags[]       uint8              (visible / is_text / …)
  z[]           int                (排序)
  bounds[]      whaleui_rect_t     (剔除)
  scroll[]      {y, max}           (滚动容器,稀疏)
  margin/padding/border_w[4]       (布局期写,绘制期少读 → 可稀疏)
  style[]       WhaleUIComputedStyle (重组件,仅布局期/动画用 → 稀疏 map)
  text[]        std::string        (仅文本 run → 稀疏)
  逻辑树:parent[] / first_child[] / next[](三个 uint32 数组)
```

设计要点:

1. **热组件 vs 冷组件**:border/content/opacity/flags 进密集数组(绘制与
   命中的主循环按序遍历,缓存命中率最高);style/text/margin 等低频访问
   走稀疏存储(存在即分配,或全局小池)。
2. **迭代器**:`for_child(id)` / `for_each_visible(id)` 内联,绘制/命中共
   用同一遍历,保证前序序号一致(选择高亮依赖)。
3. **实体复用**:free-list 回收被移除节点(动态 DOM)。
4. **与 lexbor 的关系**:DOM 保持 lexbor(解析/查询/选择器不变);布局树
   是 lexbor 树的**派生内部表示**,每次布局重建时从 DOM 计算,生命周期
   与帧一致(现状已是如此,只是存储格式从 AoS 改 SoA)。

## 迁移步骤(建议在独立提交中完成)

1. `layout.h`:定义 `WhaleUIEcsTree`(组件数组 + 逻辑树数组 + 实体 id),
   `whaleui_layout_node_t` 改为 `{ uint32_t id; }` 句柄。
2. `layout.cpp`:build/layout 写入组件数组;所有 `n->field` 改为
   `tree->component[n->id]`(机械替换,测试守护)。
3. `render.cpp`:paint/hit/scroll 读取组件数组;`paint_node` 签名增加
   tree 引用(或全局当前树)。
4. `tests/test_layout.cpp` / `test_render.cpp`:字段访问改为经 tree 的
   访问器(白盒测试同步)。
5. 基准:用文档记录的 21KB/497 节点页面滚动帧耗时(6ms)作基线对比。

## 风险与收益

- **收益**:绘制/命中/剔除主循环从指针追逐(struct 链)变为顺序数组
  遍历;布局树总占用下降(移除每节点 std::map/std::string 的内联开销,
  冷组件稀疏化);动画/合成只扫 opacity 数组。
- **风险**:迁移面大(约 600 处字段访问),一次提交不宜与功能改动混在
  一起;style/text 稀疏化需保证生命周期(布局树随帧重建,无跨帧指针)。
- 结论:收益集中在热循环(SoA 顺序访问),适合在功能稳定后作为独立
  性能专项执行;本文件即该专项的蓝图。
