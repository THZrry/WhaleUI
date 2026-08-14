/* GPU renderer implementation (see gpu.h). */

#include "render/gpu.h"
#include "render/gpu_shaders.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#ifdef _WIN32
#include <windows.h>
#endif
#include <dxcapi.h>

#include <cstring>
#include <cstdio>
#include <string>
#include <cwchar>

namespace {

/* mingw lacks __uuidof for the dxcapi interfaces, so carry the IIDs
 * explicitly (values from dxcapi.h CROSS_PLATFORM_UUIDOF) */
#ifdef _WIN32
static const IID IID_DXC_Compiler3 = {
    0x228B4687, 0x5A6A, 0x4730, {0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54}};
static const IID IID_DXC_Result = {
    0x58346CDA, 0xDDE7, 0x4497, {0x94, 0x61, 0x6F, 0x87, 0xAF, 0x5E, 0x06, 0x59}};
static const IID IID_DXC_Blob = {
    0x8BA5FB08, 0x5195, 0x40e2, {0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02}};
#endif

/* DXC (dxcompiler.dll, from the Microsoft DirectXShaderCompiler package):
 * compiles the HLSL sources to DXIL for the D3D12 backend at runtime -
 * one intermediate source, no per-backend shader copies. */
#ifdef _WIN32
HMODULE g_dxc_dll = nullptr;
DxcCreateInstanceProc g_dxc_create = nullptr;

bool dxc_init()
{
    if (g_dxc_create) {
        return true;
    }
    g_dxc_dll = LoadLibraryA("dxcompiler.dll");
    if (!g_dxc_dll) {
        g_dxc_dll = LoadLibraryA("3rdparty/dxc/bin/dxcompiler.dll");
    }
    if (!g_dxc_dll) {
        std::fprintf(stderr, "[gpu] dxcompiler.dll not found (DXC)\n");
        return false;
    }
    g_dxc_create = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(g_dxc_dll, "DxcCreateInstance"));
    return g_dxc_create != nullptr;
}
#endif

/* compile HLSL to DXIL via DXC; returns a blob the caller owns (Release).
 * profile: L"vs_6_0" / L"ps_6_0" / L"cs_6_0". When spirv is set, emits
 * SPIR-V (-spirv) with the given -fvk-bind extras. */
IDxcBlob* compile_dxil(const char* hlsl, const char* entry,
                       const wchar_t* profile, bool spirv,
                       const wchar_t* const* extra, int nextra)
{
#ifdef _WIN32
    if (!dxc_init()) {
        return nullptr;
    }
    IDxcCompiler3* compiler = nullptr;
    HRESULT hr = g_dxc_create(CLSID_DxcCompiler, IID_DXC_Compiler3,
                              reinterpret_cast<void**>(&compiler));
    if (FAILED(hr) || !compiler) {
        std::fprintf(stderr, "[gpu] DxcCreateInstance(Compiler) failed: 0x%lx\n",
                     static_cast<unsigned long>(hr));
        return nullptr;
    }
    DxcBuffer buf;
    buf.Ptr = hlsl;
    buf.Size = std::strlen(hlsl);
    buf.Encoding = DXC_CP_UTF8;
    std::wstring entry_w(entry, entry + std::strlen(entry));
    std::vector<const wchar_t*> args;
    args.push_back(L"-T");
    args.push_back(profile);
    args.push_back(L"-E");
    args.push_back(entry_w.c_str());
    if (spirv) {
        args.push_back(L"-spirv");
        for (int i = 0; i < nextra; ++i) {
            args.push_back(extra[i]);
        }
    }
    IDxcResult* result = nullptr;
    hr = compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()),
                           nullptr, IID_DXC_Result,
                          reinterpret_cast<void**>(&result));
    compiler->Release();
    if (FAILED(hr) || !result) {
        std::fprintf(stderr, "[gpu] DXC Compile(%s) failed: 0x%lx\n", entry,
                     static_cast<unsigned long>(hr));
        return nullptr;
    }
    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        IDxcBlobEncoding* errb = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_DXC_Blob,
                                        reinterpret_cast<void**>(&errb),
                                        nullptr)) &&
            errb && errb->GetBufferSize() > 0) {
            std::fprintf(stderr, "[gpu] DXC %s error: %.*s\n", entry,
                         static_cast<int>(errb->GetBufferSize()),
                         static_cast<const char*>(errb->GetBufferPointer()));
            errb->Release();
        } else {
            std::fprintf(stderr, "[gpu] DXC %s error: 0x%lx\n", entry,
                         static_cast<unsigned long>(status));
        }
        result->Release();
        return nullptr;
    }
    IDxcBlob* blob = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, IID_DXC_Blob,
                      reinterpret_cast<void**>(&blob), nullptr);
    result->Release();
    if (!blob) {
        std::fprintf(stderr, "[gpu] DXC %s: no object blob\n", entry);
        return nullptr;
    }
    return blob;
