# 窗口 API (whaleui_window_*)

窗口是应用内的显示单元,承载一个文档。

## 创建与销毁

```c
whaleui_window_t* whaleui_window_create(whaleui_app_t* app,
                                        const char* title, int width, int height);
void whaleui_window_destroy(whaleui_window_t* win);
```

## 显示状态

```c
int whaleui_window_show(whaleui_window_t* win);
int whaleui_window_hide(whaleui_window_t* win);
int whaleui_window_close(whaleui_window_t* win);   /* 关闭并标记销毁 */
```

## 属性

```c
int whaleui_window_set_title(whaleui_window_t* win, const char* title);
const char* whaleui_window_get_title(const whaleui_window_t* win);
int whaleui_window_set_size(whaleui_window_t* win, int width, int height);
int whaleui_window_get_size(const whaleui_window_t* win, int* w, int* h);
```

## 内容加载

```c
/* 直接加载 HTML 字符串 */
int whaleui_window_load_html(whaleui_window_t* win, const char* html);

/* 从 URI 加载(经虚拟文件系统;可指向 file:// 或用户 loader 支持的协议) */
int whaleui_window_load_uri(whaleui_window_t* win, const char* uri);

/* 获取当前文档 */
whaleui_dom_document_t* whaleui_window_get_document(whaleui_window_t* win);
```

## 用例

```c
whaleui_window_t* win = whaleui_window_create(app, "demo", 800, 600);
whaleui_window_load_uri(win, "file://pages/main.html");
whaleui_window_show(win);
```
