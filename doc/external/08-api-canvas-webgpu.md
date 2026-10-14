# Canvas 与 WebGPU API (whaleui_canvas_* / whaleui_gpu_*)

与浏览器 `HTMLCanvasElement` / `CanvasRenderingContext2D` /
`navigator.gpu` 对应。`<canvas>` 按 HTML 规范默认 300x150,CSS 显式尺寸优先。

> 本文档与 `include/whaleui.h` 一致。

## Canvas 元素

```c
typedef struct whaleui_canvas_context whaleui_canvas_context_t;
typedef struct whaleui_image_data whaleui_image_data_t;

/* getContext("2d"):仅 <canvas> + "2d" 返回非 NULL */
whaleui_canvas_context_t* whaleui_canvas_get_context(whaleui_dom_element_t* el,
                                                     const char* type);
/* width/height 属性(重置 backing 为透明黑) */
int whaleui_canvas_set_size(whaleui_dom_element_t* el, int w, int h);
int whaleui_canvas_get_size(whaleui_dom_element_t* el, int* w, int* h);
```

布局按 `width/height` 属性给内在尺寸(默认 300x150);`set_size` 后
触发结构性脏标记,下一帧重排。backing 变化会自动重绘,无需手动
`mark_dirty`。

## 2D 上下文状态

```c
int whaleui_canvas_set_fill_style(whaleui_canvas_context_t* ctx, const char* style);
int whaleui_canvas_set_stroke_style(whaleui_canvas_context_t* ctx, const char* style);
int whaleui_canvas_set_line_width(whaleui_canvas_context_t* ctx, float w);
int whaleui_canvas_set_global_alpha(whaleui_canvas_context_t* ctx, float a);
/* font 简写("bold 16px Arial, sans-serif");text_align =
 * start|end|left|right|center;text_baseline =
 * alphabetic|top|middle|bottom|hanging|ideographic */
int whaleui_canvas_set_font(whaleui_canvas_context_t* ctx, const char* font);
int whaleui_canvas_set_text_align(whaleui_canvas_context_t* ctx, const char* align);
int whaleui_canvas_set_text_baseline(whaleui_canvas_context_t* ctx, const char* baseline);
```

颜色支持 `#RRGGBB` / `#RGB` / `#AARRGGBB` / `rgb(r,g,b)` / 有限命名色
(black/white/red/green/lime/blue/yellow/cyan/magenta/gray/transparent)。
渐变/图案暂不支持,`set_*` 遇到不支持的写法时保留旧值。
`set_text_align/set_text_baseline` 传非法值返回非 0(浏览器会忽略非法值,
此处以失败码区分)。

## 矩形

```c
int whaleui_canvas_fill_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h);
int whaleui_canvas_stroke_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h);
int whaleui_canvas_clear_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h);
```

`fill_rect` 受当前变换矩阵影响;`clear_rect` 直接清透明(不受变换影响)。

## 路径

```c
int whaleui_canvas_begin_path(whaleui_canvas_context_t* ctx);
int whaleui_canvas_move_to(whaleui_canvas_context_t* ctx, float x, float y);
int whaleui_canvas_line_to(whaleui_canvas_context_t* ctx, float x, float y);
int whaleui_canvas_close_path(whaleui_canvas_context_t* ctx);
int whaleui_canvas_rect(whaleui_canvas_context_t* ctx, float x, float y, float w, float h);
int whaleui_canvas_arc(whaleui_canvas_context_t* ctx, float x, float y, float r, float a0, float a1);
int whaleui_canvas_fill(whaleui_canvas_context_t* ctx);
int whaleui_canvas_stroke(whaleui_canvas_context_t* ctx);
```

填充为 even-odd 扫描线;`arc` 当前为整圆(`a0/a1` 保留,后续支持弧段);
贝塞尔曲线暂不支持。

## 状态栈与变换

```c
int whaleui_canvas_save(whaleui_canvas_context_t* ctx);
int whaleui_canvas_restore(whaleui_canvas_context_t* ctx);
int whaleui_canvas_set_transform(whaleui_canvas_context_t* ctx, float a, float b, float c, float d, float e, float f);
int whaleui_canvas_transform(whaleui_canvas_context_t* ctx, float a, float b, float c, float d, float e, float f);
int whaleui_canvas_translate(whaleui_canvas_context_t* ctx, float x, float y);
int whaleui_canvas_scale(whaleui_canvas_context_t* ctx, float x, float y);
int whaleui_canvas_rotate(whaleui_canvas_context_t* ctx, float angle);
```

矩阵语义与浏览器一致(`x' = a*x + c*y + e`)。空栈 `restore` 返回失败。

## 文本(fillText/strokeText/measureText)

```c
int whaleui_canvas_fill_text(whaleui_canvas_context_t* ctx, const char* text, float x, float y);
int whaleui_canvas_stroke_text(whaleui_canvas_context_t* ctx, const char* text, float x, float y);
/* measureText 返回 advance 宽与行高 */
int whaleui_canvas_measure_text(whaleui_canvas_context_t* ctx, const char* text, int* out_w, int* out_h);
```

