# more-dom-api — 待实现 API 交接清单

> 读者:下一个实现 Agent(第三步续作)。本文档列出 `include/whaleui.h` 中**已文档化但尚未实现**的公开 API。
> **签名已在 whaleui.h 冻结,不要改签名**——实现前先通读 `09-implementation.md`(架构修正、事件循环、已知限制)与对应模块源码。
> 已完成部分直接照旧;每实现一个 API,把下方清单的 `[ ]` 勾掉并跑对应测试。

## 通用约定(与 whaleui.h 顶部一致)

- `int` 返回:`0` 成功,非 0 失败;句柄返回:`NULL` 失败。
- 字符串返回由库持有,直到所属对象销毁。
- 集合(`whaleui_dom_list_t`)用 `whaleui_dom_list_destroy()` 释放;**快照语义**,不随 DOM 变更更新。
- 句柄即 lexbor 对象:`whaleui_dom_document_t*` == `lxb_html_document*`,`whaleui_dom_element_t*` == `lxb_dom_element*`(`src/dom/dom.h` 注释),内部直接 `reinterpret_cast`。
- lexbor 经 xrepo 引入(`3rdparty/` 下无 lexbor 源码);API 名以拉取到的头文件为准,下方为参考。
- 每个新 API 在 `tests/test_dom.cpp`(DOM)或 `tests/test_window.cpp`(窗口)加 assert 用例,零依赖风格,参照现有用例。

---

## 1. 集合类型 `whaleui_dom_list_t`

JS 对应:`NodeList`。查询类 API 的返回载体。

```c
size_t whaleui_dom_list_length(const whaleui_dom_list_t* list);
whaleui_dom_element_t* whaleui_dom_list_item(const whaleui_dom_list_t* list, size_t index);
void whaleui_dom_list_destroy(whaleui_dom_list_t* list);
```

- 内部:`{ lxb_dom_collection_t* col; size_t count; }` 或直接包装 `lxb_dom_collection_t`。空结果返回非 NULL 空 list(不要返回 NULL)。
- lexbor:`lxb_dom_collection_create(doc) / lxb_dom_collection_destroy / lxb_dom_collection_length / lxb_dom_collection_element(col, i)`。
- 测试:空结果非 NULL 且 length==0;多结果按文档序取出。

## 2. 查询 API(文档级 + 元素级)

JS 对应:`document.querySelectorAll / getElementsByClassName / getElementsByTagName` 与元素版、`element.closest / matches`。

```c
whaleui_dom_list_t* whaleui_dom_query_selector_all(whaleui_dom_document_t* doc, const char* selector);
whaleui_dom_list_t* whaleui_dom_get_elements_by_class_name(whaleui_dom_document_t* doc, const char* class_name);
whaleui_dom_list_t* whaleui_dom_get_elements_by_tag_name(whaleui_dom_document_t* doc, const char* tag);
whaleui_dom_element_t* whaleui_dom_element_query_selector(whaleui_dom_element_t* el, const char* selector);
whaleui_dom_list_t* whaleui_dom_element_query_selector_all(whaleui_dom_element_t* el, const char* selector);
whaleui_dom_list_t* whaleui_dom_element_get_elements_by_class_name(whaleui_dom_element_t* el, const char* class_name);
whaleui_dom_list_t* whaleui_dom_element_get_elements_by_tag_name(whaleui_dom_element_t* el, const char* tag);
whaleui_dom_element_t* whaleui_dom_closest(whaleui_dom_element_t* el, const char* selector);
int whaleui_dom_matches(whaleui_dom_element_t* el, const char* selector);
```

- 元素级查询**以 el 为根**,不含 el 自身(querySelector 语义);closest 含自身。
- lexbor:`lxb_dom_element_query_selector(el, sel, &entry)`、`lxb_dom_element_query_selector_all(el, sel, collection)`;文档级先取 `documentElement` 再查,或 `lxb_dom_document_elements_by_class_name/tag_name`。closest:逐级祖先调用 query_selector;matches:`lxb_dom_element_matches(el, sel)`(无则用 selector 匹配)。
- 注意:**选择器语义要跑通项目已支持的子集**(见 `09-implementation.md` CSS 支持矩阵:`#id/.class/tag/组合/后代/子代/相邻兄弟/伪类`)。lexbor 的选择器能力大于渲染能力,这里只需 lexbor 原生行为。
- 测试:`<div class="a"><p class="a"></p></div>` 断言 count/顺序;元素级查询不越出子树;closest 沿祖先链。

