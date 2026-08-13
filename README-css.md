# 最重要的实现部分
CSS中有些属性是最常用的，也需要最早实现，这些是最基础的属性，需要优先支持。
color, background, margin, padding, border, display, width, height, font-size, font-weight, text-align, line-height, position, top, left, right, bottom, z-index, flex, justify-content, align-items, gap, opacity, cursor, overflow, box-sizing

# 必须实现的部分
这些属性总会使用，需要实现
font-family, text-decoration, text-transform, letter-spacing, word-spacing, white-space, border-radius, box-shadow, background-color, background-image, background-size, background-position, background-repeat, transform, transition, animation, flex-direction, flex-wrap, align-content, flex-grow, flex-shrink, flex-basis, grid-template-columns, grid-template-rows, grid-gap, place-items, float, clear, list-style, visibility, pointer-events, user-select, max-width, min-width, max-height, min-height, margin-top/right/bottom/left, padding-top/right/bottom/left, border-width, border-style, border-color, outline
另外，var，media和keyframe等也是需要做的

# 最好实现的内容
这些是其余常用css属性，实现了可以增强兼容性
font-style, font-line-height, text-indent, text-shadow, vertical-align, word-break, overflow-wrap, resize, object-fit, aspect-ratio, clip-path, filter, backdrop-filter, perspective, transform-origin, transition-duration, transition-delay, transition-timing-function, animation-duration, animation-delay, animation-iteration-count, animation-direction, animation-fill-mode, grid-column, grid-row, grid-auto-flow, order, align-self, justify-self, justify-items, column-count, column-gap, content, counter-increment, counter-reset, quotes, inset, scroll-behavior, scroll-margin, scroll-padding, tab-size, fill, stroke, stroke-width

# 全部
剩余css内容请参照W3C，挑选高频的优先实现。

# 不用实现的内容
这个项目对HTML/CSS的兼容性要求不高，已弃用内容可以不兼容

布局相关的属性应该全部实现，因为计算位置等事情应交给lexbor。兼容和不兼容的属性需要在文档中单独列表指定。