#else
    (void)hlsl;
    (void)entry;
    (void)profile;
    return nullptr;
#endif
}

SDL_GPUShader* compile_shader(SDL_GPUDevice* dev, const char* hlsl,
                              const char* entry, bool fragment)
{
#ifdef _WIN32
    const char* drv = SDL_GetGPUDeviceDriver(dev);
    const bool vulkan = drv && std::strcmp(drv, "vulkan") == 0;
    const std::wstring profile = fragment ? L"ps_6_0" : L"vs_6_0";
    const wchar_t* fbBind[5] = {L"-fvk-bind", L"b0", L"2", L"0"};
    IDxcBlob* blob =
        compile_dxil(hlsl, entry, profile.c_str(), vulkan,
                     vulkan ? fbBind : nullptr, vulkan ? 4 : 0);
    if (!blob) {
        return nullptr;
    }
    SDL_GPUShaderCreateInfo sci;
    std::memset(&sci, 0, sizeof(sci));
    sci.code = static_cast<const Uint8*>(blob->GetBufferPointer());
    sci.code_size = blob->GetBufferSize();
    sci.entrypoint = entry;
    sci.format = vulkan ? SDL_GPU_SHADERFORMAT_SPIRV
                        : SDL_GPU_SHADERFORMAT_DXIL;
    sci.stage = fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT
                         : SDL_GPU_SHADERSTAGE_VERTEX;
    sci.num_samplers = fragment ? 1 : 0;
    SDL_GPUShader* sh = SDL_CreateGPUShader(dev, &sci);
    blob->Release();
    if (!sh) {
        std::fprintf(stderr, "[gpu] SDL_CreateGPUShader(%s) failed: %s\n",
                     entry, SDL_GetError());
    }
    return sh;
#else
    (void)dev;
    (void)hlsl;
    (void)entry;
    (void)fragment;
    return nullptr;
#endif
}

SDL_GPUBuffer* make_vertex_buffer(SDL_GPUDevice* dev, uint32_t bytes)
{
    SDL_GPUBufferCreateInfo bci;
    std::memset(&bci, 0, sizeof(bci));
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = bytes;
    return SDL_CreateGPUBuffer(dev, &bci);
}

SDL_GPUTexture* make_texture(SDL_GPUDevice* dev, uint32_t w, uint32_t h,
                             SDL_GPUTextureFormat fmt, uint32_t usage)
{
    SDL_GPUTextureCreateInfo tci;
    std::memset(&tci, 0, sizeof(tci));
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = fmt;
    tci.width = w;
    tci.height = h;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.usage = usage;
    return SDL_CreateGPUTexture(dev, &tci);
}

} // namespace

