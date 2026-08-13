# DOM API (whaleui_dom_*)

与 JavaScript DOM 对应,覆盖 HTML 解析、节点查询、修改、属性与样式。

## 解析

```c
whaleui_dom_document_t* whaleui_dom_parse_html(const char* html, size_t len);
void whaleui_dom_document_destroy(whaleui_dom_document_t* doc);
```

## 查询(对应 document.* / element.*)

```c
whaleui_dom_element_t* whaleui_dom_get_element_by_id(whaleui_dom_document_t* doc,
                                                     const char* id);
whaleui_dom_element_t* whaleui_dom_query_selector(whaleui_dom_document_t* doc,
                                                  const char* selector);
whaleui_dom_element_t* whaleui_dom_document_element(whaleui_dom_document_t* doc);
whaleui_dom_element_t* whaleui_dom_parent(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_first_child(whaleui_dom_element_t* el);
whaleui_dom_element_t* whaleui_dom_next_sibling(whaleui_dom_element_t* el);
```

## 修改(对应 element.appendChild 等)

```c
whaleui_dom_element_t* whaleui_dom_create_element(whaleui_dom_document_t* doc,
                                                  const char* tag);
int whaleui_dom_append_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);
int whaleui_dom_remove_child(whaleui_dom_element_t* parent, whaleui_dom_element_t* child);
int whaleui_dom_element_destroy(whaleui_dom_element_t* el);
```

## 属性(对应 setAttribute / getAttribute)

```c
int  whaleui_dom_set_attribute(whaleui_dom_element_t* el, const char* name, const char* value);
const char* whaleui_dom_get_attribute(whaleui_dom_element_t* el, const char* name);
```

## 文本(对应 textContent)

```c
int whaleui_dom_set_text(whaleui_dom_element_t* el, const char* text);
const char* whaleui_dom_get_text(whaleui_dom_element_t* el);
```

## 内联样式(对应 el.style.xxx)

```c
int whaleui_dom_set_style(whaleui_dom_element_t* el, const char* property, const char* value);
const char* whaleui_dom_get_style(whaleui_dom_element_t* el, const char* property);
```

## 标签名

```c
const char* whaleui_dom_tag_name(whaleui_dom_element_t* el);
```

## 用例

```c
whaleui_dom_document_t* doc = whaleui_dom_parse_html(html, len);
whaleui_dom_element_t* box = whaleui_dom_get_element_by_id(doc, "box");
whaleui_dom_set_attribute(box, "class", "theme-card");
whaleui_dom_set_style(box, "width", "100px");
whaleui_dom_set_text(box, "hello");
```

## 内部表示说明

- 元素句柄直接包装 lexbor 节点;所有 tag 统一视为 div 的内部表示在第三步实现。
- 文档销毁时释放其下所有元素;元素句柄在文档销毁后失效。
