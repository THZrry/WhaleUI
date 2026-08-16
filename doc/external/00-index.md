# 外部文档(调用手册)索引

> 本目录是 **面向 WhaleUI 使用者的调用文档**:API 手册、标准支持情况与用例。
> 内部实现细节(架构、实现状态、ECS、交接清单)见 [`doc/internal/`](../internal/)。

## 快速上手(综合用例)

一个完整的最小应用:创建应用 → 建窗口 → 加载 HTML → 注册事件 → 运行。

```c
#include "whaleui.h"

int main(void) {
    whaleui_app_t* app = whaleui_app_create();
    whaleui_app_set_theme(app, WHALEUI_THEME_DARK);   /* 深浅色,默认跟随系统 */

    whaleui_window_t* win = whaleui_window_create(app, "Hello", 800, 600);
    whaleui_window_load_html(win,
        "<html><body>"
        "<div class='card' id='card'>"
        "  <h1>Hello, WhaleUI</h1>"
        "  <p>点击按钮切换背景色</p>"
        "  <button id='btn'>Switch</button>"
        "</div>"
        "</body></html>");
    whaleui_window_show(win);

    /* 事件监听(目标元素派发);doc 经 userdata 传入 */
    whaleui_dom_document_t* doc = whaleui_window_get_document(win);
    whaleui_dom_element_t* btn = whaleui_dom_get_element_by_id(doc, "btn");
    whaleui_dom_add_event_listener(btn, "click", [](whaleui_dom_event_t* ev, void* ud) {
        whaleui_dom_document_t* d = (whaleui_dom_document_t*)ud;
        whaleui_dom_element_t* card = whaleui_dom_get_element_by_id(d, "card");
        whaleui_dom_class_toggle(card, "active");
    }, doc);

    whaleui_app_run(app);          /* 阻塞事件循环,T 切换深浅色,ESC 退出 */

    whaleui_window_destroy(win);
    whaleui_app_destroy(app);
    return 0;
}
```

## API 手册(按模块)

| 文档 | 内容 |
|------|------|
| [02-api-app.md](02-api-app.md) | 应用 API:`whaleui_app_*`(生命周期 / 主题 / 渲染选项 / 构建变体) |
| [03-api-window.md](03-api-window.md) | 窗口 API:`whaleui_window_*`(创建 / 几何 / 外观 / 全屏 / 事件回调 / 内容加载) |
| [04-api-dom.md](04-api-dom.md) | DOM API:`whaleui_dom_*`(解析 / 查询 / 遍历 / 修改 / 属性 / 事件 / 几何) |
| [05-api-css.md](05-api-css.md) | CSS API:`whaleui_css_*`(解析 / 加载 / 应用) |
| [06-api-fs.md](06-api-fs.md) | 虚拟文件系统:`whaleui_fs_*`(loader 替换 / URI 约定) |
| [07-api-font.md](07-api-font.md) | 字体 API:`whaleui_font_*`(注册 / 默认字体 / 系统兜底) |

## 标准支持情况

| 文档 | 内容 |
|------|------|
| [html-css-support.md](html-css-support.md) | HTML 标签 / CSS 属性 / 选择器 / @ 规则的支持矩阵与已知限制(含用例) |

## 相关内部文档

- [`doc/internal/00-structure.md`](../internal/00-structure.md) — 项目结构
- [`doc/internal/01-architecture.md`](../internal/01-architecture.md) — 底层设计
- [`doc/internal/09-implementation.md`](../internal/09-implementation.md) — 实现状态与已知限制
- [`doc/internal/css-priority.md`](../internal/css-priority.md) — CSS 属性需求优先级清单
