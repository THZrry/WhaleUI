# DOM API (whaleui_dom_*)

与 JavaScript DOM 对应,覆盖 HTML 解析、节点查询、修改、属性、样式、事件与几何。元素句柄即 lexbor 节点,随所属文档存活;文档销毁后全部句柄失效。

> 本文档与 `include/whaleui.h` 一致。未实现部分见 `../internal/more-dom-api.md` 交接清单。

## 解析

```c
whaleui_dom_document_t* whaleui_dom_parse_html(const char* html, size_t len); /* len=0 视为 NUL 结尾 */
void whaleui_dom_document_destroy(whaleui_dom_document_t* doc);
```

## 元素集合(查询返回值)

```c
typedef struct whaleui_dom_list whaleui_dom_list_t;

size_t whaleui_dom_list_length(const whaleui_dom_list_t* list);
whaleui_dom_element_t* whaleui_dom_list_item(const whaleui_dom_list_t* list, size_t index);
void whaleui_dom_list_destroy(whaleui_dom_list_t* list);
```

快照语义:不随 DOM 变更更新;空结果为非 NULL 的长度 0 集合。

## 文档级查询(对应 document.*)

```c
whaleui_dom_element_t* whaleui_dom_get_element_by_id(whaleui_dom_document_t* doc, const char* id);
whaleui_dom_element_t* whaleui_dom_query_selector(whaleui_dom_document_t* doc, const char* selector);
whaleui_dom_list_t*    whaleui_dom_query_selector_all(whaleui_dom_document_t* doc, const char* selector);
whaleui_dom_list_t*    whaleui_dom_get_elements_by_class_name(whaleui_dom_document_t* doc, const char* class_name);
whaleui_dom_list_t*    whaleui_dom_get_elements_by_tag_name(whaleui_dom_document_t* doc, const char* tag);

/* 结构访问 */
whaleui_dom_element_t* whaleui_dom_document_element(whaleui_dom_document_t* doc); /* <html> */
whaleui_dom_element_t* whaleui_dom_body(whaleui_dom_document_t* doc);
whaleui_dom_element_t* whaleui_dom_head(whaleui_dom_document_t* doc);

/* document.title */
int whaleui_dom_set_title(whaleui_dom_document_t* doc, const char* title);
const char* whaleui_dom_get_title(whaleui_dom_document_t* doc);

/* 当前聚焦元素(无则 NULL) */
whaleui_dom_element_t* whaleui_dom_active_element(whaleui_dom_document_t* doc);
```

## 元素级查询(对应 element.*)

```c
whaleui_dom_element_t* whaleui_dom_element_query_selector(whaleui_dom_element_t* el, const char* selector);
whaleui_dom_list_t*    whaleui_dom_element_query_selector_all(whaleui_dom_element_t* el, const char* selector);
whaleui_dom_list_t*    whaleui_dom_element_get_elements_by_class_name(whaleui_dom_element_t* el, const char* class_name);
whaleui_dom_list_t*    whaleui_dom_element_get_elements_by_tag_name(whaleui_dom_element_t* el, const char* tag);

whaleui_dom_element_t* whaleui_dom_closest(whaleui_dom_element_t* el, const char* selector); /* 含自身 */
int whaleui_dom_matches(whaleui_dom_element_t* el, const char* selector);                   /* 1=匹配 */
```

## 树遍历(对应 element.*)

```c
whaleui_dom_element_t* whaleui_dom_parent(whaleui_dom_element_t* el);          /* parentNode */
whaleui_dom_element_t* whaleui_dom_parent_element(whaleui_dom_element_t* el);  /* parentElement */
whaleui_dom_element_t* whaleui_dom_first_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_last_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_next_sibling(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_previous_sibling(whaleui_dom_element_t* el);

/* 仅元素(跳过文本/注释节点) */
whaleui_dom_element_t* whaleui_dom_first_element_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_last_element_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_next_element_sibling(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_previous_element_sibling(whaleui_dom_element_t* el);

whaleui_dom_list_t* whaleui_dom_children(whaleui_dom_element_t* el);           /* element.children */
size_t whaleui_dom_child_element_count(whaleui_dom_element_t* el);             /* childElementCount */
```

## 修改(对应 element.appendChild 等)

