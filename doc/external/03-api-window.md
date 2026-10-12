# 窗口 API (whaleui_window_*)

窗口是应用内的显示单元,承载一个文档。原生窗口在首次 `show()` 时惰性创建;在此之前 title/size 等由库缓存,创建时应用。

> 本文档与 `include/whaleui.h` 一致。窗口扩展已全部实现,无剩余未实现项。

## 创建与销毁

```c
whaleui_window_t* whaleui_window_create(whaleui_app_t* app,
                                        const char* title, int width, int height);
void whaleui_window_destroy(whaleui_window_t* win);
```

## 显示状态

```c
int whaleui_window_show(whaleui_window_t* win);   /* 首次调用时创建原生窗口 */
int whaleui_window_hide(whaleui_window_t* win);
int whaleui_window_close(whaleui_window_t* win);  /* 拆除原生窗口+渲染上下文,句柄仍有效 */

/* 查询 */
int whaleui_window_is_visible(const whaleui_window_t* win);
int whaleui_window_is_minimized(const whaleui_window_t* win);
int whaleui_window_is_maximized(const whaleui_window_t* win);
int whaleui_window_is_focused(const whaleui_window_t* win);

/* 操作 */
int whaleui_window_minimize(whaleui_window_t* win);
int whaleui_window_maximize(whaleui_window_t* win);
int whaleui_window_restore(whaleui_window_t* win);
int whaleui_window_raise(whaleui_window_t* win);  /* 提升到桌面叠放次序顶部 */
```

## 标题与几何(逻辑像素)

```c
int whaleui_window_set_title(whaleui_window_t* win, const char* title);
const char* whaleui_window_get_title(const whaleui_window_t* win);

int whaleui_window_set_size(whaleui_window_t* win, int width, int height);
int whaleui_window_get_size(const whaleui_window_t* win, int* w, int* h);

/* 物理(后备缓冲)尺寸,高 DPI 缩放下与 get_size 不同 */
int whaleui_window_get_size_in_pixels(const whaleui_window_t* win, int* w, int* h);

/* 屏幕坐标位置 */
int whaleui_window_set_position(whaleui_window_t* win, int x, int y);
int whaleui_window_get_position(const whaleui_window_t* win, int* x, int* y);
int whaleui_window_center(whaleui_window_t* win); /* 居中于当前显示器 */

/* 最小/最大客户区约束(0 = 不限制) */
int whaleui_window_set_min_size(whaleui_window_t* win, int width, int height);
int whaleui_window_get_min_size(const whaleui_window_t* win, int* w, int* h);
int whaleui_window_set_max_size(whaleui_window_t* win, int width, int height);
int whaleui_window_get_max_size(const whaleui_window_t* win, int* w, int* h);
```

## 外观

```c
int   whaleui_window_set_bordered(whaleui_window_t* win, int bordered);      /* 1=系统边框(默认),0=无边框 */
int   whaleui_window_set_resizable(whaleui_window_t* win, int resizable);    /* 1=可调(默认),0=固定 */
int   whaleui_window_set_always_on_top(whaleui_window_t* win, int on_top);   /* 置顶 */
int   whaleui_window_set_opacity(whaleui_window_t* win, float opacity);      /* 0.0..1.0,默认 1.0 */
float whaleui_window_get_opacity(const whaleui_window_t* win);

/* 图标:RGBA 像素(库拷贝);或经 VFS 加载图片文件(full=SDL_image / lite=stb_image) */
int whaleui_window_set_icon(whaleui_window_t* win,
                            const unsigned char* rgba, int width, int height);
int whaleui_window_set_icon_uri(whaleui_window_t* win, const char* uri);
```

## 全屏

```c
typedef enum whaleui_fullscreen_mode {
    WHALEUI_FULLSCREEN_OFF = 0,
    WHALEUI_FULLSCREEN_DESKTOP, /* 铺满桌面分辨率(推荐) */
    WHALEUI_FULLSCREEN_REAL     /* 独占:切换显示器分辨率 */
} whaleui_fullscreen_mode_t;

int whaleui_window_set_fullscreen(whaleui_window_t* win, whaleui_fullscreen_mode_t mode);
whaleui_fullscreen_mode_t whaleui_window_get_fullscreen(const whaleui_window_t* win);
```

## 输入焦点与鼠标捕获

```c
int whaleui_window_set_mouse_grab(whaleui_window_t* win, int grab);
int whaleui_window_get_mouse_grab(const whaleui_window_t* win);
int whaleui_window_set_keyboard_grab(whaleui_window_t* win, int grab);
int whaleui_window_get_keyboard_grab(const whaleui_window_t* win);

int whaleui_window_set_mouse_position(whaleui_window_t* win, int x, int y);
int whaleui_window_set_focusable(whaleui_window_t* win, int focusable); /* 1=可获焦点(默认) */
```

## 父窗口与模态(平台相关:Windows/macOS/X11)

```c
int whaleui_window_set_parent(whaleui_window_t* win, whaleui_window_t* parent); /* NULL 解除 */
int whaleui_window_set_modal(whaleui_window_t* win, whaleui_window_t* parent);
```

## 显示信息

```c
int   whaleui_window_display_id(const whaleui_window_t* win);    /* 所在显示器 ID,-1 不可用 */
float whaleui_window_display_scale(const whaleui_window_t* win); /* 逻辑/物理像素比,失败返回 1.0 */
```

## 任务栏通知

