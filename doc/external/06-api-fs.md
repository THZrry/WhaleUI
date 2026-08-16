# 虚拟文件系统 API (whaleui_fs_*)

所有文件操作(HTML/CSS/图片/字体加载)一律经过虚拟文件系统。默认实现从磁盘读取;
用户可整体替换 loader,例如接入 HTTP(S) 以支持 CDN 上的 CSS 资源。

## 回调类型

```c
typedef void* (*whaleui_fs_open_fn)(const char* uri, void* userdata);
typedef size_t (*whaleui_fs_read_fn)(void* handle, char* buf, size_t size, void* userdata);
typedef void  (*whaleui_fs_close_fn)(void* handle, void* userdata);
```

- `open`:打开 `uri` 指向的资源,返回句柄;失败返回 `NULL`。
- `read`:从句柄读取最多 `size` 字节到 `buf`,返回实际读取字节数;0 表示 EOF。
- `close`:关闭句柄并释放资源。
- `userdata`:用户自定义上下文,所有回调透传。

## 设置 loader

```c
/* 替换全局 loader。传 NULL 恢复默认磁盘实现。 */
int whaleui_fs_set_loader(whaleui_fs_open_fn open, whaleui_fs_read_fn read,
                          whaleui_fs_close_fn close, void* userdata);
```

## 读取

```c
/* 整体读取 uri 内容到内存(内部 malloc,调用者 free)。
 * uri 支持 file:// 前缀与裸路径;其他协议由用户 loader 决定。 */
int whaleui_fs_load(const char* uri, char** out, size_t* outlen);
```

## URI 约定

| 前缀 | 处理 |
|------|------|
| `file://` / 裸路径 | 默认磁盘 loader |
| `http://` / `https://` | 需用户设置 loader 后才可用 |
| 其他 | 由用户 loader 自行解释 |

## 用例

```c
/* 默认磁盘读取 */
char* css = NULL; size_t n = 0;
whaleui_fs_load("file://themes/light.css", &css, &n);

/* 替换为网络 loader 后,同一代码可加载 CDN 资源 */
whaleui_fs_set_loader(net_open, net_read, net_close, myctx);
whaleui_fs_load("https://cdn.example.com/app.css", &css, &n);
```

## 内部调用点

- `whaleui_window_load_uri` → `whaleui_fs_load`
- `whaleui_css_load` → `whaleui_fs_load`
- `whaleui_font_register` → `whaleui_fs_load`(字体文件同样经 VFS)
