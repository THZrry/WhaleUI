# Web 平台 API(clipboard / 定时器 / 视口 / 音频 / 手柄)

对应浏览器 `navigator.clipboard`、`setTimeout`/`setInterval`/
`requestAnimationFrame`、`window.visualViewport`、`AudioContext`、
`navigator.getGamepads`。这些能力由 WhaleUI 内部事件循环承接,或包装
SDL3 已有能力。

> 本文档与 `include/whaleui.h` 一致。

## Clipboard(navigator.clipboard 子集)

```c
int whaleui_clipboard_has_text(void);          /* 1 = 有文本 */
const char* whaleui_clipboard_get_text(void);  /* "" 为空;指针下次调用前有效 */
int whaleui_clipboard_set_text(const char* text);

/* 异步(promise 语义)形式:回调在事件循环线程上执行 */
typedef void (*whaleui_clipboard_text_cb)(const char* text, void* userdata);
int whaleui_clipboard_read_text_async(whaleui_clipboard_text_cb cb, void* userdata);
int whaleui_clipboard_write_text_async(const char* text,
                                       whaleui_clipboard_text_cb cb, void* userdata);
```

底层是 SDL 的系统剪贴板,内容与桌面其他程序共享。浏览器 API 是异步的,
引擎没有 promise 对象,因此给出同步对 + 回调形式;回调**排队到事件循环**
(不是内联调用),调用方观察到与 promise 相同的"稍后 resolve"时序。

可编辑控件的 Ctrl+C/X/V 会先派发 DOM `copy`/`cut`/`paste`(冒泡):
- 监听器可 `preventDefault()` 取消引擎的默认写入/插入;
- 监听器可用 `whaleui_dom_event_set_clipboard_text(ev, "text")` 提供自己的
  文本(复制/剪切时写入剪贴板,paste 时插入该文本);
- `whaleui_dom_event_clipboard_text(ev)` 读取事件携带的文本。

## 定时器(setTimeout / setInterval)

```c
double whaleui_now(void);  /* 应用启动以来的毫秒(performance.now()) */

typedef void (*whaleui_timer_cb)(double timestamp_ms, void* userdata);
long long whaleui_set_timeout(whaleui_timer_cb cb, void* userdata, double delay_ms);
long long whaleui_set_interval(whaleui_timer_cb cb, void* userdata, double interval_ms);
void whaleui_clear_timeout(long long id);   /* 两种 id 都接受(浏览器语义) */
void whaleui_clear_interval(long long id);
int whaleui_timer_count(void);
```

回调在事件循环线程(渲染 + DOM 事件同一线程)执行,可直接操作 DOM。
**定时器会让事件循环保持唤醒**:工作线程只睡到下一个到期时刻。
`set_interval` 从到期时刻重排(不因回调耗时漂移);`interval_ms == 0`
每次循环触发一次。

## requestAnimationFrame

```c
typedef void (*whaleui_frame_cb)(double timestamp_ms, void* userdata);
long long whaleui_request_animation_frame(whaleui_frame_cb cb, void* userdata);
void whaleui_cancel_animation_frame(long long id);
int whaleui_animation_frame_count(void);
```

回调在**每帧绘制之后**执行一次(与浏览器语义一致:回调看到它被调度的
那一帧),一次性;回调内可再次 request 以持续。有未触发的 rAF 时事件循环
保持唤醒。

## 可视视口与像素比

```c
typedef struct whaleui_visual_viewport {
    float width, height;           /* 可视视口尺寸(CSS px) */
    float scale;                   /* 1.0 = 无捏合缩放 */
    float offset_left, offset_top; /* 可视相对布局视口 */
    float page_left, page_top;     /* 可视相对文档 */
} whaleui_visual_viewport_t;

int whaleui_window_visual_viewport(const whaleui_window_t* win, whaleui_visual_viewport_t* out);
float whaleui_window_device_pixel_ratio(const whaleui_window_t* win);
```

引擎没有捏合缩放,可视视口等于布局视口(`scale == 1.0`、偏移为 0)。
窗口 resize 时会在 documentElement 上派发 `resize`。

## Web Audio(占位)

音频输出按 `rules/goal.md` 暂缓。这些函数存在以便能力探测与冻结接口形状;
除状态/时钟查询外全部返回不支持,且不创建设备。

