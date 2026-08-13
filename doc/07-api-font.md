# 字体 API (whaleui_font_*)

字体加载**一律经虚拟文件系统**(见 06-api-fs.md)。默认按系统字体目录兜底,用户可注册自定义字体。

## 注册自定义字体

```c
/* 从 URI 加载字体文件并注册(经 VFS;file:// 或用户 loader 支持的协议)。
 * 成功返回字体内存中 family 名,失败返回 NULL。 */
const char* whaleui_font_register(const char* uri);

/* 从内存注册字体(不复制数据,调用者保证存活)。 */
const char* whaleui_font_register_memory(const unsigned char* data, size_t len);
```

## 默认字体

```c
/* 设置默认字体 family;样式引擎对未指定 font-family 的元素使用它。 */
int whaleui_font_set_default(const char* family);

/* 查询当前默认字体 family。 */
const char* whaleui_font_get_default(void);
```

## 查询

```c
/* 返回已注册的 family 列表(逗号分隔),用于调试/枚举。 */
const char* whaleui_font_list(void);
```

## 系统字体兜底

- 未注册任何字体时,默认从系统字体目录加载兜底字体(各平台 `src/platform/<os>/` 提供路径)。
- 平台字体枚举失败时回退到内置 stb 位图字体。

## emoji 说明

- stb_font 不支持彩色 emoji;按 nothings/stb issue #512 与 PR #1135 的方案在渲染层补充
  (彩色 emoji 走 SDL3_ttf/系统彩色字体,单色文本走 stb)。

## 用例

```c
/* 注册自定义字体(经 VFS,磁盘路径) */
whaleui_font_register("file://fonts/MyFont.ttf");
whaleui_font_set_default("MyFont");

/* 或从内存注册 */
const unsigned char* ttf = load_my_font();
whaleui_font_register_memory(ttf, ttf_len);
```
