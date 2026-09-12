# 项目进程: WhaleUI

第六步(长期维护): 修复 BUG、补充功能与优化。

<1>(SOLVED): 主题 CSS 按参考重写 —— 编译通过,16 张离屏渲染的深浅配对与 token
对照自检全部通过;引擎缺口的实测结论记在 doc/internal/10-theme-gaps.md
<2>(UNRESOLVED): 主题样式与参考截图的肉眼比对未做(本机无图像理解能力),见 notes/current.md
<3>(TIPS): 判断引擎支持什么不要看文档、要实测 —— 探针页 temp/gaps.html、
temp/gaps2.html 每行一种写法,渲染后用 temp/rect_color.py 采样(本次实测推翻了
"伪元素只是文本 run""相对定位会压成竖线"等文档结论)
<4>(SOLVED): 引擎 CSS 缺口修复(16 项,其中 6 项是清单里没有的或描述错的) ——
水平 padding/margin 取了 right 索引、inline-flex 撑满整行、out-of-flow 子项被当
flex item、text run 继承 overflow 被当容器、多阴影只画第一层(且无 spread)、
outline 不绘制、四边 border-color 退化单色、text-shadow 与**全部彩色文字**
R/B 反转(GPU text layer 双重交换)、UTF-8 BOM 让整页 CSS 失效、
`[attr]`/`:checked`/`:disabled` 对无值属性不匹配、无方向 `linear-gradient()`
不画、`background-size/-position/-repeat` 被忽略、`hsl()` 不认、
`rotate()` 带崩整串 transform、渐变过渡瞬时、checkbox 布局盒被 UA 的
`min-height` 撑成 32px;支持矩阵与逐项证据见
doc/internal/16-engine-support.md,修复前数据留在 temp/engine-gaps.md
<5>(UNRESOLVED): `transform: rotate` 只解析不绘制(需矩阵 paint 路径);
`backdrop-filter` 仅 GPU 路径;`conic/repeating-gradient`、`hwb/oklab/color-mix`、
`attr()/counter()`、`::first-line`、`:has()`、容器查询、`filter`/混合模式未实现 ——
见 doc/internal/16-engine-support.md 与 notes/current.md
