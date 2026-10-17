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
/* 把一组规则挂到文档上(替换上一次挂的),下一帧生效 */
int whaleui_css_apply(whaleui_dom_document_t* doc,
                      const whaleui_css_rule_t* rules, size_t count);
```

规则被**深拷贝**为该文档自有的样式表,并**追加**在文档的
`<style>`/`<link rel=stylesheet>` 之后参与同一次级联——所以同特异度时后挂的
胜出,与浏览器里"后插入的样式表赢"一致。`count == 0` 清空该表(换整套主题表
的用法)。`rules` 数组在调用返回后即可销毁,文档持有自己的副本。

挂表会标记该文档的样式表为陈旧,下一帧自动重收并重排/重绘——与 DOM 契约一致
("改动在下一次重绘时生效"),不需要额外调用任何刷新函数。

## 用例

```c
whaleui_css_rule_t* rules = NULL;
size_t count = 0;
whaleui_css_load(&rules, &count, "file://themes/fluent-dark.css");
whaleui_css_apply(doc, rules, count);
whaleui_css_rules_destroy(rules, count);   /* 文档已持有副本 */
```

C++ 侧用 `whaleui::StyleSheet`(RAII):

```cpp
whaleui::StyleSheet sheet;
sheet.Load("file://themes/fluent-dark.css");
sheet.Apply(doc);
```

## 样式表来源的改动会自动重收

文档的样式表来源是 `<head>`/`<body>` 里的 `<style>` 与
`<link rel=stylesheet>`(浏览器允许 `<style>` 出现在 body 里,引擎同样读取)。
任何一条路径改动它,引擎都会在下一帧重收:

- 追加 / 删除 / 移动 `<style>`、`<link>`(含 `insertBefore`、`innerHTML`);
- 改写 `<style>` 的文本、改它的属性(如 `media`);
- 换 `<link href>`;
- `whaleui_css_apply`。

普通文本 / class / 普通属性编辑**不会**触发重收(否则每次按键都要重解析整张
表)。`<style media="print">` 这类元素的 media 属性会被当成 `@media` 条件,
由与 CSS 内联 `@media` 相同的求值器判定。

## 支持范围

按 `../internal/css-priority.md` 三档实现:

| 档位 | 内容 |
|------|------|
| 最常用 | color, background, margin, padding, border, display, width, height, font-size, font-weight, text-align, line-height, position, top/left/right/bottom, z-index, flex, justify-content, align-items, gap, opacity, cursor, overflow, box-sizing |
| 必须 | font-family, text-decoration, border-radius, box-shadow, transform, transition, animation, flex-direction, grid-template-columns, var, media, keyframes 等(详见 css-priority.md) |
| 最好 | font-style, text-shadow, filter, aspect-ratio 等 |

- 各属性(含不兼容项)的实际支持状态与用例见 [html-css-support.md](html-css-support.md)。
- 主题热切换 = 换一组已解析规则重新 apply(或直接
  `whaleui_app_set_theme_style`)。
## CSS.supports

```c
int whaleui_css_supports(const char* property, const char* value);
```

属性是否实现查显式属性表,值是否合法用引擎自己的解析器(颜色/长度/枚举关键字)
判定;`var()`/`calc()`/`gradient()` 等延迟解析的值一律放行。详见
[09-api-web.md](09-api-web.md#csssupports)。

- 各属性(含不兼容项)的实际支持状态与用例见 [html-css-support.md](html-css-support.md)。
- 主题热切换 = 换一组已解析规则重新 apply(或直接 `whaleui_app_set_theme_style`)。
