# WhaleUI 项目结构

## 目录总览

```
whaleui/
├── include/
│   └── whaleui.h              # 唯一公开头文件:全部公开 C API
├── src/
│   ├── core/                  # 应用/窗口核心、生命周期、主题(含公开 C API 实现)
│   ├── dom/                   # DOM 解析与操作(包装 lexbor,含公开 C API 实现)
│   ├── style/                 # 样式计算、CSS 解析、默认主题与变量(含公开 C API 实现)
│   ├── layout/                # 自研布局引擎(盒模型 + 基础 flex)
│   ├── render/                # 渲染(SDL3 GPU + 软件回退,脏矩形/缓存为后续优化)
│   ├── fs/                    # 虚拟文件系统(默认磁盘,可替换,含公开 C API 实现)
│   ├── font/                  # 字体注册/加载/默认字体(含公开 C API 实现)
│   └── platform/
│       ├── windows/           # Windows 后端(私有头文件随目录)
│       ├── linux/             # Linux 后端(Wayland 优先,X11 兼容)
│       └── macos/             # macOS 后端
├── doc/                       # 设计文档(本目录)
├── tests/                     # 单元测试(零依赖,assert 风格)
├── examples/                  # demo 示例
├── tools/
│   └── fetch-3rdparty.ps1     # 下载官方预编译第三方包
├── 3rdparty/                  # 预编译第三方包(仅 include 入库,其余 gitignore)
└── xmake.lua                  # 构建脚本(Full/Lite/Minimal 三 target)
```

> 注:公开 C API(`whaleui_*`)实现在各自模块内就地实现(core/dom/style/fs/font),不设独立薄包装目录——内部结构与公开接口一一对应,减少一层间接。

## 分层原则

- **公开层**:`include/whaleui.h` 是唯一对外头文件,全部函数为 `whaleui_` 前缀的 C 函数,便于多语言/多系统绑定。
- **内部层**:`src/` 内各模块头文件就近存放,不对外暴露、不安装。
- **平台层**:`src/platform/<os>/` 各自私有,通过 `platform.h` 抽象统一入口;脱离平台的方法集中在 `src/` 上层。
- **资源层**:所有文件读取(HTML/CSS/图片/字体)一律走虚拟文件系统 `src/fs/`,默认磁盘实现,用户可整体替换(如接 HTTP CDN)。

## 构建目标

| target | 说明 |
|--------|------|
| `whaleui-full` | 全功能:SDL3 + SDL_Image + SDL3_ttf + lexbor + stb + utf8proc |
| `whaleui-lite` | 精简:砍 SDL_Image/SDL3_ttf,stb 管理资源 |
| `whaleui-minimal` | 仅布局:无 HTML 解析/缓存,stb 资源 |