## 3. 树遍历

JS 对应:`parentElement / lastChild / previousSibling / firstElementChild / lastElementChild / nextElementSibling / previousElementSibling / children / childElementCount`。

```c
whaleui_dom_element_t* whaleui_dom_parent_element(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_last_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_previous_sibling(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_first_element_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_last_element_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_next_element_sibling(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_previous_element_sibling(whaleui_dom_element_t* el);
whaleui_dom_list_t* whaleui_dom_children(whaleui_dom_element_t* el);
size_t whaleui_dom_child_element_count(whaleui_dom_element_t* el);
```

- 现有已实现:`parent / first_child / next_sibling`(即 parentNode/firstChild/nextSibling,含文本节点)。
- 已实现三个是 `lxb_dom_node_*` 直接透传;新 API 注意**元素/节点区分**:parent_element 需向上跳过非元素;element 系列需过滤 `LXB_DOM_NODE_TYPE_ELEMENT`。`children` 可复用 `child_element_count` + 遍历拼 list。
- 测试:含文本节点的子树,断言 element 系列跳过文本、node 系列不跳。

## 4. 节点操作

JS 对应:`insertBefore / replaceChild / remove / cloneNode / contains / hasChildNodes / isConnected / createTextNode`。

```c
whaleui_dom_element_t* whaleui_dom_create_text_node(whaleui_dom_document_t* doc, const char* text);
int whaleui_dom_insert_before(whaleui_dom_element_t* parent, whaleui_dom_element_t* new_child, whaleui_dom_element_t* ref_child);
int whaleui_dom_replace_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* new_child, whaleui_dom_element_t* old_child);
int whaleui_dom_remove(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_clone(whaleui_dom_element_t* el, int deep);
int whaleui_dom_contains(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);
int whaleui_dom_has_child_nodes(whaleui_dom_element_t* el);
int whaleui_dom_is_connected(whaleui_dom_element_t* el);
```

- lexbor:`lxb_dom_document_create_text_node`、`lxb_dom_node_insert_before(parent, node, ref)`(ref NULL 追加)、`lxb_dom_node_append_child` 组合 replace(先 insert 后 remove)、`lxb_dom_node_remove`、`lxb_dom_node_clone(node, deep)`、`lxb_dom_node_contains`。
- `whaleui_dom_remove`:`lxb_dom_node_remove(el)` 后 el 句柄仍有效(lexbor remove 是摘除不解构),可再 append。
- `is_connected`:`lxb_dom_node_is_connected(node)`。
- 测试:insert_before 中间位置;replace 后 old 脱离;clone deep 复制子树且 detached;remove 后重新 append 成功;contains 含自身。

## 5. 文档级属性

JS 对应:`document.body / head / title / activeElement`。

```c
whaleui_dom_element_t* whaleui_dom_body(whaleui_dom_document_t* doc);
whaleui_dom_element_t* whaleui_dom_head(whaleui_dom_document_t* doc);
int whaleui_dom_set_title(whaleui_dom_document_t* doc, const char* title);
const char* whaleui_dom_get_title(whaleui_dom_document_t* doc);
whaleui_dom_element_t* whaleui_dom_active_element(whaleui_dom_document_t* doc);
```

- body/head:lexbor 的 `lxb_html_document` 自带 `body`/`head` 成员(见 `src/core/window.cpp` 里 `hd->head` 用法)。body 为空文档时为 NULL 是合理的(现有测试里空文档 `query_selector(doc,"body")` 非 NULL,与实现核对后再定)。
- title:lexbor 有 `lxb_html_document_title / lxb_html_document_title_set`(以头文件为准;set 时 head 缺 `<title>` 需创建,lexbor 通常自动处理)。
- activeElement:`focus_el` 状态现在由 render/app 维护(`10-theme-gaps.md` 的 `focus_el`);本 API 需要引擎暴露"当前聚焦元素"查询——在 render 层加内部 getter,或把聚焦元素记录上移到 document 结构。**先读 `src/render/render.h` 的 focus 相关字段再定**。
- 测试:parse 带 `<title>` 的文档读回;空文档 set_title 后读回;activeElement 在无焦点时 NULL。