whaleui_gpu_t* whaleui_gpu_create(SDL_GPUDevice* device, int w, int h)
{
    if (!device || w <= 0 || h <= 0) {
        return nullptr;
    }
    whaleui_gpu_t* g = new whaleui_gpu_t;
    g->device = device;
    g->pipe_solid = nullptr;
    g->pipe_text = nullptr;
    g->sampler = nullptr;
    g->white_tex = nullptr;
    g->glyph_atlas = nullptr;
    g->target = nullptr;
    g->target2 = nullptr;
    g->vb_solid = nullptr;
    g->vb_text = nullptr;
    g->vb_transfer = nullptr;
    g->atlas_transfer = nullptr;
    g->pipe_text_composite = nullptr;
    g->text_layer = nullptr;
    g->layer_transfer = nullptr;
    g->layer_dirty = 0;
    g->fb_w = static_cast<float>(w);
    g->fb_h = static_cast<float>(h);
    g->atlas_w = 2048;
    g->atlas_h = 2048;
    g->atlas_cx = 0;
    g->atlas_cy = 0;
    g->atlas_row_h = 0;
    g->atlas_dirty = 0;

    /* shaders (HLSL -> DXIL at runtime) */
    SDL_GPUShader* vs_s = compile_shader(device, kSolidVS, "main", false);
    SDL_GPUShader* ps_s = compile_shader(device, kSolidPS, "main", true);
    SDL_GPUShader* vs_t = compile_shader(device, kTextVS, "main", false);
    SDL_GPUShader* ps_t = compile_shader(device, kTextPS, "main", true);
    if (!vs_s || !ps_s || !vs_t || !ps_t) {
        goto fail;
    }

    /* solid pipeline */
    {
        SDL_GPUVertexAttribute attrs[6];
        std::memset(attrs, 0, sizeof(attrs));
        attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0};
        attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8};
        attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16};
        attrs[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 32};
        attrs[4] = {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 40};
        attrs[5] = {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 44};
        SDL_GPUVertexBufferDescription vbd;
        std::memset(&vbd, 0, sizeof(vbd));
        vbd.slot = 0;
        vbd.pitch = sizeof(gpu_vert_solid);
        vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vbd.instance_step_rate = 0;
        SDL_GPUVertexInputState vis;
        std::memset(&vis, 0, sizeof(vis));
        vis.vertex_buffer_descriptions = &vbd;
        vis.num_vertex_buffers = 1;
        vis.vertex_attributes = attrs;
        vis.num_vertex_attributes = 6;

        SDL_GPUColorTargetBlendState blend;
        std::memset(&blend, 0, sizeof(blend));
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.enable_blend = true;

        SDL_GPUColorTargetDescription ct;
        std::memset(&ct, 0, sizeof(ct));
        ct.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        ct.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo info;
        std::memset(&info, 0, sizeof(info));
        info.vertex_shader = vs_s;
        info.fragment_shader = ps_s;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.vertex_input_state = vis;
        info.target_info.num_color_targets = 1;
        info.target_info.color_target_descriptions = &ct;
        g->pipe_solid = SDL_CreateGPUGraphicsPipeline(device, &info);
    }
    if (!g->pipe_solid) {
        goto fail;
    }

    /* sampler (linear for text AA) */
    SDL_GPUSamplerCreateInfo ssi;
    std::memset(&ssi, 0, sizeof(ssi));
    ssi.min_filter = SDL_GPU_FILTER_LINEAR;
    ssi.mag_filter = SDL_GPU_FILTER_LINEAR;
    ssi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ssi.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ssi.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ssi.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g->sampler = SDL_CreateGPUSampler(device, &ssi);
    if (!g->sampler) {
        goto fail;
    }

    /* 1x1 white texture */

    g->white_tex = make_texture(device, 1, 1,
                                SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                                SDL_GPU_TEXTUREUSAGE_SAMPLER);

    g->glyph_atlas = make_texture(device, g->atlas_w, g->atlas_h,
                                  SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                                  SDL_GPU_TEXTUREUSAGE_SAMPLER);

    g->target = make_texture(device, static_cast<uint32_t>(w),
                             static_cast<uint32_t>(h),
                             SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                             SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                 SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ);
    g->target_b = make_texture(device, static_cast<uint32_t>(w),
                               static_cast<uint32_t>(h),
                               SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                               SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                   SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                   SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ);
    g->geom_cur = g->target;

    g->target2 = make_texture(device, static_cast<uint32_t>(w),
                              static_cast<uint32_t>(h),
                              SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                              SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE |
                                  SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                                  SDL_GPU_TEXTUREUSAGE_SAMPLER);

    g->text_layer = make_texture(device, static_cast<uint32_t>(w),
                                 static_cast<uint32_t>(h),
                                 SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                                 SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!g->white_tex || !g->glyph_atlas || !g->target || !g->target_b ||
        !g->target2 || !g->text_layer) {
        goto fail;
    }
    /* upload the white pixel */
    {
        g->atlas.assign(static_cast<size_t>(g->atlas_w) * g->atlas_h, 0);
        SDL_GPUTransferBufferCreateInfo tbi;
        std::memset(&tbi, 0, sizeof(tbi));
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = 4;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device, &tbi);
        if (tb) {
            void* p = SDL_MapGPUTransferBuffer(device, tb, false);
            if (p) {
                std::memcpy(p, kSolidPixel, 4);
                SDL_UnmapGPUTransferBuffer(device, tb);
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
                if (cmd) {
                    SDL_GPUTextureTransferInfo ti;
                    std::memset(&ti, 0, sizeof(ti));
                    ti.transfer_buffer = tb;
                    SDL_GPUTextureRegion reg;
                    std::memset(&reg, 0, sizeof(reg));
                    reg.texture = g->white_tex;
                    reg.w = 1;
                    reg.h = 1;
                    reg.d = 1;
                    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
                    SDL_UploadToGPUTexture(cp, &ti, &reg, false);
                    SDL_EndGPUCopyPass(cp);
                    SDL_SubmitGPUCommandBuffer(cmd);
                }
            }
            SDL_ReleaseGPUTransferBuffer(device, tb);
        }
    }

    /* vertex buffers: fixed capacity, reused every frame */
    g->vb_solid = make_vertex_buffer(device, 256 * 1024);
    g->vb_text = make_vertex_buffer(device, 256 * 1024);
    SDL_GPUTransferBufferCreateInfo tbi;
    std::memset(&tbi, 0, sizeof(tbi));
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = 512 * 1024;
    g->vb_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    tbi.size = 2048 * 2048;
    g->atlas_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    tbi.size = static_cast<uint32_t>(static_cast<size_t>(w) * h * 4);
    g->layer_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    if (!g->vb_solid || !g->vb_text || !g->vb_transfer || !g->atlas_transfer ||
        !g->layer_transfer) {
        goto fail;
    }

    /* text-composite compute pipeline: text_layer (t0,t1) -> target2 (u0) */
    {
        const char* drv = SDL_GetGPUDeviceDriver(device);
        const bool vulkan = drv && std::strcmp(drv, "vulkan") == 0;
        /* SPIR-V layout (SDL 3.4): set0 = sampled/ro-storage, set1 =
         * rw-storage; DXC needs explicit -fvk-bind to land there */
        const wchar_t* binds[12] = {
            L"-fvk-bind", L"t0", L"0", L"0",
            L"-fvk-bind", L"t1", L"0", L"1",
            L"-fvk-bind", L"u0", L"1", L"0"};
        IDxcBlob* cs =
            compile_dxil(kTextCompositeCS, "main", L"cs_6_0", vulkan,
                         vulkan ? binds : nullptr, vulkan ? 12 : 0);
        if (!cs) {
            goto fail;
        }
        SDL_GPUComputePipelineCreateInfo cpi;
        std::memset(&cpi, 0, sizeof(cpi));
        cpi.code = static_cast<const Uint8*>(cs->GetBufferPointer());
        cpi.code_size = cs->GetBufferSize();
        cpi.entrypoint = "main";
        cpi.format = vulkan ? SDL_GPU_SHADERFORMAT_SPIRV
                            : SDL_GPU_SHADERFORMAT_DXIL;
        cpi.num_samplers = 2;
        cpi.num_readwrite_storage_textures = 1;
        cpi.threadcount_x = 16;
        cpi.threadcount_y = 16;
        cpi.threadcount_z = 1;
        g->pipe_text_composite = SDL_CreateGPUComputePipeline(device, &cpi);
        cs->Release();
        if (!g->pipe_text_composite) {
            std::fprintf(stderr, "[gpu] text composite pipeline failed: %s\n",
                         SDL_GetError());
            goto fail;
        }
    }

    SDL_ReleaseGPUShader(device, vs_s);
    SDL_ReleaseGPUShader(device, ps_s);
    return g;

