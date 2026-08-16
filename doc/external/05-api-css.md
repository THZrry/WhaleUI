# CSS API (whaleui_css_*)

样式解析与规则管理。解析器自研(`src/style/css.cpp`),布局位置计算由自研布局引擎完成(见 `../internal/09-implementation.md`)。

## 解析

```c
/* 从字符串解析一组规则 */
int whaleui_css_parse(whaleui_css_rule_t** rules, size_t* count, const char* css, size_t len);

/* 从 URI 加载并解析(经虚拟文件系统) */
int whaleui_css_load(whaleui_css_rule_t** rules, size_t* count, const char* uri);

void whaleui_css_rules_destroy(whaleui_css_rule_t* rules, size_t count);
```

## 选择器与声明访问

```c
const char* whaleui_css_selector(const whaleui_css_rule_t* rule);
const char* whaleui_css_get_property(const whaleui_css_rule_t* rule, const char* name);
int         whaleui_css_has_property(const whaleui_css_rule_t* rule, const char* name);
```

## 应用

```c
/* 将一组规则应用到文档(内部按选择器匹配元素) */
int whaleui_css_apply(whaleui_dom_document_t* doc,
                      const whaleui_css_rule_t* rules, size_t count);
```

## 用例

```c
whaleui_css_rule_t* rules = NULL;
size_t count = 0;
whaleui_css_load(&rules, &count, "file://themes/fluent-dark.css");
whaleui_css_apply(doc, rules, count);
```

## 支持范围

按 `../internal/css-priority.md` 三档实现:

| 档位 | 内容 |
|------|------|
| 最常用 | color, background, margin, padding, border, display, width, height, font-size, font-weight, text-align, line-height, position, top/left/right/bottom, z-index, flex, justify-content, align-items, gap, opacity, cursor, overflow, box-sizing |
| 必须 | font-family, text-decoration, border-radius, box-shadow, transform, transition, animation, flex-direction, grid-template-columns, var, media, keyframes 等(详见 css-priority.md) |
| 最好 | font-style, text-shadow, filter, aspect-ratio 等 |

- 各属性(含不兼容项)的实际支持状态与用例见 [html-css-support.md](html-css-support.md)。
- 主题热切换 = 换一组已解析规则重新 apply。