## 6. 属性与 classList

JS 对应:`hasAttribute / removeAttribute / toggleAttribute / classList.add/remove/toggle/contains`。

```c
int whaleui_dom_has_attribute(whaleui_dom_element_t* el, const char* name);
int whaleui_dom_remove_attribute(whaleui_dom_element_t* el, const char* name);
int whaleui_dom_toggle_attribute(whaleui_dom_element_t* el, const char* name, int force);
int whaleui_dom_class_add(whaleui_dom_element_t* el, const char* cls);
int whaleui_dom_class_remove(whaleui_dom_element_t* el, const char* cls);
int whaleui_dom_class_toggle(whaleui_dom_element_t* el, const char* cls);
int whaleui_dom_class_contains(whaleui_dom_element_t* el, const char* cls);
```

- lexbor:`lxb_dom_element_has_attribute / remove_attribute`;toggle 用 has+set/remove 组合(force>=0 强制,<0 翻转,返回最终存在性)。
- classList:lexbor 若有 `lxb_dom_element_class_list_*` 直接用;否则读 `class` 属性按空格 token 操作再写回(注意多 class 保留顺序、去重)。class 变化后**样式需重算**——现有样式由 `whaleui_css_apply`/render 持有,需触发重布局(参照现有 `set_attribute` 后如何处理,读 `dom.cpp` 确认是否已有脏标记机制)。
- 测试:add 幂等(重复 add 不重复)、remove 不存在 class 返回 0 不报错、toggle 翻转、contains 边界(空格分隔)。

## 7. 内容:innerHTML / outerHTML / value

JS 对应:`element.innerHTML / outerHTML` 读写、表单 `.value`。

```c
int whaleui_dom_set_inner_html(whaleui_dom_element_t* el, const char* html);
const char* whaleui_dom_get_inner_html(whaleui_dom_element_t* el);
int whaleui_dom_set_outer_html(whaleui_dom_element_t* el, const char* html);
const char* whaleui_dom_get_outer_html(whaleui_dom_element_t* el);
int whaleui_dom_set_value(whaleui_dom_element_t* el, const char* value);
const char* whaleui_dom_get_value(whaleui_dom_element_t* el);
```

- get:lexbor serialize(`lxb_dom_element_inner_html / outer_html`,返回 malloc 字符串——**返回前需转成库持有内存**,现有 `get_text` 的做法可参照 `src/dom/dom.cpp`)。set:片段解析(fragment parse)后替换子节点——lexbor 有 fragment 解析 API(以头文件为准),或临时 `lxb_html_document_parse_fragment`。
- value:`<input>/<textarea>/<select>` 用 lexbor 表单接口(`lxb_html_input_element_value` 等,以头文件为准);其他元素返回失败/NULL。注意与 `value` **属性**区分(JS 语义:property 优先于 attribute)。实现后需同步控件显示(编辑框文本)——**读 `src/render/render.cpp` 的文本编辑路径,value set 后要触发重绘/重布局**。
- 测试:innerHTML 往返一致(parse → get → 与原串等价);set 后 querySelector 能查到新元素;value 在 input 上 get/set 往返,在 div 上返回 NULL/失败。

## 8. 焦点与几何

```c
int whaleui_dom_focus(whaleui_dom_element_t* el);
int whaleui_dom_blur(whaleui_dom_element_t* el);
typedef struct whaleui_dom_rect { float x, y, width, height; } whaleui_dom_rect_t;
int whaleui_dom_get_bounding_client_rect(whaleui_dom_element_t* el, whaleui_dom_rect_t* out);
```

- focus/blur:复用引擎现有焦点状态(`focus_el`,见 `10-theme-gaps.md`)。focus 后触发 `:focus` 样式刷新 + 重绘;blur 清除。
- getBoundingClientRect:数据源是**最近一次布局树**(`src/layout/layout.h` 的 `whaleui_layout_node_t`,含 `border/content` 盒与 `el` 指针)。需要在 render 或 layout 层提供内部查询:按 el 找布局节点,返回 viewport 坐标盒子(考虑滚动偏移、`position:fixed`),再转 float。**布局树每次重算,查询结果只在最近一次布局后有效**;未渲染过(无布局树)返回失败。坐标语义:JS 的 getBoundingClientRect 是相对 viewport 的 border-box。
- 测试:布局后 div 的 rect 与预期尺寸一致(简单固定尺寸页面);滚动容器内元素 rect 随滚动偏移。

