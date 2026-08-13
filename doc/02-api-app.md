# 应用 API (whaleui_app_*)

应用是顶层对象,管理事件循环、主题、渲染选项与全局资源。

## 生命周期

```c
whaleui_app_t* whaleui_app_create(void);
void whaleui_app_destroy(whaleui_app_t* app);
int  whaleui_app_run(whaleui_app_t* app);        /* 进入事件循环,阻塞 */
void whaleui_app_quit(whaleui_app_t* app);       /* 请求退出事件循环 */
```

## 主题与颜色

```c
typedef enum whaleui_theme {
    WHALEUI_THEME_SYSTEM = 0,  /* 跟随系统(默认) */
    WHALEUI_THEME_LIGHT,
    WHALEUI_THEME_DARK
} whaleui_theme_t;

int  whaleui_app_set_theme(whaleui_app_t* app, whaleui_theme_t theme);
whaleui_theme_t whaleui_app_get_theme(const whaleui_app_t* app);

/* 主题色(accent),传入 #RRGGBB 或 #AARRGGBB 字符串 */
int  whaleui_app_set_accent_color(whaleui_app_t* app, const char* hex);
```

## 渲染选项

```c
typedef enum whaleui_render_option {
    WHALEUI_RENDER_MAX_FPS = 0,      /* int: 帧率上限,0=不限 */
    WHALEUI_RENDER_BATTERY_SAVER,    /* int: 1=省电模式(默认 60fps),0=关闭 */
    WHALEUI_RENDER_VSYNC              /* int: 1=垂直同步,0=关闭 */
} whaleui_render_option_t;

int whaleui_app_set_render_option(whaleui_app_t* app,
                                  whaleui_render_option_t opt, int value);
```

## 构建变体

```c
const char* whaleui_variant(void);   /* "full" | "lite" | "minimal" */
const char* whaleui_version(void);   /* 版本号 */
```

## 返回值约定

所有返回 `int` 的 API:`0` 成功,非 0 失败。返回 `whaleui_*_t` 句柄的 API:`NULL` 失败。

## 用例

```c
whaleui_app_t* app = whaleui_app_create();
whaleui_app_set_theme(app, WHALEUI_THEME_DARK);
whaleui_app_set_render_option(app, WHALEUI_RENDER_BATTERY_SAVER, 1);
whaleui_window_t* win = whaleui_window_create(app, "MyApp", 800, 600);
whaleui_window_load_html(win, "<div>hello</div>");
whaleui_window_show(win);
whaleui_app_run(app);
whaleui_app_destroy(app);
```
