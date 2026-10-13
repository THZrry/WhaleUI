# more-dom-api — 待实现 API 交接清单

> 读者:下一个实现 Agent(第三步续作)。本文档列出 `include/whaleui.h` 中**已文档化但尚未实现**的公开 API。
> **签名已在 whaleui.h 冻结,不要改签名**——实现前先通读 `09-implementation.md`(架构修正、事件循环、已知限制)与对应模块源码。
> 已完成部分直接照旧;每实现一个 API,把下方清单的 `[ ]` 勾掉并跑对应测试。
>
> 2026-10-13 更新:§10 窗口扩展已全部实现并合入(de48f1c:SDL 全量薄包装 +
> HitTest + per-window 事件回调 + 多窗口路由修复;2cea474:per-window key/select
> 回调 + 窗口事件分发);Style 声明式 API 已实现(2cea474);Canvas 2D 核心 +
> WebGPU stub 已实现(614740a)。下方 §10 全部勾选,§6/§9 部分更新,剩余见各节标注。

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

## 9. 事件系统 ✅ 已全接入(2026-10-13)

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

### 设计(已按此实现;2026-10-13 更新)

- **派发模型:DOM 三阶段**(捕获自根向下 → 目标 → 冒泡向上),`whaleui_dom_dispatch_phased`;
  `mouseenter/mouseleave/focus/blur` 走 `whaleui_dom_dispatch_nonbubble`(捕获+目标,无冒泡,浏览器语义)。
  `stopPropagation` 中断派发,`preventDefault` 上报 out_prevented 供引擎跳过默认行为。
- **事件对象是派发期间的瞬态对象**:堆分配、回调结束 delete;访问器读字段;`prevent_default` 置标志位。
- listener 表:`src/dom/events.cpp` 的 side map(`map<element*, vector<Listener>>`);`whaleui_events_clear_element`(元素销毁)与 `whaleui_events_clear_all`(文档销毁)清理。
- `dispatch_event`:三阶段派发;`mousedown` 的 prevent_default 取消按压链(focus/:active/选择),`keydown` 的取消编辑键,`wheel` 的取消滚动。

### 接入点(全部已接,`src/core/app.cpp` 的 `process_event` / `src/render/render.cpp` 内派发宏 `WUI_DOM_FIRE[_NB]`)

| 事件类型 | 接入 | 状态 |
|---|---|---|
| mousedown/mouseup/click/dblclick(左)/contextmenu(右) | `SDL_EVENT_MOUSE_BUTTON_DOWN/UP` | ✅ dblclick 在第二次 click 后触发;button 映射 0=左 1=中 2=右 |
| mousemove/mouseenter/mouseleave | `render_set_hover` 帧末命中测试 | ✅ enter/leave 在非冒泡派发,hover 变化与出窗(clear_hover)时触发 |
| wheel | `render_handle_wheel` 头部派发,preventDefault 跳过滚动 | ✅ |
| keydown/keyup | 聚焦元素;keydown 先行派发,preventDefault 取消 `render_handle_key` | ✅ |
| focus/blur | `set_pressed_ex` 焦点变化点 + 滚动条拖拽焦点 | ✅ 非冒泡 |
| input/change | `edit_replace`(input + change 脏标记)、失焦/Enter 提交 change、select 换选项、checkbox/radio 翻转 | ✅ |
| load | `window_load_html_len` 成功后在 documentElement 上派发 | ✅ |
| scroll | `do_scroll` 实际位移后在滚动容器上派发(冒泡) | ✅ |
| resize | `SDL_EVENT_WINDOW_RESIZED` 分支在 documentElement 上派发 | ✅ |
| submit | 表单提交逻辑未做,不派发(可手动 dispatch) | ⬜ |

- **测试**:三阶段顺序/stopPropagation/非冒泡契约/8 个新接入类型在 `tests/test_dom.cpp` 覆盖;C++ 侧在 `tests/test_cpp.cpp`。

## 10. 窗口扩展 ✅ 已全部实现(de48f1c + 2cea474)

JS 无对应(平台能力),全部是 SDL3 薄包装,实现于 `src/core/window.cpp`。现有缓存字段 `title/width/height/visible` 与 SDL 查询**以 SDL 为准**:原生窗口已创建(`win->sdl != NULL`)时 getter 优先查 SDL,未创建时用缓存。