## 9. 事件系统(最大块)

JS 对应:`addEventListener / removeEventListener / dispatchEvent` 与 Event 对象访问器。

```c
typedef struct whaleui_dom_event whaleui_dom_event_t;
typedef void (*whaleui_event_cb)(whaleui_dom_event_t* ev, void* userdata);
int whaleui_dom_add_event_listener(whaleui_dom_element_t* el, const char* type, whaleui_event_cb cb, void* userdata);
int whaleui_dom_remove_event_listener(whaleui_dom_element_t* el, const char* type, whaleui_event_cb cb, void* userdata);
int whaleui_dom_dispatch_event(whaleui_dom_element_t* el, const char* type);
whaleui_dom_element_t* whaleui_dom_event_target(const whaleui_dom_event_t* ev);
const char* whaleui_dom_event_type(const whaleui_dom_event_t* ev);
int whaleui_dom_event_prevent_default(whaleui_dom_event_t* ev);
int whaleui_dom_event_stop_propagation(whaleui_dom_event_t* ev);
int whaleui_dom_event_default_prevented(const whaleui_dom_event_t* ev);
int whaleui_dom_event_key_code(const whaleui_dom_event_t* ev);
int whaleui_dom_event_mouse_x(const whaleui_dom_event_t* ev);
int whaleui_dom_event_mouse_y(const whaleui_dom_event_t* ev);
int whaleui_dom_event_mouse_button(const whaleui_dom_event_t* ev);
float whaleui_dom_event_wheel_delta_x(const whaleui_dom_event_t* ev);
float whaleui_dom_event_wheel_delta_y(const whaleui_dom_event_t* ev);
```

支持的事件类型:`click, dblclick, mousedown, mouseup, mousemove, mouseenter, mouseleave, contextmenu, wheel, keydown, keyup, focus, blur, change, input, submit, load, scroll, resize`。

### 设计(按此实现,已在 whaleui.h 文档承诺)

- **派发模型:仅目标元素,无冒泡/捕获**。`stop_propagation` 保留接口,当前为 no-op(未来加冒泡时再生效)。
- **事件对象是派发期间的瞬态对象**:回调内有效,回调返回后失效。实现为一个栈上 struct,字段在派发时填充;访问器读字段;`prevent_default` 置标志位。
- listener 表:per-document 的 `(type, cb, ud)` 链表/哈希,key = element 指针。**文档销毁时清理**(document 结构里挂表,或静态表 + 注册时记 document)。
- `dispatch_event`:`el` 上构造事件、按 type 查表逐个调用;`click` 默认行为(select 展开/链接)若被 prevent_default 应跳过。

### 接入点(`src/core/app.cpp` 的 `SDL_PollEvent` 分支,参照现有 `render_handle_*` 调用)

| 事件类型 | 接入 | 数据来源 |
|---|---|---|
| mousedown/mouseup/click/dblclick/contextmenu | `SDL_EVENT_MOUSE_BUTTON_DOWN/UP` | 现有 hit-test(`render_set_pressed` / `render_handle_click` 已有命中逻辑;button 0/1/2 映射) |
| mousemove/mouseenter/mouseleave | `SDL_EVENT_MOUSE_MOTION` | 现有 `render_set_hover` 的 hover 元素;enter/leave = hover 变化时新旧元素 |
| wheel | `SDL_EVENT_MOUSE_WHEEL` | 现有 `render_handle_wheel` 的命中元素;delta = `e.wheel.y`(方向与内部滚动一致) |
| keydown/keyup | `SDL_EVENT_KEY_DOWN/UP` | 聚焦元素(`focus_el`);keycode = `WHALEUI_KEY_*` |
| focus/blur | 焦点变化点 | `render_set_pressed` / 现有 focus 设置处 |
| change/input | 文本编辑路径 | `render_handle_text` / 编辑提交处(先做 input 即可,change 在失焦/回车时) |
| load | `whaleui_window_load_html` 成功后 | window.cpp |
| scroll | 滚动后 | `render_handle_wheel` / 滚动条拖动处 |
| resize | `SDL_EVENT_WINDOW_RESIZED` | 现有分支 |
| submit | form 提交(暂无提交逻辑,可留空,不派发) | — |