```c
typedef struct whaleui_audio_context whaleui_audio_context_t;
typedef enum whaleui_audio_state {
    WHALEUI_AUDIO_SUSPENDED = 0, WHALEUI_AUDIO_RUNNING, WHALEUI_AUDIO_CLOSED
} whaleui_audio_state_t;

int whaleui_audio_is_supported(void);   /* 当前恒为 0(未实现) */
whaleui_audio_context_t* whaleui_audio_context_create(void);
void whaleui_audio_context_destroy(whaleui_audio_context_t* ctx);
whaleui_audio_state_t whaleui_audio_context_state(const whaleui_audio_context_t* ctx);
int whaleui_audio_context_resume(whaleui_audio_context_t* ctx);   /* 恒失败 */
int whaleui_audio_context_suspend(whaleui_audio_context_t* ctx);
int whaleui_audio_context_close(whaleui_audio_context_t* ctx);
float  whaleui_audio_context_sample_rate(const whaleui_audio_context_t* ctx);  /* 48000 */
double whaleui_audio_context_current_time(const whaleui_audio_context_t* ctx); /* 0.0 */
```

## Gamepad(navigator.getGamepads 子集)

SDL3 提供完整手柄支持,这里暴露浏览器形状的子集。索引是**稠密**的
(0..count-1,连接期间稳定);不镜像 W3C 的稀疏索引行为。连接/断开会在
documentElement 上派发 `gamepadconnected`/`gamepaddisconnected`。

```c
int whaleui_gamepad_count(void);
int whaleui_gamepad_connected(int index);
const char* whaleui_gamepad_name(int index);  /* "" 未知 */

#define WHALEUI_GAMEPAD_MAX_AXES 8
#define WHALEUI_GAMEPAD_MAX_BUTTONS 32
typedef struct whaleui_gamepad_state {
    int connected;
    int axis_count;
    float axes[WHALEUI_GAMEPAD_MAX_AXES];   /* -1.0..1.0 */
    int button_count;
    unsigned int buttons;                   /* 位 i = 按钮 i 按下(W3C 顺序) */
    float trigger_left;                     /* 0..1 (L2) */
    float trigger_right;                    /* 0..1 (R2) */
} whaleui_gamepad_state_t;

int whaleui_gamepad_poll(int index, whaleui_gamepad_state_t* out);
int whaleui_gamepad_rumble(int index, float low_freq, float high_freq, unsigned int duration_ms);
```

按钮位顺序按 W3C:`0` A/south、`1` B/east、`2` X/west、`3` Y/north、
`4/5` 肩键、`6/7` 摇杆按下、`8` Back、`9` Start、`10-13` 方向键、`14` Guide。

## 事件载荷(修饰键与富字段)

```c
enum {
    WHALEUI_MOD_NONE = 0, WHALEUI_MOD_SHIFT = 1, WHALEUI_MOD_CTRL = 2,
    WHALEUI_MOD_ALT = 4, WHALEUI_MOD_META = 8
};
int whaleui_dom_event_mods(const whaleui_dom_event_t* ev);
int whaleui_dom_event_ctrl_key(const whaleui_dom_event_t* ev);
int whaleui_dom_event_shift_key(const whaleui_dom_event_t* ev);
int whaleui_dom_event_alt_key(const whaleui_dom_event_t* ev);
int whaleui_dom_event_meta_key(const whaleui_dom_event_t* ev);

const char* whaleui_dom_event_key(const whaleui_dom_event_t* ev);   /* KeyboardEvent.key */
const char* whaleui_dom_event_code(const whaleui_dom_event_t* ev);  /* KeyboardEvent.code */
int whaleui_dom_event_repeat(const whaleui_dom_event_t* ev);
int whaleui_dom_event_detail(const whaleui_dom_event_t* ev);        /* 点击次数 */
float whaleui_dom_event_movement_x(const whaleui_dom_event_t* ev);  /* MouseEvent.movementX */
float whaleui_dom_event_movement_y(const whaleui_dom_event_t* ev);
whaleui_dom_element_t* whaleui_dom_event_related_target(const whaleui_dom_event_t* ev);
const char* whaleui_dom_event_clipboard_text(const whaleui_dom_event_t* ev);
int whaleui_dom_event_set_clipboard_text(whaleui_dom_event_t* ev, const char* text);
```

`key`/`code` 由引擎从 SDL keycode 映射(可打印 ASCII、Enter/Escape/Tab/
方向键/F1-F24/修饰键等)。事件循环派发的 keydown/keyup、mousemove、
mouseenter/leave、click/dblclick/contextmenu 均携带上述字段。