| whaleui API | SDL 函数(3.x) | 状态 |
|---|---|---|
| `is_visible` | `SDL_GetWindowFlags & HIDDEN` | ✅ |
| `is_minimized / is_maximized / is_focused` | `SDL_GetWindowFlags` | ✅ |
| `minimize / maximize / restore / raise` | `SDL_Minimize/Maximize/Restore/RaiseWindow` | ✅ |
| `get_size_in_pixels` | `SDL_GetWindowSizeInPixels` | ✅ |
| `set/get_position` | `SDL_Set/GetWindowPosition` | ✅ |
| `center` | `SDL_GetDisplayBounds + primary fallback` | ✅ |
| `set/get_min_size`、`set/get_max_size` | `SDL_Set/GetWindowMinimum/MaximumSize` | ✅ |
| `set_bordered` | `SDL_SetWindowBordered`(+无边框默认 HitTest) | ✅ |
| `set_resizable` | `SDL_SetWindowResizable`(+创建标志) | ✅ |
| `set_always_on_top` | `SDL_SetWindowAlwaysOnTop`(+创建标志) | ✅ |
| `set/get_opacity` | `SDL_Set/GetWindowOpacity` | ✅ |
| `set_icon` | `SDL_CreateSurfaceFrom + SetWindowIcon` | ✅ |
| `set_icon_uri` | VFS 读文件;Full 走 SDL_image 路径,lite/minimal 走 stb,minimal 图标 uri 失败干净 | ✅ |
| `set/get_fullscreen` | `SDL_SetWindowFullscreen(bool)` + flags 判 | ✅(DESKTOP/REAL 均走 fullscreen=true,SDL3 无 exclusive 区分) |
| `set/get_mouse_grab`、`set/get_keyboard_grab` | `SDL_Set/GetWindowMouse/KeyboardGrab` | ✅ |
| `set_mouse_position` | `SDL_WarpMouseInWindow`(SDL3 无 SetWindowMousePosition) | ✅ |
| `set_focusable` | `SDL_SetWindowFocusable`(+创建标志 NOT_FOCUSABLE) | ✅ |
| `set_parent` | `SDL_SetWindowParent` | ✅ |
| `set_modal` | `SDL_SetWindowModal + SetWindowParent` | ✅ |
| `display_id` | `SDL_GetDisplayForWindow` | ✅ |
| `display_scale` | `SDL_GetWindowDisplayScale` | ✅ |
| `flash` | `SDL_FlashWindow` | ✅ |
| `set_hit_test`(新增) | `SDL_SetWindowHitTest` | ✅(默认无边框实现见下) |

差异记录:SDL3 的 `SDL_SetWindowFullscreen` 只有 bool(无 DESKTOP/REAL 区分),
`set_mouse_position` 对应 `SDL_WarpMouseInWindow`;`set_modal` 需配合
`SetWindowParent`。`set_icon_uri` 的 Full 路径当前经 VFS + stb 回退,SDL_image
直解为后续优化项,不影响契约。

### 窗口事件回调

```c
typedef enum whaleui_window_event { /* 见 whaleui.h */ } whaleui_window_event_t;
typedef int (*whaleui_window_event_cb)(whaleui_window_t* win, whaleui_window_event_t ev, int a, int b, void* userdata);
int whaleui_window_set_event_callback(whaleui_window_t* win, whaleui_window_event_cb cb, void* userdata);
```

- ✅ 窗口结构已加 `win_event_cb / win_event_ud` + `key_cb/ud` + `select_cb/ud` 字段;`app.cpp` 事件循环已加分发:
  - `CLOSE_REQUESTED → CLOSE`(先调回调,非 0 取消默认关闭)✅
  - `RESIZED/MOVED/SHOWN/HIDDEN/MINIMIZED/RESTORED/MAXIMIZED/FOCUS_GAINED/FOCUS_LOST/MOUSE_ENTER/MOUSE_LEAVE` ✅
  - key/select 按窗口优先、应用兜底 ✅
  - `app_resize_watch` 按 WindowID 路由 ✅,per-window `display_refresh` ✅
- 回调返回非 0 仅对 `CLOSE` 有意义(取消默认关闭),其余事件忽略返回值。
- 测试:`tests/test_window.cpp` 已覆盖 setter 契约 + headless 缓存路径;CLOSE 取消的集成路径已在 `process_event` 实现。

### HitTest(新增,无边框窗口)

- `whaleui_window_set_hit_test` + 默认实现(8px 边缘缩放 + 顶部 32px 拖拽);
  `set_bordered(0)` 自动安装默认、`set_bordered(1)` 自动拆除默认(自定义不动)。
- 测试:`test_window` 覆盖 setter;手动验收待补(无边框拖拽需真窗口)。

---

## 优先级

| 优先级 | 内容 | 状态 |
|---|---|---|
| P0 | §2 查询、§3 遍历、§4 节点操作、§6 属性/classList、§5 title/body/head | ✅ 基线已实现;选择器全量透传待做 |
| P0 | §10 窗口扩展(不含事件回调) | ✅ de48f1c |
| P0 | Style 声明式(cssText/getComputedStyle) | ✅ 2cea474 |
| P1 | §7 innerHTML/value、§8 focus | ✅ 基线已实现 |
| P1 | §10 窗口事件回调 + HitTest | ✅ de48f1c + 2cea474 |
| P1 | Canvas 2D 核心 + WebGPU stub | ✅ 614740a |
| P2 | §9 事件系统剩余(mousemove/enter/leave/wheel/focus/blur/input/change/scroll 全接入) | ⬜ 待做 |
| P2 | §8 getBoundingClientRect | ✅ 基线已实现(依赖最近布局) |
| P2 | 选择器全量透传(`> + ~ [attr] :pseudo`) | ✅ DOM 查询已切 `whaleui_style_match` 语义(含逗号分支);元素级修正为后代语义 |

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
