# WhaleUI 底层设计

## 核心类型

公开 API 中所有句柄均为不透明指针,内部结构定义在 `src/` 各模块。

| 公开类型 | 内部结构 | 职责 |
|----------|----------|------|
| `whaleui_app_t*` | `WhaleUIApp` (src/core/app.h) | 应用实例:事件循环、主题、渲染选项、全局字体表 |
| `whaleui_window_t*` | `WhaleUIWindow` (src/core/window.h) | 窗口:标题/尺寸/可见性/挂载的 DOM |
| `whaleui_dom_document_t*` | `WhaleUIDocument` (src/dom/dom.h) | 文档:根节点、解析结果 |
| `whaleui_dom_element_t*` | `WhaleUIElement` (src/dom/element.h) | 元素:tag/属性/子节点/计算样式 |
| `whaleui_css_rule_t*` | `WhaleUIRule` (src/style/css.h) | 样式规则:选择器 + 声明 |
| `whaleui_font_t*` | `WhaleUIFont` (src/font/font.h) | 字体:family/文件路径/内存数据 |

## 内部模块接口

### fs —— 虚拟文件系统 (src/fs/fs.h)

所有文件操作(HTML/CSS/图片/字体)必经此处。

```c
typedef void* (*whaleui_fs_open_fn)(const char* uri, void* userdata);
typedef size_t (*whaleui_fs_read_fn)(void* handle, char* buf, size_t n, void* userdata);
typedef void  (*whaleui_fs_close_fn)(void* handle, void* userdata);

int  whaleui_fs_set_loader(whaleui_fs_open_fn, whaleui_fs_read_fn,
                           whaleui_fs_close_fn, void* userdata);
int  whaleui_fs_load(const char* uri, char** out, size_t* outlen); /* 整体读取 */
```

- `uri` 支持 `file://` 与裸路径;`http(s)://` 由用户替换 loader 后自行实现。
- 未设置 loader 时使用内置磁盘实现。
- 字体加载同样走 `whaleui_fs_load`(见 font 模块)。

### font —— 字体 (src/font/font.h)

```c
int  whaleui_font_register(const char* uri);              /* 经 VFS 加载 */
int  whaleui_font_register_memory(const unsigned char* data, size_t len);
void whaleui_font_set_default(const char* family);
```

- 默认从系统字体目录加载兜底字体(各平台 `platform/` 提供路径)。
- 自定义字体注册后进入全局字体表,样式引擎按 `font-family` 查找。
- stb_font 彩色 emoji 补充(issue #512 / PR #1135)在渲染层字体光栅化时处理。

### platform —— 平台抽象 (src/platform/platform.h)

```c
typedef struct whaleui_platform_api {
    /* 窗口:创建/显示/隐藏/关闭/设置标题尺寸 */
    /* 事件:轮询/分发 */
    /* 系统字体目录枚举 */
    /* 系统主题检测(深浅色) */
} whaleui_platform_api_t;
```

- Windows / Linux / macOS 各实现一份,`render` 通过它拿到原生窗口句柄创建 SDL3 GPU surface。
- Android/iOS 预留目录,第二步不实现。

## 渲染管线 (render)

1. DOM 解析(lexbor)→ 文档句柄
2. 样式计算(CSS 规则 + 主题变量)→ 每个元素 computed style(见 `src/style/`)
3. 布局:自研盒模型 + 基础 flex(`src/layout/`,lexbor 只提供解析不计算布局;位置/尺寸计算在本项目内完成)
4. 渲染:`src/render/` 将布局树绘制到 CPU framebuffer,经 SDL_GPU 纹理呈现;无 GPU 时回退 SDL_Renderer 软件渲染
5. 帧率:省电模式默认 60fps,允许用户覆盖(`whaleui_app_set_render_option`)

## 主题与样式

- 内置默认样式表 + 浅/深色两套 `--*` 变量,可热切换。
- 系统风格(Windows Classic / Aero / Metro / Fluent、GTK、macOS、Material)以 `--*` 变量表的形式逐步扩展(见 `doc/09-implementation.md`)。