```c
whaleui_dom_element_t* whaleui_dom_create_element(whaleui_dom_document_t* doc, const char* tag);
whaleui_dom_element_t* whaleui_dom_create_text_node(whaleui_dom_document_t* doc, const char* text);

int whaleui_dom_append_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);
int whaleui_dom_remove_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);
int whaleui_dom_insert_before(whaleui_dom_element_t* parent,
                              whaleui_dom_element_t* new_child,
                              whaleui_dom_element_t* ref_child); /* ref 为 NULL 追加 */
int whaleui_dom_replace_child(whaleui_dom_element_t* parent,
                              whaleui_dom_element_t* new_child,
                              whaleui_dom_element_t* old_child);
int whaleui_dom_remove(whaleui_dom_element_t* el); /* 摘除,元素本身仍存活可再挂 */

/* element.append/prepend/before/after(单节点形式) */
int whaleui_dom_append(whaleui_dom_element_t* el, whaleui_dom_element_t* node);
int whaleui_dom_prepend(whaleui_dom_element_t* el, whaleui_dom_element_t* node);
int whaleui_dom_before(whaleui_dom_element_t* el, whaleui_dom_element_t* node);
int whaleui_dom_after(whaleui_dom_element_t* el, whaleui_dom_element_t* node);

/* element.insertAdjacentHTML(position, html):
 * position = beforebegin|afterbegin|beforeend|afterend */
int whaleui_dom_insert_adjacent_html(whaleui_dom_element_t* el,
                                     const char* position, const char* html);

/* element.insertAdjacentText(position, text):同 insertAdjacentHTML 的四个位置,
 * 但把文本作为单个文本节点插入(不解析 HTML) */
int whaleui_dom_insert_adjacent_text(whaleui_dom_element_t* el,
                                     const char* position, const char* text);

/* document.createDocumentFragment():脱离文档的容器节点;把它 append/insert 进
 * 树时移入的是它的子节点(浏览器语义),fragment 自身变空。用完 destroy。 */
whaleui_dom_element_t* whaleui_dom_create_document_fragment(whaleui_dom_document_t* doc);

whaleui_dom_element_t* whaleui_dom_clone(whaleui_dom_element_t* el, int deep); /* deep=1 深克隆 */
int whaleui_dom_contains(whaleui_dom_element_t* parent, whaleui_dom_element_t* child); /* 含自身 */
int whaleui_dom_has_child_nodes(whaleui_dom_element_t* el);
int whaleui_dom_is_connected(whaleui_dom_element_t* el);

int whaleui_dom_element_destroy(whaleui_dom_element_t* el);
```

## 属性(对应 setAttribute / getAttribute 等)

```c
int  whaleui_dom_set_attribute(whaleui_dom_element_t* el, const char* name, const char* value);
const char* whaleui_dom_get_attribute(whaleui_dom_element_t* el, const char* name);
int  whaleui_dom_has_attribute(whaleui_dom_element_t* el, const char* name);
int  whaleui_dom_remove_attribute(whaleui_dom_element_t* el, const char* name);
int  whaleui_dom_toggle_attribute(whaleui_dom_element_t* el, const char* name, int force);
     /* force>=0 强制 开(1)/关(0),force<0 翻转;返回最终存在性(1/0),错误 -1 */

/* classList */
int whaleui_dom_class_add(whaleui_dom_element_t* el, const char* cls);
int whaleui_dom_class_remove(whaleui_dom_element_t* el, const char* cls);
int whaleui_dom_class_toggle(whaleui_dom_element_t* el, const char* cls);  /* 返回最终存在性 */
int whaleui_dom_class_contains(whaleui_dom_element_t* el, const char* cls);
/* classList.length / item(index) / replace(old, new) / toggle(cls, force) */
size_t whaleui_dom_class_length(whaleui_dom_element_t* el);
int whaleui_dom_class_item(whaleui_dom_element_t* el, size_t index, char* out, size_t cap);
int whaleui_dom_class_replace(whaleui_dom_element_t* el, const char* old_cls, const char* new_cls);
int whaleui_dom_class_toggle_force(whaleui_dom_element_t* el, const char* cls, int force);
/* force>=0 强制开(1)/关(0),force<0 翻转;返回最终存在性 */
```

## dataset(对应 element.dataset.*)

```c
int whaleui_dom_dataset_set(whaleui_dom_element_t* el, const char* key, const char* value);
const char* whaleui_dom_dataset_get(whaleui_dom_element_t* el, const char* key);
int whaleui_dom_dataset_has(whaleui_dom_element_t* el, const char* key);
int whaleui_dom_dataset_remove(whaleui_dom_element_t* el, const char* key);
```