- **dispatch 函数**:`src/dom/` 提供 `whaleui_dom_dispatch_typed(el, type, ...fields)`,app.cpp 各处调用。不要在每个 SDL 分支手写回调逻辑。
- `prevent_default` 的实际效果:先实现 `click`(阻止 select 展开)与 `keydown`(阻止编辑框处理)即可,其余类型 prevent 无默认行为可略。
- **测试**:注册/移除(重复注册 no-op 成功;移除后不触发);`dispatch_event` 手动派发 click 收到回调且 target/type 正确;mouse 字段填充正确;prevent_default 后 `default_prevented` 为 1。事件触发的集成测试放 `test_render`(需要窗口+事件循环,参照现有交互测试)。

## 10. 窗口扩展

JS 无对应(平台能力),全部是 SDL3 薄包装,实现于 `src/core/window.cpp`。现有缓存字段 `title/width/height/visible` 与 SDL 查询**以 SDL 为准**:原生窗口已创建(`win->sdl != NULL`)时 getter 优先查 SDL,未创建时用缓存。

| whaleui API | SDL 函数(3.x) | 备注 |
|---|---|---|
| `is_visible` | `SDL_IsWindowVisible` | 未创建时返回缓存 |
| `is_minimized / is_maximized / is_focused` | `SDL_IsWindowMinimized / Maximized / Focused` | |
| `minimize / maximize / restore / raise` | `SDL_MinimizeWindow / MaximizeWindow / RestoreWindow / RaiseWindow` | |
| `get_size_in_pixels` | `SDL_GetWindowSizeInPixels` | 未创建时=缓存尺寸 |
| `set/get_position` | `SDL_SetWindowPosition / SDL_GetWindowPosition` | 屏幕坐标 |
| `center` | `SDL_GetDisplayBounds(SDL_GetWindowDisplayIndex)` | 未创建时先按主显示器/0 号显示器算 |
| `set/get_min_size`、`set/get_max_size` | `SDL_SetWindowMinimumSize / GetWindowMinimumSize` 等 | 0 = 不限制 |
| `set_bordered` | `SDL_SetWindowBordered` | |
| `set_resizable` | `SDL_SetWindowResizable` | |
| `set_always_on_top` | `SDL_SetWindowAlwaysOnTop` | |
| `set/get_opacity` | `SDL_SetWindowOpacity / SDL_GetWindowOpacity` | 默认 1.0 |
| `set_icon` | `SDL_CreateSurfaceFrom(rgba, w, h, 4, w*4)` → `SDL_SetWindowIcon` → `SDL_DestroySurface` | 数据由库拷贝(surface 创建即拷贝) |
| `set_icon_uri` | full: `IMG_Load`(读 VFS 数据);lite: stb_image;minimal: 返回失败 | 复用 `fs` 读整文件再解码 |
| `set/get_fullscreen` | `SDL_SetWindowFullscreen(DESKTOP→SDL_WINDOW_FULLSCREEN_DESKTOP, REAL→SDL_WINDOW_FULLSCREEN, OFF→0)` | getter:`SDL_GetWindowFlags` 判 `SDL_WINDOW_FULLSCREEN*` |
| `set/get_mouse_grab`、`set/get_keyboard_grab` | `SDL_SetWindowMouseGrab / GetWindowMouseGrab` 等 | |
| `set_mouse_position` | `SDL_SetWindowMousePosition` | |
| `set_focusable` | `SDL_SetWindowFocusable` | |
| `set_parent` | `SDL_SetWindowParent` | 平台不支持时返回失败 |
| `set_modal` | `SDL_SetWindowModalFor` | 平台不支持时返回失败 |
| `display_id` | `SDL_GetWindowDisplayIndex` | -1 失败 |
| `display_scale` | `SDL_GetWindowDisplayScale` | 失败返回 1.0 |
| `flash` | `SDL_FlashWindow`(`BRIEFLY→SDL_FLASH_BRIEFLY`, `UNTIL_FOCUSED→SDL_FLASH_UNTIL_FOCUSED`, `CANCEL→SDL_FLASH_CANCEL`) | |