fail:
    whaleui_gpu_destroy(g);
    return nullptr;
}

void whaleui_gpu_destroy(whaleui_gpu_t* g)
{
    if (!g) {
        return;
    }
    if (g->device) {
        if (g->pipe_solid) SDL_ReleaseGPUGraphicsPipeline(g->device, g->pipe_solid);
        if (g->pipe_text) SDL_ReleaseGPUGraphicsPipeline(g->device, g->pipe_text);
        if (g->pipe_text_composite) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_text_composite);
        if (g->sampler) SDL_ReleaseGPUSampler(g->device, g->sampler);
        if (g->white_tex) SDL_ReleaseGPUTexture(g->device, g->white_tex);
        if (g->glyph_atlas) SDL_ReleaseGPUTexture(g->device, g->glyph_atlas);
        if (g->text_layer) SDL_ReleaseGPUTexture(g->device, g->text_layer);
        if (g->target) SDL_ReleaseGPUTexture(g->device, g->target);
        if (g->target_b) SDL_ReleaseGPUTexture(g->device, g->target_b);
        if (g->target2) SDL_ReleaseGPUTexture(g->device, g->target2);
        if (g->vb_solid) SDL_ReleaseGPUBuffer(g->device, g->vb_solid);
        if (g->vb_text) SDL_ReleaseGPUBuffer(g->device, g->vb_text);
        if (g->vb_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->vb_transfer);
        if (g->atlas_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->atlas_transfer);
        if (g->layer_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->layer_transfer);
    }
    delete g;
}