camelCase 键 <-> `data-kebab-case` 属性(`fooBar` <-> `data-foo-bar`)。

## 文本与 HTML 内容

```c
/* textContent */
int whaleui_dom_set_text(whaleui_dom_element_t* el, const char* text);
const char* whaleui_dom_get_text(whaleui_dom_element_t* el);

/* innerHTML:set 解析片段并替换子节点 */
int whaleui_dom_set_inner_html(whaleui_dom_element_t* el, const char* html);
const char* whaleui_dom_get_inner_html(whaleui_dom_element_t* el);

/* outerHTML:set 替换元素自身 */
int whaleui_dom_set_outer_html(whaleui_dom_element_t* el, const char* html);
const char* whaleui_dom_get_outer_html(whaleui_dom_element_t* el);
```

## 表单值(对应 .value 属性)

```c
int whaleui_dom_set_value(whaleui_dom_element_t* el, const char* value); /* input/textarea/select */
const char* whaleui_dom_get_value(whaleui_dom_element_t* el);            /* 其他元素返回 NULL */
```

## 内联样式(对应 el.style.xxx / CSSStyleDeclaration)

```c
int whaleui_dom_set_style(whaleui_dom_element_t* el, const char* property, const char* value);
const char* whaleui_dom_get_style(whaleui_dom_element_t* el, const char* property);

/* setProperty / getPropertyValue / removeProperty */
int whaleui_dom_style_set_property(whaleui_dom_element_t* el, const char* property, const char* value);
int whaleui_dom_style_get_property(whaleui_dom_element_t* el, const char* property, char* out, size_t cap);
int whaleui_dom_style_remove_property(whaleui_dom_element_t* el, const char* property);

/* cssText:整段内联样式读写("color: red; margin: 0") */
int whaleui_dom_style_get_css_text(whaleui_dom_element_t* el, char* out, size_t cap);
int whaleui_dom_style_set_css_text(whaleui_dom_element_t* el, const char* css_text);

/* length / item(index):声明个数 + 第 index 个属性名 */
size_t whaleui_dom_style_length(whaleui_dom_element_t* el);
int whaleui_dom_style_item(whaleui_dom_element_t* el, size_t index, char* out, size_t cap);
```

改动内联样式会标记脏并在下一帧重排/重绘;含 `--var` 定义时还会重收变量。

## 计算样式(对应 getComputedStyle,快照语义)

```c
typedef struct whaleui_style_declaration whaleui_style_declaration_t;
whaleui_style_declaration_t* whaleui_dom_get_computed_style(whaleui_dom_element_t* el, const char* pseudo);
void whaleui_style_declaration_destroy(whaleui_style_declaration_t* decl);
size_t whaleui_style_declaration_length(const whaleui_style_declaration_t* decl);
int whaleui_style_declaration_item(const whaleui_style_declaration_t* decl, size_t index, char* out, size_t cap);
int whaleui_style_declaration_get(const whaleui_style_declaration_t* decl, const char* property, char* out, size_t cap);
```

有布局结果时返回级联后的快照(含 stylesheet + 内联样式),否则回退为内联样式。
快照与后续 DOM 变更无关,用完必须 `destroy`。`pseudo` 保留(`::before/::after`
后续支持),当前忽略。

## 标签名

```c
const char* whaleui_dom_tag_name(whaleui_dom_element_t* el); /* 小写:"input"、"div" ... */
```

## 焦点

```c
int whaleui_dom_focus(whaleui_dom_element_t* el); /* 对应 el.focus() */
int whaleui_dom_blur(whaleui_dom_element_t* el);  /* 对应 el.blur() */
```

focus()/blur() 移动 `document.activeElement` 镜像并派发非冒泡的 focus/blur(失焦元素先 blur,新元素后 focus;blur() 仅在当前激活元素上触发)。`:focus` 样式跟随引擎焦点(点击 / Tab),渲染窗口上两者下一帧收敛。

## 几何(对应 getBoundingClientRect)

```c
typedef struct whaleui_dom_rect {
    float x, y;        /* 左上角,viewport 坐标 */
    float width, height;
} whaleui_dom_rect_t;

int whaleui_dom_get_bounding_client_rect(whaleui_dom_element_t* el, whaleui_dom_rect_t* out);
/* 数据来自最近一次布局;未渲染过返回失败 */
```

## 事件(对应 addEventListener 等)