## 拖放(drag & drop)

两个来源,同一套模型:

1. **从系统拖入窗口**(从资源管理器/访达/其他程序拖文件或文本):SDL 交付
   `SDL_EVENT_DROP_*`,引擎命中测试后在指针下的元素上派发
   `dragenter`/`dragover`/`drop`/`dragleave`,DataTransfer 里带文件
   (`DROP_FILE`)或文本(`DROP_TEXT`)。
2. **页内 HTML5 拖放**:`draggable="true"` 的元素按下并拖过阈值后开始会话——
   源上 `dragstart`,指针下元素上 `dragenter`/`dragover`/`dragleave`,
   在 dragover 里 `preventDefault` 的目标收到 `drop`,源上总是收到 `dragend`。

**引擎对两者都不做默认行为**(浏览器会导航到拖入的文件):页面自己处理 `drop`。
在 `dragover` 监听器里 `preventDefault` 就是"把该元素变成放置目标"的契约,与
浏览器一致。

### File

```c
const char* whaleui_file_name(const whaleui_file_t* f);  /* 文件名 */
const char* whaleui_file_uri(const whaleui_file_t* f);   /* 拖入时的 URI */
long long   whaleui_file_size(const whaleui_file_t* f);  /* 字节数,-1 读不到 */
int whaleui_file_read(const whaleui_file_t* f, char** out, size_t* outlen); /* malloc'd,调用方 free */
```

文件字节**惰性读取**(拖入一个 2GB 文件不读就零成本);`size` 首次调用时经 VFS
测量并缓存。

### DataTransfer

```c
whaleui_data_transfer_t* whaleui_data_transfer_create(void);
void whaleui_data_transfer_destroy(whaleui_data_transfer_t* dt);

int whaleui_data_transfer_set_data(whaleui_data_transfer_t* dt, const char* format, const char* data);
const char* whaleui_data_transfer_get_data(const whaleui_data_transfer_t* dt, const char* format);
int whaleui_data_transfer_has_format(const whaleui_data_transfer_t* dt, const char* format);
int whaleui_data_transfer_clear_data(whaleui_data_transfer_t* dt);
size_t whaleui_data_transfer_type_count(const whaleui_data_transfer_t* dt);
const char* whaleui_data_transfer_type_item(const whaleui_data_transfer_t* dt, size_t index);

int whaleui_data_transfer_set_text(whaleui_data_transfer_t* dt, const char* text);
const char* whaleui_data_transfer_get_text(const whaleui_data_transfer_t* dt);

size_t whaleui_data_transfer_file_count(const whaleui_data_transfer_t* dt);
const whaleui_file_t* whaleui_data_transfer_file_item(const whaleui_data_transfer_t* dt, size_t index);
int whaleui_data_transfer_add_file(whaleui_data_transfer_t* dt, const char* name, const char* uri);

/* effectAllowed / dropEffect: 0 none, 1 copy, 2 move, 3 link */
int  whaleui_data_transfer_effect_allowed(const whaleui_data_transfer_t* dt);
void whaleui_data_transfer_set_effect_allowed(whaleui_data_transfer_t* dt, int effect);
int  whaleui_data_transfer_drop_effect(const whaleui_data_transfer_t* dt);
void whaleui_data_transfer_set_drop_effect(whaleui_data_transfer_t* dt, int effect);
```

`set_data` 对同 format 是**就地替换**(保持插入顺序)。`type_item` 按插入顺序
返回 format。事件携带的 DataTransfer 由引擎持有,**监听器不要 destroy**;要
合成拖放请自行 create。

### 事件侧

```c
/* drag/drop 事件上的 transfer(NULL 表示非 drag 事件);派发期间有效 */
whaleui_data_transfer_t* whaleui_dom_event_data_transfer(const whaleui_dom_event_t* ev);

/* 该窗口是否有进行中的页内拖放会话;以及拖放源 */
int whaleui_window_drag_active(const whaleui_window_t* win);
whaleui_dom_element_t* whaleui_window_drag_source(const whaleui_window_t* win);
```

页内拖放默认把源元素的 `textContent` 预填为 `text/plain`(浏览器用选区;
textContent 是最接近的等价物),`dragstart` 监听器可替换。`draggable` 只认显式
属性(浏览器里 img/a 默认可拖,本引擎的 UA 样式不表达这点——已在实现注释写明)。