void whaleui_gpu_rect(whaleui_gpu_t* g, float x, float y, float w, float h,
                      float radius, unsigned int color, const int* clip)
{
    if (!g || w <= 0 || h <= 0) {
        return;
    }
    if (clip) {
        float x0 = x < clip[0] ? clip[0] : x;
        float y0 = y < clip[1] ? clip[1] : y;
        float x1 = x + w < clip[0] + clip[2] ? x + w : clip[0] + clip[2];
        float y1 = y + h < clip[1] + clip[3] ? y + h : clip[1] + clip[3];
        if (x1 <= x0 || y1 <= y0) {
            return;
        }
        x = x0;
        y = y0;
        w = x1 - x0;
        h = y1 - y0;
    }
    if (g->solids.size() + 6 > 65536) {
        return; /* vertex buffer cap */
    }
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float gg = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;
    gpu_vert_solid v[6];
    /* two triangles, UV 0..1, same color, size + radius + fb size */
    for (int i = 0; i < 6; ++i) {
        v[i].x = x;
        v[i].y = y;
        v[i].r = r;
        v[i].g = gg;
        v[i].b = b;
        v[i].a = a;
        v[i].size_x = w;
        v[i].size_y = h;
        v[i].radius = radius;
        v[i].fb_w = g->fb_w;
        v[i].fb_h = g->fb_h;
    }
    v[0].x = x;         v[0].y = y;         v[0].u = 0; v[0].v = 0;
    v[1].x = x + w;     v[1].y = y;         v[1].u = 1; v[1].v = 0;
    v[2].x = x;         v[2].y = y + h;     v[2].u = 0; v[2].v = 1;
    v[3].x = x + w;     v[3].y = y;         v[3].u = 1; v[3].v = 0;
    v[4].x = x;         v[4].y = y + h;     v[4].u = 0; v[4].v = 1;
    v[5].x = x + w;     v[5].y = y + h;     v[5].u = 1; v[5].v = 1;
    for (int i = 0; i < 6; ++i) {
        g->solids.push_back(v[i]);
    }
}

