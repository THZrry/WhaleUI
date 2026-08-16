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
```

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

## 内联样式(对应 el.style.xxx)

```c
int whaleui_dom_set_style(whaleui_dom_element_t* el, const char* property, const char* value);
const char* whaleui_dom_get_style(whaleui_dom_element_t* el, const char* property);
```

## 标签名

```c
const char* whaleui_dom_tag_name(whaleui_dom_element_t* el); /* 小写:"input"、"div" ... */
```

## 焦点

```c
int whaleui_dom_focus(whaleui_dom_element_t* el); /* 对应 el.focus() */
int whaleui_dom_blur(whaleui_dom_element_t* el);  /* 对应 el.blur() */
```

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
int whaleui_dom_remove_event_listener(whaleui_dom_element_t* el, const char* type,
                                      whaleui_event_cb cb, void* userdata);
int whaleui_dom_dispatch_event(whaleui_dom_element_t* el, const char* type); /* 同步派发 */

/* 事件对象访问器(回调内有效) */
whaleui_dom_element_t* whaleui_dom_event_target(const whaleui_dom_event_t* ev);
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

派发模型:仅在目标元素触发(暂无冒泡/捕获);`stop_propagation` 为预留接口。

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

- 元素句柄直接包装 lexbor 节点;所有 tag 统一视为 div 的内部表示在第三步实现。
- 文档销毁时释放其下所有元素;元素句柄在文档销毁后失效。
- 选择器能力与渲染层一致的部分见 `../internal/09-implementation.md` 的 CSS 支持矩阵。
