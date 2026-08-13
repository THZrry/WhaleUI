# WhaleUI

轻量级 HTML/CSS 渲染引擎与 GUI 工具包。用 C++14 编写,暴露纯 C API,可在多语言、多系统中复用。目标是小巧、快速、开箱即用的桌面小工具界面。

## 特性

- **HTML + CSS**:DOM 解析由 [lexbor](https://github.com/lexbor/lexbor) 提供;自研轻量 CSS 解析器与样式引擎,支持选择器匹配(标签 / `#id` / `.class` / 后代 / 子代)、级联(specificity + `!important`)、`var()` 自定义属性、`@media`、`@keyframes`。
- **布局**:自研盒模型与基础 flex 引擎——margin / padding / border / content、`content-box` 与 `border-box`、`px` / `%` / `em` / `auto` 长度、block 流、flex(`direction` / `justify-content` / `gap` / `flex-grow`)、`position`(static / relative / absolute / fixed)、`z-index`、`opacity`、`display: none`。
- **渲染**:SDL3 GPU(`SDL_GPU` 离屏纹理 + blit 呈现);无 GPU 环境(虚拟机 / 远程桌面)自动回退到 SDL_Renderer 软件渲染。文本由 SDL3_ttf 栅格化,字体经虚拟文件系统注册。
- **主题**:内置默认样式表(每个标签有预设,保留通用工具类),浅色 / 深色两套变量,跟随系统或手动切换,支持热切换。
- **虚拟文件系统**:所有资源(HTML / CSS / 字体)读取走统一 VFS,默认磁盘实现,可整体替换(如 HTTP CDN)。
- **三种构建目标**:`full`(SDL3 + SDL_Image + SDL3_ttf + lexbor + stb + utf8proc)、`lite`(无 SDL_Image / SDL3_ttf)、`minimal`(仅布局与渲染核心)。

## 构建

依赖:[xmake](https://xmake.io) + MinGW-w64(gcc ≥ 14)。第三方库通过 [xrepo](https://xrepo.xmake.io) 获取;`SDL3` 与 `SDL3_ttf` 优先使用官方预编译包(见下文)。

```bash
xmake            # 构建全部目标(静态库 + 测试 + demo)
xmake build demo # 只构建 demo
xmake run demo   # 运行 demo(T 切换深浅色,ESC 退出)
xmake run test_dom   # 运行单个测试(需在仓库根目录,file:// 相对路径依赖 cwd)
```

Windows 下运行任何可执行文件前,请先执行:

```powershell
.\tools\fetch-3rdparty.ps1   # 下载 SDL3_ttf / SDL3 官方预编译包到 3rdparty/
```

构建产物位于 `build/<plat>/<arch>/release/`,可执行文件旁会自动带上 `SDL3.dll` 与 `SDL3_ttf.dll`。

### 测试

```bash
xmake build
xmake run test_api test_fs test_font test_dom test_style test_layout test_render test_window
```

全部为无框架的 `assert` 风格单测,退出码 0 即通过。

## 快速上手

```c
#include "whaleui.h"

int main(void) {
    whaleui_app_t* app = whaleui_app_create();

    whaleui_window_t* win = whaleui_window_create(app, "Hello", 640, 480);
    whaleui_window_load_html(win,
        "<html><body><div class=\"card\">"
        "<h1>Hello, WhaleUI</h1>"
        "<p>Press ESC to quit.</p>"
        "</div></body></html>");
    whaleui_window_show(win);

    whaleui_app_run(app);

    whaleui_window_destroy(win);
    whaleui_app_destroy(app);
    return 0;
}
```

## 目录结构

```
include/whaleui.h       # 唯一公开头文件:全部 whaleui_* C API
src/
  core/                 # 应用 / 窗口 / 事件循环 / 主题
  dom/                  # DOM(包装 lexbor)
  style/                # CSS 解析、样式引擎、默认主题
  layout/               # 布局引擎
  render/               # SDL3 渲染(GPU + 软件回退)
  fs/                   # 虚拟文件系统
  font/                 # 字体注册
  platform/             # 平台后端(Windows 现成,Linux/macOS 待补)
doc/                    # 设计文档
tests/                  # 单元测试
examples/demo.cpp       # 演示程序
```

## 文档

- [README-css.md](README-css.md) — CSS 属性支持清单与优先级
- [doc/00-structure.md](doc/00-structure.md) — 项目结构
- [doc/01-architecture.md](doc/01-architecture.md) — 底层设计
- [doc/09-implementation.md](doc/09-implementation.md) — 实现状态与已知限制
- 各模块 API 手册见 `doc/02-*.md` ~ `doc/07-*.md`

## 平台

| 平台 | 状态 |
|------|------|
| Windows | ✅ 支持(SDL3 窗口、注册表深浅色检测、系统字体目录) |
| Linux(Wayland/X11) | 🚧 SDL3 跨平台,平台后端待实现 |
| macOS | 🚧 同上 |
| Android / iOS | 🔲 预留 |

## License

MIT(第三方依赖遵循各自许可证,见 `3rdparty/*/share/licenses`)。
