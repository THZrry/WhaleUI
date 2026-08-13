# 测试计划

单元测试零依赖:每模块一个可执行测试 target,`assert` 断言,退出码 0 即通过。
由 xmake 管理:`xmake` 构建全部 target,`xmake run <test-target>` 运行单个。

## 测试 target 与覆盖

| 测试 target | 覆盖模块 | 断言内容 |
|-------------|----------|----------|
| `test_api` | 公开 C API | variant/version 字符串、app 生命周期、返回值约定 |
| `test_fs` | 虚拟文件系统 | 默认磁盘 loader 读写、URI 前缀、set_loader 替换 |
| `test_font` | 字体 | 内存注册、默认字体设置/查询、family 列表 |
| `test_dom` | DOM | 解析、getElementById/querySelector、创建/挂载、属性、文本、样式 |
| `test_style` | 样式 | CSS 解析(注释/逗号/!important/media/keyframes)、选择器匹配、级联、var() |
| `test_layout` | 布局 | 盒模型、block 流、flex row/column、border-box、position、opacity/z-index |
| `test_render` | 渲染 | 像素级绘制验证(背景/盒子/主题切换,软件渲染路径) |
| `test_window` | 窗口 | 创建/标题/尺寸、load_html/load_uri、document 获取 |

## 运行方式

```bash
xmake                        # 构建全部(含测试)
xmake run test_api           # 运行单个测试
xmake run test_dom
```

> Windows 下测试二进制依赖 `SDL3.dll` 与 `SDL3_ttf.dll`,构建后会自动拷贝到输出目录。

## 验收标准

- 每个测试 target 退出码 0。
- 第二步阶段:测试验证 API 契约(stub 实现满足签名与基本语义);
  第三步实现真实逻辑后,测试扩展为行为断言并全部通过。

## 新增测试规则

- 新公开 API 必须同步补测试(target 内加断言)。
- 测试只依赖 `include/whaleui.h` 与内部模块头,不依赖平台后端。