void whaleui_gpu_text(whaleui_gpu_t* g, float x, float y, float w, float h,
                      float u, float v, float u2, float v2,
                      unsigned int color)
{
    if (!g || w <= 0 || h <= 0) {
        return;
    }
    if (g->texts.size() + 6 > 65536) {
        return;
    }
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float gg = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;
    gpu_vert_text vt[6];
    for (int i = 0; i < 6; ++i) {
        vt[i].r = r;
        vt[i].g = gg;
        vt[i].b = b;
        vt[i].a = a;
    }
    vt[0].x = x;      vt[0].y = y;      vt[0].u = u;  vt[0].v = v;
    vt[1].x = x + w;  vt[1].y = y;      vt[1].u = u2; vt[1].v = v;
    vt[2].x = x;      vt[2].y = y + h;  vt[2].u = u;  vt[2].v = v2;
    vt[3].x = x + w;  vt[3].y = y;      vt[3].u = u2; vt[3].v = v;
    vt[4].x = x;      vt[4].y = y + h;  vt[4].u = u;  vt[4].v = v2;
    vt[5].x = x + w;  vt[5].y = y + h;  vt[5].u = u2; vt[5].v = v2;
    for (int i = 0; i < 6; ++i) {
        g->texts.push_back(vt[i]);
    }
}

int whaleui_gpu_atlas_alloc(whaleui_gpu_t* g, int w, int h, int* x, int* y)
{
    if (!g || w <= 0 || h <= 0 || w >= static_cast<int>(g->atlas_w)) {
        return -1;
    }
    /* row packing: grow the row height, wrap to the next row on overflow */
    if (g->atlas_cx + w + 1 > static_cast<int>(g->atlas_w)) {
        g->atlas_cx = 0;
        g->atlas_cy += g->atlas_row_h + 1;
        g->atlas_row_h = 0;
    }
    if (g->atlas_cy + h + 1 > static_cast<int>(g->atlas_h)) {
        return -1; /* atlas full */
    }
    *x = g->atlas_cx;
    *y = g->atlas_cy;
    g->atlas_cx += w + 1;
    if (h > g->atlas_row_h) {
        g->atlas_row_h = h;
    }
    return 0;
}

void whaleui_gpu_atlas_dirty(whaleui_gpu_t* g)
{
    if (g) {
        g->atlas_dirty = 1;
    }
}

void whaleui_gpu_text_layer(whaleui_gpu_t* g, const unsigned int* pixels,
                            int w, int h)
{
    if (!g || !pixels || w <= 0 || h <= 0 || !g->layer_transfer) {
        return;
    }
    size_t bytes = static_cast<size_t>(w) * h * 4;
    void* mapped = SDL_MapGPUTransferBuffer(g->device, g->layer_transfer, false);
    if (!mapped) {
        return;
    }
    /* CPU pixels are 0xAARRGGBB; the RGBA8 layer wants R,G,B,A */
    const unsigned char* src = reinterpret_cast<const unsigned char*>(pixels);
    unsigned char* dst = static_cast<unsigned char*>(mapped);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }
    SDL_UnmapGPUTransferBuffer(g->device, g->layer_transfer);
    g->layer_dirty = 1;
}

