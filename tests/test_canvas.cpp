// test_canvas: Canvas 2D + WebGPU stub contract tests.
#include "whaleui.h"

#include <cassert>
#include <cstring>

int main(void)
{
    /* canvas element + 2d context */
    {
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html("<canvas id=\"c\" width=\"4\" height=\"4\"></canvas>", 0);
        assert(doc != nullptr);
        whaleui_dom_element_t* c = whaleui_dom_get_element_by_id(doc, "c");
        assert(c != nullptr);

        int w = 0, h = 0;
        assert(whaleui_canvas_get_size(c, &w, &h) == 0);
        assert(w == 4 && h == 4);

        whaleui_canvas_context_t* ctx = whaleui_canvas_get_context(c, "2d");
        assert(ctx != nullptr);
        assert(whaleui_canvas_get_context(c, "webgpu") == nullptr);

        whaleui_dom_element_t* div = whaleui_dom_create_element(doc, "div");
        assert(whaleui_canvas_get_context(div, "2d") == nullptr);
        assert(whaleui_canvas_get_context(nullptr, "2d") == nullptr);

        /* fillRect red then read back */
        assert(whaleui_canvas_set_fill_style(ctx, "#FF0000") == 0);
        assert(whaleui_canvas_fill_rect(ctx, 0, 0, 4, 4) == 0);
        whaleui_image_data_t* img = whaleui_canvas_get_image_data(ctx, 0, 0, 4, 4);
        assert(img != nullptr);
        int iw = 0, ih = 0;
        assert(whaleui_image_data_size(img, &iw, &ih) == 0);
        assert(iw == 4 && ih == 4);
        assert(whaleui_image_data_get(img, 0, 0) == 0xFFFF0000u);
        assert(whaleui_image_data_get(img, 3, 3) == 0xFFFF0000u);

        /* clear a pixel -> transparent */
        assert(whaleui_canvas_clear_rect(ctx, 0, 0, 1, 1) == 0);
        whaleui_image_data_t* img2 = whaleui_canvas_get_image_data(ctx, 0, 0, 1, 1);
        assert(img2 != nullptr);
        assert(whaleui_image_data_get(img2, 0, 0) == 0x00000000u);
        whaleui_image_data_destroy(img2);

        /* putImageData round-trip */
        assert(whaleui_image_data_set(img, 1, 1, 0xFF00FF00u) == 0);
        assert(whaleui_canvas_put_image_data(ctx, img, 0, 0) == 0);
        whaleui_image_data_t* img3 = whaleui_canvas_get_image_data(ctx, 1, 1, 1, 1);
        assert(img3 != nullptr);
        assert(whaleui_image_data_get(img3, 0, 0) == 0xFF00FF00u);
        whaleui_image_data_destroy(img3);
        whaleui_image_data_destroy(img);

        /* path fill/stroke + transform/save/restore */
        assert(whaleui_canvas_begin_path(ctx) == 0);
        assert(whaleui_canvas_move_to(ctx, 0, 0) == 0);
        assert(whaleui_canvas_line_to(ctx, 3, 0) == 0);
        assert(whaleui_canvas_line_to(ctx, 3, 3) == 0);
        assert(whaleui_canvas_close_path(ctx) == 0);
        assert(whaleui_canvas_set_fill_style(ctx, "#0000FF") == 0);
        assert(whaleui_canvas_fill(ctx) == 0);
        assert(whaleui_canvas_stroke(ctx) == 0);
        assert(whaleui_canvas_save(ctx) == 0);
        assert(whaleui_canvas_translate(ctx, 1, 0) == 0);
        assert(whaleui_canvas_scale(ctx, 1, 1) == 0);
        assert(whaleui_canvas_rotate(ctx, 0.0f) == 0);
        assert(whaleui_canvas_restore(ctx) == 0);
        assert(whaleui_canvas_restore(ctx) != 0);
        assert(whaleui_canvas_rect(ctx, 0, 0, 2, 2) == 0);
        assert(whaleui_canvas_arc(ctx, 2, 2, 1, 0, 6.28f) == 0);
        assert(whaleui_canvas_stroke_rect(ctx, 0, 0, 2, 2) == 0);
        assert(whaleui_canvas_set_line_width(ctx, 2.0f) == 0);
        assert(whaleui_canvas_set_global_alpha(ctx, 0.5f) == 0);
        assert(whaleui_canvas_set_global_alpha(ctx, 2.0f) != 0);

        /* resize resets backing */
        assert(whaleui_canvas_set_size(c, 2, 2) == 0);
        assert(whaleui_canvas_get_size(c, &w, &h) == 0);
        assert(w == 2 && h == 2);
        whaleui_image_data_t* img4 = whaleui_canvas_get_image_data(ctx, 0, 0, 2, 2);
        assert(img4 != nullptr);
        assert(whaleui_image_data_get(img4, 0, 0) == 0x00000000u);
        whaleui_image_data_destroy(img4);

        /* createImageData standalone */
        whaleui_image_data_t* blank = whaleui_canvas_create_image_data(ctx, 2, 3);
        assert(blank != nullptr);
        assert(whaleui_image_data_size(blank, &iw, &ih) == 0);
        assert(iw == 2 && ih == 3);
        whaleui_image_data_destroy(blank);
        assert(whaleui_canvas_create_image_data(ctx, 0, 1) == nullptr);

        /* text state setters (validation) */
        assert(whaleui_canvas_set_font(ctx, "bold 16px Arial, sans-serif") == 0);
        assert(whaleui_canvas_set_font(ctx, nullptr) != 0);
        assert(whaleui_canvas_set_text_align(ctx, "center") == 0);
        assert(whaleui_canvas_set_text_align(ctx, "sideways") != 0);
        assert(whaleui_canvas_set_text_baseline(ctx, "middle") == 0);
        assert(whaleui_canvas_set_text_baseline(ctx, "sideways") != 0);

        /* text measure + fill: needs the SDL3_ttf backend (full) and a
         * registered font; assert the closed loop where available. TTF
         * lives behind app_create (platform_init -> TTF_Init), so spin up an
         * app + the platform fonts just for this section. */
        whaleui_app_t* tapp = whaleui_app_create();
        if (tapp) {
            whaleui_font_register_system_defaults();
        }
        {
            int tw = 0, th = 0;
            int mrc = whaleui_canvas_measure_text(ctx, "Hello", &tw, &th);
#ifdef WHALEUI_BUILD_FULL
            assert(tapp != nullptr); /* TTF_Init + system fonts present */
            assert(mrc == 0);
            assert(tw > 0 && th > 0);
            /* a longer string measures wider */
            int tw2 = 0, th2 = 0;
            assert(whaleui_canvas_measure_text(ctx, "Hello world", &tw2,
                                               &th2) == 0);
            assert(tw2 > tw);
            /* a bigger size measures wider */
            assert(whaleui_canvas_set_font(ctx, "32px Arial") == 0);
            int tw3 = 0, th3 = 0;
            assert(whaleui_canvas_measure_text(ctx, "Hello", &tw3, &th3) == 0);
            assert(tw3 > tw);
            assert(whaleui_canvas_set_font(ctx, "16px Arial") == 0);
            /* fillText paints ink somewhere in the box */
            assert(whaleui_canvas_set_size(c, 200, 60) == 0);
            assert(whaleui_canvas_set_fill_style(ctx, "#FFFFFF") == 0);
            assert(whaleui_canvas_fill_text(ctx, "Hello", 2, 40) == 0);
            whaleui_image_data_t* timg =
                whaleui_canvas_get_image_data(ctx, 0, 0, 200, 60);
            assert(timg != nullptr);
            int ink = 0;
            for (int y = 0; y < 60; ++y)
                for (int x = 0; x < 200; ++x)
                    if ((whaleui_image_data_get(timg, x, y) >> 24) != 0) ++ink;
            assert(ink > 0);
            whaleui_image_data_destroy(timg);
            /* strokeText also paints (outline) */
            assert(whaleui_canvas_clear_rect(ctx, 0, 0, 200, 60) == 0);
            assert(whaleui_canvas_set_stroke_style(ctx, "#FF0000") == 0);
            assert(whaleui_canvas_stroke_text(ctx, "Hi", 2, 40) == 0);
            whaleui_image_data_t* simg =
                whaleui_canvas_get_image_data(ctx, 0, 0, 200, 60);
            assert(simg != nullptr);
            int sink = 0;
            for (int y = 0; y < 60; ++y)
                for (int x = 0; x < 200; ++x)
                    if ((whaleui_image_data_get(simg, x, y) >> 24) != 0) ++sink;
            assert(sink > 0);
            whaleui_image_data_destroy(simg);
            /* textAlign center shifts the painted run right of a left run */
            assert(whaleui_canvas_clear_rect(ctx, 0, 0, 200, 60) == 0);
            assert(whaleui_canvas_set_text_align(ctx, "start") == 0);
            assert(whaleui_canvas_fill_text(ctx, "M", 2, 40) == 0);
            int left_ink = 0;
            whaleui_image_data_t* li = whaleui_canvas_get_image_data(ctx, 0, 0, 100, 60);
            for (int y = 0; y < 60; ++y)
                for (int x = 0; x < 100; ++x)
                    if ((whaleui_image_data_get(li, x, y) >> 24) != 0) ++left_ink;
            whaleui_image_data_destroy(li);
            assert(whaleui_canvas_clear_rect(ctx, 0, 0, 200, 60) == 0);
            assert(whaleui_canvas_set_text_align(ctx, "center") == 0);
            assert(whaleui_canvas_fill_text(ctx, "M", 100, 40) == 0);
            int ctr_ink = 0;
            whaleui_image_data_t* ci = whaleui_canvas_get_image_data(ctx, 100, 0, 100, 60);
            for (int y = 0; y < 60; ++y)
                for (int x = 0; x < 100; ++x)
                    if ((whaleui_image_data_get(ci, x, y) >> 24) != 0) ++ctr_ink;
            whaleui_image_data_destroy(ci);
            assert(ctr_ink > 0); /* center lands on the right half of the box */
            assert(whaleui_canvas_set_text_align(ctx, "start") == 0);
#else
            assert(mrc == -2); /* lite/minimal: no TTF canvas backend yet */
#endif
            assert(whaleui_canvas_measure_text(nullptr, "x", &tw, &th) != 0);
            assert(whaleui_canvas_fill_text(ctx, nullptr, 0, 0) != 0);
        }
        if (tapp) {
            whaleui_app_destroy(tapp);
        }

        whaleui_dom_document_destroy(doc);
    }

    /* webgpu stub closed loop */
    {
        whaleui_gpu_backend_t be = whaleui_gpu_backend();
        assert(be == WHALEUI_GPU_BACKEND_SDL_GPU || be == WHALEUI_GPU_BACKEND_NONE ||
               be == WHALEUI_GPU_BACKEND_WGPU_NATIVE);
        whaleui_gpu_adapter_t* a = nullptr;
        int rc = whaleui_gpu_request_adapter(&a);
#ifdef WHALEUI_BUILD_MINIMAL
        assert(rc != 0 && a == nullptr);
        assert(whaleui_gpu_is_supported() == 0);
#else
        assert(rc == 0 && a != nullptr);
        assert(whaleui_gpu_is_supported() == 1);
        whaleui_gpu_limits_t lim;
        assert(whaleui_gpu_adapter_limits(a, &lim) == 0);
        assert(lim.max_texture_dimension_2d >= 1024);
        whaleui_gpu_device_t* d = nullptr;
        assert(whaleui_gpu_adapter_request_device(a, &d) == 0);
        assert(d != nullptr);
        whaleui_gpu_queue_t* q = whaleui_gpu_device_queue(d);
        assert(q != nullptr);
        whaleui_gpu_buffer_t* b = whaleui_gpu_device_create_buffer(d, 64, 1);
        assert(b != nullptr);
        char payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        assert(whaleui_gpu_queue_write_buffer(q, b, 0, payload, sizeof(payload)) == 0);
        assert(whaleui_gpu_queue_write_buffer(q, b, 60, payload, sizeof(payload)) != 0);
        whaleui_gpu_buffer_destroy(b);
        whaleui_gpu_texture_t* t = whaleui_gpu_device_create_texture_2d(d, 4, 4, 1);
        assert(t != nullptr);
        whaleui_gpu_texture_destroy(t);
        whaleui_dom_document_t* doc =
            whaleui_dom_parse_html("<canvas id=\"g\"></canvas>", 0);
        assert(doc != nullptr);
        whaleui_dom_element_t* c = whaleui_dom_get_element_by_id(doc, "g");
        assert(c != nullptr);
        whaleui_gpu_canvas_context_t* gc = whaleui_gpu_canvas_get_context(c);
        assert(gc != nullptr);
        assert(whaleui_gpu_canvas_configure(gc, d, 1, 0) == 0);
        whaleui_gpu_canvas_context_destroy(gc);
        whaleui_dom_document_destroy(doc);
        whaleui_gpu_device_destroy(d);
        whaleui_gpu_adapter_destroy(a);
#endif
        whaleui_gpu_adapter_destroy(nullptr);
    }

    return 0;
}