### 窗口事件回调

```c
typedef enum whaleui_window_event { /* 见 whaleui.h */ } whaleui_window_event_t;
typedef int (*whaleui_window_event_cb)(whaleui_window_t* win, whaleui_window_event_t ev, int a, int b, void* userdata);
int whaleui_window_set_event_callback(whaleui_window_t* win, whaleui_window_event_cb cb, void* userdata);
```

- 窗口结构加 `win_event_cb / win_event_ud` 字段;`app.cpp` 事件循环加 `SDL_EVENT_WINDOW_*` 分发分支,映射:
  - `SDL_EVENT_WINDOW_CLOSE_REQUESTED → CLOSE`(**行为变更**:现在该分支直接 `running=0` 并隐藏窗口;改为先调回调,回调返回 0 才执行原默认关闭逻辑,非 0 取消)
  - `RESIZED → RESIZED(a=w,b=h)`(在现有 `SDL_EVENT_WINDOW_RESIZED` 分支内加)
  - `MOVED → MOVED(a=x,b=y)`、`SHOWN/HIDDEN → SHOWN/HIDDEN`、`MINIMIZED → MINIMIZED`、`RESTORED → RESTORED`、`MAXIMIZED → MAXIMIZED`、`FOCUS_GAINED/FOCUS_LOST → FOCUS_GAINED/FOCUS_LOST`、`MOUSE_ENTER/MOUSE_LEAVE → MOUSE_ENTER/MOUSE_LEAVE`
  - SDL 事件名以 SDL3 头文件为准(3.x 已统一为 `SDL_EVENT_WINDOW_*`)。
- 回调返回非 0 仅对 `CLOSE` 有意义(取消默认关闭),其余事件忽略返回值。
- 测试:`set_event_callback` 后模拟(直接调内部分发函数,或真实 resize)断言收到事件与参数;CLOSE 返回非 0 时窗口不关。

---

## 优先级

| 优先级 | 内容 | 理由 |
|---|---|---|
| P0 | §2 查询、§3 遍历、§4 节点操作、§6 属性/classList、§5 title/body/head | 纯 lexbor 包装,工作量小、覆盖 JS 高频用法,先行落地 |
| P0 | §10 窗口扩展(不含事件回调) | SDL 薄包装,立即补齐窗口能力 |
| P1 | §7 innerHTML/value、§8 focus | 需要处理重布局/重绘联动,中等 |
| P1 | §10 窗口事件回调 | 触及现有 CLOSE 行为,改动事件循环 |
| P2 | §9 事件系统 | 最大块,依赖 P0 的查询(hit-test 复用)与 focus;建议独立提交 |
| P2 | §8 getBoundingClientRect | 依赖布局树查询接口,需与 render/layout 协商 |

## 提交建议

- 每完成一个 P0 分组(查询 / 遍历 / 节点操作 / 窗口扩展)提交一次 git,消息按仓库惯例。
- 事件系统单独一个大提交(改动 app.cpp 事件循环,风险最高)。
- 全部完成后更新 `09-implementation.md` 的进度表,并把本文档 `[ ]` 全部勾掉。

## 边界与已知取舍

- **minimal 变体**:无 HTML 解析,§1-§9 的 DOM API 在 minimal 不提供(返回 NULL/失败或编译期裁掉);窗口扩展 §10 全部变体可用。编译期按 `WHALEUI_BUILD_MINIMAL` 裁。
- **事件无冒泡/捕获**:whaleui.h 文档已承诺"target only",未来加冒泡时 `stop_propagation` 语义再落地。
- **list 快照**:不随 DOM 变更更新(与 JS NodeList 的 live 语义不同),文档已写明。
- **value 与渲染同步**:set_value 后编辑框显示需重绘(先做 input/textarea;select 显示已走回调路径,核对)。
- **getBoundingClientRect 的布局依赖**:布局树是重算产物,查询结果以最近一次布局为准;这与 JS 的"强制同步布局"不同,文档已写明边界。