SDL_GPUCommandBuffer* whaleui_gpu_flush(whaleui_gpu_t* g, int fb_w, int fb_h,
                                        unsigned int clear_color, int scroll_dy,
                                        int load_only)
{
    if (!g) {
        return nullptr;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g->device);
    if (!cmd) {
        return nullptr;
    }

    /* scroll: shift the previous geometry by blitting it into the other
     * ping-pong target (newly exposed strip is repainted by the caller) */
    if (scroll_dy != 0 && g->target_b) {
        SDL_GPUTexture* dst = (g->geom_cur == g->target) ? g->target_b : g->target;
        int strip = scroll_dy < 0 ? -scroll_dy : scroll_dy;
        if (strip < fb_h) {
            SDL_GPUBlitInfo blit;
            std::memset(&blit, 0, sizeof(blit));
            blit.source.texture = g->geom_cur;
            blit.source.x = 0;
            blit.source.y = scroll_dy > 0 ? static_cast<Uint32>(scroll_dy) : 0;
            blit.source.w = static_cast<Uint32>(fb_w);
            blit.source.h = static_cast<Uint32>(fb_h - strip);
            blit.destination.texture = dst;
            blit.destination.x = 0;
            blit.destination.y = scroll_dy > 0 ? 0 : static_cast<Uint32>(-scroll_dy);
            blit.destination.w = static_cast<Uint32>(fb_w);
            blit.destination.h = static_cast<Uint32>(fb_h - strip);
            blit.load_op = SDL_GPU_LOADOP_LOAD;
            blit.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &blit);
        }
        g->geom_cur = dst;
    }

    /* upload vertex data + atlas/text layer when dirty */
    if (!g->solids.empty() || !g->texts.empty() || g->atlas_dirty ||
        g->layer_dirty) {
        void* mapped = SDL_MapGPUTransferBuffer(g->device, g->vb_transfer, false);
        if (mapped) {
            size_t off = 0;
            if (!g->solids.empty()) {
                std::memcpy(static_cast<char*>(mapped) + off,
                            g->solids.data(), g->solids.size() * sizeof(gpu_vert_solid));
                off += g->solids.size() * sizeof(gpu_vert_solid);
            }
            if (!g->texts.empty()) {
                std::memcpy(static_cast<char*>(mapped) + off,
                            g->texts.data(), g->texts.size() * sizeof(gpu_vert_text));
            }
            SDL_UnmapGPUTransferBuffer(g->device, g->vb_transfer);
        }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        if (cp) {
            SDL_GPUTransferBufferLocation bti;
            std::memset(&bti, 0, sizeof(bti));
            bti.transfer_buffer = g->vb_transfer;
            bti.offset = 0;
            if (!g->solids.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->vb_solid;
                br.offset = 0;
                br.size = g->solids.size() * sizeof(gpu_vert_solid);
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
            }
            if (!g->texts.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->vb_text;
                br.offset = 0;
                br.size = g->texts.size() * sizeof(gpu_vert_text);
                bti.offset = g->solids.size() * sizeof(gpu_vert_solid);
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
            }
            if (g->atlas_dirty) {
                SDL_GPUTextureTransferInfo ti;
                std::memset(&ti, 0, sizeof(ti));
                ti.transfer_buffer = g->atlas_transfer;
                ti.offset = 0;
                SDL_GPUTextureRegion reg;
                std::memset(&reg, 0, sizeof(reg));
                reg.texture = g->glyph_atlas;
                reg.w = g->atlas_w;
                reg.h = g->atlas_h;
                reg.d = 1;
                void* am = SDL_MapGPUTransferBuffer(g->device, g->atlas_transfer, false);
                if (am) {
                    std::memcpy(am, g->atlas.data(), g->atlas.size());
                    SDL_UnmapGPUTransferBuffer(g->device, g->atlas_transfer);
                    SDL_UploadToGPUTexture(cp, &ti, &reg, false);
                }
                g->atlas_dirty = 0;
            }
            if (g->layer_dirty) {
                SDL_GPUTextureTransferInfo ti;
                std::memset(&ti, 0, sizeof(ti));
                ti.transfer_buffer = g->layer_transfer;
                ti.offset = 0;
                SDL_GPUTextureRegion reg;
                std::memset(&reg, 0, sizeof(reg));
                reg.texture = g->text_layer;
                reg.w = static_cast<Uint32>(fb_w);
                reg.h = static_cast<Uint32>(fb_h);
                reg.d = 1;
                SDL_UploadToGPUTexture(cp, &ti, &reg, false);
            }
            SDL_EndGPUCopyPass(cp);
        }
    }

    /* render pass into the offscreen target */
    SDL_GPUColorTargetInfo ct;
    std::memset(&ct, 0, sizeof(ct));
    ct.texture = g->geom_cur;
    ct.clear_color = SDL_FColor{
        ((clear_color >> 16) & 0xFF) / 255.0f,
        ((clear_color >> 8) & 0xFF) / 255.0f,
        (clear_color & 0xFF) / 255.0f,
        ((clear_color >> 24) & 0xFF) / 255.0f};
    ct.load_op = (scroll_dy != 0 || load_only) ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    if (!rp) {
        return nullptr;
    }
    SDL_GPUViewport vp;
    std::memset(&vp, 0, sizeof(vp));
    vp.x = 0;
    vp.y = 0;
    vp.w = static_cast<float>(fb_w);
    vp.h = static_cast<float>(fb_h);
    vp.min_depth = 0;
    vp.max_depth = 1;
    SDL_SetGPUViewport(rp, &vp);

    if (!g->solids.empty()) {
        SDL_BindGPUGraphicsPipeline(rp, g->pipe_solid);
        SDL_GPUBufferBinding bb;
        std::memset(&bb, 0, sizeof(bb));
        bb.buffer = g->vb_solid;
        bb.offset = 0;
        SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
        SDL_GPUTextureSamplerBinding tsb;
        std::memset(&tsb, 0, sizeof(tsb));
        tsb.texture = g->white_tex;
        tsb.sampler = g->sampler;
        SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
        SDL_DrawGPUPrimitives(rp, static_cast<int>(g->solids.size()), 1, 0, 0);
    }
    if (!g->texts.empty()) {
        SDL_BindGPUGraphicsPipeline(rp, g->pipe_text);
        SDL_GPUBufferBinding bb;
        std::memset(&bb, 0, sizeof(bb));
        bb.buffer = g->vb_text;
        bb.offset = 0;
        SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
        SDL_GPUTextureSamplerBinding tsb;
        std::memset(&tsb, 0, sizeof(tsb));
        tsb.texture = g->glyph_atlas;
        tsb.sampler = g->sampler;
        SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
        SDL_DrawGPUPrimitives(rp, static_cast<int>(g->texts.size()) / 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);

    /* composite the CPU text layer over the geometry (always: without text
     * it is a straight copy of the geometry into the blit source) */
    if (g->pipe_text_composite) {
        SDL_GPUStorageTextureReadWriteBinding rw;
        std::memset(&rw, 0, sizeof(rw));
        rw.texture = g->target2;
        rw.mip_level = 0;
        rw.layer = 0;
        rw.cycle = false;
        SDL_GPUComputePass* cps =
            SDL_BeginGPUComputePass(cmd, &rw, 1, nullptr, 0);
        if (cps) {
            SDL_BindGPUComputePipeline(cps, g->pipe_text_composite);
            SDL_GPUTextureSamplerBinding tsb[2];
            std::memset(tsb, 0, sizeof(tsb));
            tsb[0].texture = g->geom_cur;
            tsb[1].texture = g->text_layer;
            tsb[0].sampler = g->sampler;
            tsb[1].sampler = g->sampler;
            SDL_BindGPUComputeSamplers(cps, 0, tsb, 2);
            SDL_DispatchGPUCompute(cps, (static_cast<Uint32>(fb_w) + 15) / 16,
                                   (static_cast<Uint32>(fb_h) + 15) / 16, 1);
            SDL_EndGPUComputePass(cps);
        }
        g->layer_dirty = 0;
    }

    g->solids.clear();
    g->texts.clear();
    return cmd;
}
