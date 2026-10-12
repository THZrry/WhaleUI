/* WebGPU: public C API stub backend (navigator.gpu C-port).
 *
 * Phase 6 milestone is API-first: the header surface mirrors the WebGPU
 * minimum closed loop (adapter/device/queue/buffer/texture/canvas-context)
 * so bindings and pages can feature-detect. The stub reports the SDL_GPU
 * backend when a GPU device exists, else NONE; wgpu-native wiring is the
 * follow-up without header changes. */

#include "whaleui.h"

#include <cstddef>
#include <cstring>

struct whaleui_gpu_adapter { int backend; };
struct whaleui_gpu_device { int backend; whaleui_gpu_queue_t* queue; };
struct whaleui_gpu_queue { int backend; };
struct whaleui_gpu_buffer { unsigned long long size; unsigned int usage; };
struct whaleui_gpu_texture { unsigned int w, h, format; };
struct whaleui_gpu_canvas_context {
    whaleui_dom_element_t* el;
    whaleui_gpu_device_t* device;
    unsigned int format;
    unsigned int alpha_mode;
};

extern "C" whaleui_gpu_backend_t whaleui_gpu_backend(void)
{
#ifdef WHALEUI_BUILD_MINIMAL
    return WHALEUI_GPU_BACKEND_NONE;
#else
    return WHALEUI_GPU_BACKEND_SDL_GPU;
#endif
}

extern "C" int whaleui_gpu_is_supported(void)
{
#ifdef WHALEUI_BUILD_MINIMAL
    return 0;
#else
    return 1;
#endif
}

extern "C" int whaleui_gpu_request_adapter(whaleui_gpu_adapter_t** out)
{
    if (!out) return -1;
#ifdef WHALEUI_BUILD_MINIMAL
    *out = nullptr;
    return -2;
#else
    whaleui_gpu_adapter_t* a = new whaleui_gpu_adapter_t;
    a->backend = (int)WHALEUI_GPU_BACKEND_SDL_GPU;
    *out = a;
    return 0;
#endif
}

extern "C" void whaleui_gpu_adapter_destroy(whaleui_gpu_adapter_t* a)
{
    delete a;
}

extern "C" int whaleui_gpu_adapter_limits(const whaleui_gpu_adapter_t* a, whaleui_gpu_limits_t* out)
{
    if (!a || !out) return -1;
    out->max_texture_dimension_2d = 8192;
    out->max_buffer_size = 256u * 1024u * 1024u;
    return 0;
}

extern "C" int whaleui_gpu_adapter_request_device(whaleui_gpu_adapter_t* a, whaleui_gpu_device_t** out)
{
    if (!a || !out) return -1;
    whaleui_gpu_device_t* d = new whaleui_gpu_device_t;
    d->backend = a->backend;
    d->queue = new whaleui_gpu_queue_t;
    d->queue->backend = a->backend;
    *out = d;
    return 0;
}

extern "C" void whaleui_gpu_device_destroy(whaleui_gpu_device_t* d)
{
    if (!d) return;
    delete d->queue;
    delete d;
}

extern "C" whaleui_gpu_queue_t* whaleui_gpu_device_queue(whaleui_gpu_device_t* d)
{
    return d ? d->queue : nullptr;
}

extern "C" whaleui_gpu_buffer_t* whaleui_gpu_device_create_buffer(whaleui_gpu_device_t* d,
                                                                  unsigned long long size, unsigned int usage)
{
    if (!d || size == 0) return nullptr;
    whaleui_gpu_buffer_t* b = new whaleui_gpu_buffer_t;
    b->size = size;
    b->usage = usage;
    return b;
}

extern "C" void whaleui_gpu_buffer_destroy(whaleui_gpu_buffer_t* b)
{
    delete b;
}

extern "C" int whaleui_gpu_queue_write_buffer(whaleui_gpu_queue_t* q, whaleui_gpu_buffer_t* b,
                                              unsigned long long offset, const void* data, size_t size)
{
    if (!q || !b || !data || size == 0) return -1;
    if (offset + size > b->size) return -2;
    /* stub: accept without a real upload; wgpu-native wires the copy */
    return 0;
}

extern "C" whaleui_gpu_texture_t* whaleui_gpu_device_create_texture_2d(whaleui_gpu_device_t* d,
                                                                       unsigned int w, unsigned int h, unsigned int format)
{
    if (!d || w == 0 || h == 0) return nullptr;
    whaleui_gpu_texture_t* t = new whaleui_gpu_texture_t;
    t->w = w; t->h = h; t->format = format;
    return t;
}

extern "C" void whaleui_gpu_texture_destroy(whaleui_gpu_texture_t* t)
{
    delete t;
}

extern "C" whaleui_gpu_canvas_context_t* whaleui_gpu_canvas_get_context(whaleui_dom_element_t* el)
{
    if (!el) return nullptr;
    whaleui_gpu_canvas_context_t* c = new whaleui_gpu_canvas_context_t;
    c->el = el;
    c->device = nullptr;
    c->format = 0;
    c->alpha_mode = 0;
    return c;
}

extern "C" int whaleui_gpu_canvas_configure(whaleui_gpu_canvas_context_t* c, whaleui_gpu_device_t* d,
                                            unsigned int format, unsigned int alpha_mode)
{
    if (!c || !d) return -1;
    c->device = d;
    c->format = format;
    c->alpha_mode = alpha_mode;
    return 0;
}

extern "C" void whaleui_gpu_canvas_context_destroy(whaleui_gpu_canvas_context_t* c)
{
    delete c;
}