`x/y` 是文本原点(默认 alphabetic 基线);`text_align`/`text_baseline` 与当前变换
一并生效。文本光栅走 SDL3_ttf + 字体注册表(与页面文字同一字体链),`fill`
用 `fillStyle` 颜色、`stroke` 画字形轮廓(edge-only,近似)。**lite/minimal 变体
暂无 TTF 后端,三个 API 返回 -2**(Full 构建可用;`test_canvas` 按变体断言)。

## ImageData(像素 0xAARRGGBB)

```c
whaleui_image_data_t* whaleui_canvas_create_image_data(whaleui_canvas_context_t* ctx, int w, int h);
void whaleui_image_data_destroy(whaleui_image_data_t* d);
int whaleui_image_data_size(const whaleui_image_data_t* d, int* w, int* h);
unsigned int whaleui_image_data_get(const whaleui_image_data_t* d, int x, int y);
int whaleui_image_data_set(whaleui_image_data_t* d, int x, int y, unsigned int argb);
whaleui_image_data_t* whaleui_canvas_get_image_data(whaleui_canvas_context_t* ctx, int x, int y, int w, int h);
int whaleui_canvas_put_image_data(whaleui_canvas_context_t* ctx, whaleui_image_data_t* d, int x, int y);
```

`get/put` 越界像素按透明黑/裁剪处理。`drawImage` 暂未实现(需要图片解码
链),后续补齐。

## 用例

```c
whaleui_dom_element_t* c = whaleui_dom_get_element_by_id(doc, "c");
whaleui_canvas_context_t* ctx = whaleui_canvas_get_context(c, "2d");
whaleui_canvas_set_fill_style(ctx, "#FF0000");
whaleui_canvas_fill_rect(ctx, 0, 0, 300, 150);
whaleui_canvas_begin_path(ctx);
whaleui_canvas_move_to(ctx, 0, 0);
whaleui_canvas_line_to(ctx, 300, 150);
whaleui_canvas_stroke(ctx);
```

## WebGPU(navigator.gpu C-port,当前为 stub 后端)

```c
typedef struct whaleui_gpu_adapter whaleui_gpu_adapter_t;
typedef struct whaleui_gpu_device whaleui_gpu_device_t;
typedef struct whaleui_gpu_queue whaleui_gpu_queue_t;
typedef struct whaleui_gpu_buffer whaleui_gpu_buffer_t;
typedef struct whaleui_gpu_texture whaleui_gpu_texture_t;
typedef struct whaleui_gpu_canvas_context whaleui_gpu_canvas_context_t;

typedef enum whaleui_gpu_backend {
    WHALEUI_GPU_BACKEND_NONE = 0,
    WHALEUI_GPU_BACKEND_SDL_GPU,
    WHALEUI_GPU_BACKEND_WGPU_NATIVE
} whaleui_gpu_backend_t;

typedef struct whaleui_gpu_limits {
    unsigned int max_texture_dimension_2d;
    unsigned int max_buffer_size;
} whaleui_gpu_limits_t;

whaleui_gpu_backend_t whaleui_gpu_backend(void);
int whaleui_gpu_is_supported(void);

int whaleui_gpu_request_adapter(whaleui_gpu_adapter_t** out);
void whaleui_gpu_adapter_destroy(whaleui_gpu_adapter_t* a);
int whaleui_gpu_adapter_limits(const whaleui_gpu_adapter_t* a, whaleui_gpu_limits_t* out);
int whaleui_gpu_adapter_request_device(whaleui_gpu_adapter_t* a, whaleui_gpu_device_t** out);
void whaleui_gpu_device_destroy(whaleui_gpu_device_t* d);
whaleui_gpu_queue_t* whaleui_gpu_device_queue(whaleui_gpu_device_t* d);

whaleui_gpu_buffer_t* whaleui_gpu_device_create_buffer(whaleui_gpu_device_t* d,
                                                       unsigned long long size, unsigned int usage);
void whaleui_gpu_buffer_destroy(whaleui_gpu_buffer_t* b);
int whaleui_gpu_queue_write_buffer(whaleui_gpu_queue_t* q, whaleui_gpu_buffer_t* b,
                                   unsigned long long offset, const void* data, size_t size);
whaleui_gpu_texture_t* whaleui_gpu_device_create_texture_2d(whaleui_gpu_device_t* d,
                                                            unsigned int w, unsigned int h, unsigned int format);
void whaleui_gpu_texture_destroy(whaleui_gpu_texture_t* t);

whaleui_gpu_canvas_context_t* whaleui_gpu_canvas_get_context(whaleui_dom_element_t* el);
int whaleui_gpu_canvas_configure(whaleui_gpu_canvas_context_t* c, whaleui_gpu_device_t* d,
                                 unsigned int format, unsigned int alpha_mode);
void whaleui_gpu_canvas_context_destroy(whaleui_gpu_canvas_context_t* c);
```

对应 `requestAdapter` / `requestDevice` / `queue.writeBuffer` /
`configure` 最小闭环。当前为 stub:非 Minimal 构建返回
`SDL_GPU` 后端桩(调用 accepted,无真实上传),Minimal 返回
`NONE` 不可用。`wgpu-native` 真实后端后续接入,签名不变。

## 实现说明

- Canvas backing 为 CPU `0xAARRGGBB`,绘制合成为布局盒拉伸 blit
  (与 `text_layer` 同机制),GPU 上传复用后续再做。
- `Minimal` 变体:WebGPU 不可用(`request_adapter` 失败);Canvas 2D 可用。