```c
typedef enum whaleui_flash {
    WHALEUI_FLASH_CANCEL = 0,
    WHALEUI_FLASH_BRIEFLY,
    WHALEUI_FLASH_UNTIL_FOCUSED
} whaleui_flash_t;

int whaleui_window_flash(whaleui_window_t* win, whaleui_flash_t mode);
```

## 窗口事件回调

```c
typedef enum whaleui_window_event {
    WHALEUI_WINDOW_EVENT_CLOSE = 0,      /* 用户点关闭;回调返回非 0 取消默认关闭 */
    WHALEUI_WINDOW_EVENT_RESIZED,        /* a=宽 b=高(逻辑像素) */
    WHALEUI_WINDOW_EVENT_MOVED,          /* a=x b=y(屏幕坐标) */
    WHALEUI_WINDOW_EVENT_SHOWN,
    WHALEUI_WINDOW_EVENT_HIDDEN,
    WHALEUI_WINDOW_EVENT_MINIMIZED,
    WHALEUI_WINDOW_EVENT_RESTORED,
    WHALEUI_WINDOW_EVENT_MAXIMIZED,
    WHALEUI_WINDOW_EVENT_FOCUS_GAINED,
    WHALEUI_WINDOW_EVENT_FOCUS_LOST,
    WHALEUI_WINDOW_EVENT_MOUSE_ENTER,
    WHALEUI_WINDOW_EVENT_MOUSE_LEAVE
} whaleui_window_event_t;

/* 返回 0 允许默认行为(仅 CLOSE 有:关闭窗口);非 0 取消 */
typedef int (*whaleui_window_event_cb)(whaleui_window_t* win,
                                       whaleui_window_event_t ev,
                                       int a, int b, void* userdata);
int whaleui_window_set_event_callback(whaleui_window_t* win,
                                      whaleui_window_event_cb cb, void* userdata);
```

事件在工作线程派发,回调内不要阻塞。`CLOSE` 返回非 0 即取消默认关闭(窗口保持可见);
其余事件的返回值被忽略。`RESIZED` 在合并节流(80ms)之前派发,拖拽中会收到多次。

## 按窗口输入回调(优先于应用全局回调)

```c
int whaleui_window_set_key_callback(whaleui_window_t* win, whaleui_key_cb cb,
                                    void* userdata);
int whaleui_window_set_select_callback(whaleui_window_t* win, whaleui_select_cb cb,
                                       void* userdata);
```

窗口上设置后,该窗口的按键/下拉选择走窗口回调,不再走 `whaleui_app_set_key_callback` /
`whaleui_app_set_select_callback` 的应用全局回调;未设置的窗口继续走全局回调。
这是多窗口不串扰的关键:全局回调只做兜底。

## 无边框 HitTest(拖拽与缩放)

```c
typedef enum whaleui_hit_test_result {
    WHALEUI_HITTEST_NORMAL = 0,
    WHALEUI_HITTEST_DRAGGABLE = 1,
    WHALEUI_HITTEST_RESIZE_TOPLEFT = 2,
    WHALEUI_HITTEST_RESIZE_TOP = 3,
    WHALEUI_HITTEST_RESIZE_TOPRIGHT = 4,
    WHALEUI_HITTEST_RESIZE_RIGHT = 5,
    WHALEUI_HITTEST_RESIZE_BOTTOMRIGHT = 6,
    WHALEUI_HITTEST_RESIZE_BOTTOM = 7,
    WHALEUI_HITTEST_RESIZE_BOTTOMLEFT = 8,
    WHALEUI_HITTEST_RESIZE_LEFT = 9
} whaleui_hit_test_result_t;
typedef whaleui_hit_test_result_t (*whaleui_hit_test_fn)(whaleui_window_t* win,
                                                          int x, int y, void* userdata);
int whaleui_window_set_hit_test(whaleui_window_t* win, whaleui_hit_test_fn cb,
                                void* userdata);
```

`SDL_SetWindowHitTest` 的薄包装。`set_bordered(win, 0)` 且未设置自定义 HitTest 时,
库自动安装默认实现(8px 边缘八向缩放 + 顶部 32px 标题拖拽);恢复边框时自动拆除
默认实现(自定义的不动)。`cb` 传 NULL 即拆除。坐标为窗口客户区逻辑像素。

## 内容加载

```c
/* 直接加载 HTML 字符串(替换当前文档) */
int whaleui_window_load_html(whaleui_window_t* win, const char* html);

/* 从 URI 加载(经虚拟文件系统;可指向 file:// 或用户 loader 支持的协议) */
int whaleui_window_load_uri(whaleui_window_t* win, const char* uri);

/* 获取当前文档(加载前为 NULL;窗口销毁时一并销毁) */
whaleui_dom_document_t* whaleui_window_get_document(whaleui_window_t* win);
```

## 用例

```c
whaleui_window_t* win = whaleui_window_create(app, "demo", 800, 600);
whaleui_window_center(win);
whaleui_window_set_min_size(win, 400, 300);
whaleui_window_set_icon_uri(win, "file://assets/app.png");
whaleui_window_load_uri(win, "file://pages/main.html");
whaleui_window_show(win);

whaleui_window_set_event_callback(win, [](whaleui_window_t* w,
                                          whaleui_window_event_t ev,
                                          int a, int b, void*) {
    if (ev == WHALEUI_WINDOW_EVENT_CLOSE) {
        /* 询问后再关:返回 0 允许默认关闭 */
        return 0;
    }
    if (ev == WHALEUI_WINDOW_EVENT_RESIZED) {
        /* a/b = 新宽高 */
    }
    return 0;
}, nullptr);
```