```c
typedef struct whaleui_dom_event whaleui_dom_event_t;
typedef void (*whaleui_event_cb)(whaleui_dom_event_t* ev, void* userdata);

int whaleui_dom_add_event_listener(whaleui_dom_element_t* el, const char* type,
                                   whaleui_event_cb cb, void* userdata);
int whaleui_dom_add_event_listener_ex(whaleui_dom_element_t* el, const char* type,
                                      whaleui_event_cb cb, void* userdata, int capture);
int whaleui_dom_remove_event_listener(whaleui_dom_element_t* el, const char* type,
                                      whaleui_event_cb cb, void* userdata);
int whaleui_dom_dispatch_event(whaleui_dom_element_t* el, const char* type); /* 同步派发 */

/* 事件对象访问器(回调内有效) */
whaleui_dom_element_t* whaleui_dom_event_target(const whaleui_dom_event_t* ev);
whaleui_dom_element_t* whaleui_dom_event_current_target(const whaleui_dom_event_t* ev);
int whaleui_dom_event_phase(const whaleui_dom_event_t* ev); /* 1捕获 2目标 3冒泡 */
const char* whaleui_dom_event_type(const whaleui_dom_event_t* ev);
int whaleui_dom_event_prevent_default(whaleui_dom_event_t* ev);
int whaleui_dom_event_stop_propagation(whaleui_dom_event_t* ev);
int whaleui_dom_event_default_prevented(const whaleui_dom_event_t* ev);

int   whaleui_dom_event_key_code(const whaleui_dom_event_t* ev);          /* WHALEUI_KEY_* */
int   whaleui_dom_event_mouse_x(const whaleui_dom_event_t* ev);           /* viewport 坐标 */
int   whaleui_dom_event_mouse_y(const whaleui_dom_event_t* ev);
int   whaleui_dom_event_mouse_button(const whaleui_dom_event_t* ev);      /* 0=左 1=中 2=右 */
float whaleui_dom_event_wheel_delta_x(const whaleui_dom_event_t* ev);
float whaleui_dom_event_wheel_delta_y(const whaleui_dom_event_t* ev);
```

支持的事件类型:`click, dblclick, mousedown, mouseup, mousemove, mouseenter, mouseleave, contextmenu, wheel, keydown, keyup, focus, blur, change, input, submit, load, scroll, resize`。

派发模型:DOM 三阶段(捕获自根向下 → 目标 → 冒泡向上);`mouseenter/mouseleave/focus/blur` 为非冒泡事件(仅捕获+目标)。`stopPropagation()` 中断派发,`preventDefault()` 取消引擎默认行为(mousedown 取消按压链、keydown 取消编辑键、wheel 取消滚动)。事件循环已接入全部类型,含 submit(单行输入在 `<form>` 内按 Enter,向 form 派发;引擎不做表单导航)。

## 用例

```c
whaleui_dom_document_t* doc = whaleui_dom_parse_html(html, len);
whaleui_dom_element_t* box = whaleui_dom_get_element_by_id(doc, "box");
whaleui_dom_set_attribute(box, "class", "theme-card");
whaleui_dom_set_style(box, "width", "100px");
whaleui_dom_set_text(box, "hello");

/* 查询 + 事件 */
whaleui_dom_list_t* cards = whaleui_dom_query_selector_all(doc, ".theme-card");
for (size_t i = 0; i < whaleui_dom_list_length(cards); ++i) {
    whaleui_dom_element_t* c = whaleui_dom_list_item(cards, i);
    whaleui_dom_add_event_listener(c, "click", [](whaleui_dom_event_t* ev, void*) {
        whaleui_dom_element_t* t = whaleui_dom_event_target(ev);
        whaleui_dom_class_add(t, "active");
    }, nullptr);
}
whaleui_dom_list_destroy(cards);

whaleui_dom_rect_t r;
if (whaleui_dom_get_bounding_client_rect(box, &r) == 0) {
    /* r.x, r.y, r.width, r.height */
}
```

## 实现说明

- 元素句柄直接包装 lexbor 节点;tag→div 的适配由 UA 默认样式 + `tag_id` 组件完成(见 `../internal/09-implementation.md`)。
- 文档销毁时释放其下所有元素;元素句柄在文档销毁后失效。
- 选择器能力与样式级联同一语义(`whaleui_style_match`),与渲染层一致的部分见 `../internal/09-implementation.md` 的 CSS 支持矩阵。
