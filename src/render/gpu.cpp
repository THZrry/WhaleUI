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

/* is the active SDL GPU driver Vulkan? (SPIR-V shader path) */
bool vulkan_driver(SDL_GPUDevice* dev)
{
    const char* drv = SDL_GetGPUDeviceDriver(dev);
    return drv && std::strcmp(drv, "vulkan") == 0;
}

SDL_GPUShader* compile_shader(SDL_GPUDevice* dev, const char* hlsl,
                              const char* entry, bool fragment)
{
#ifdef _WIN32
    const bool vulkan = vulkan_driver(dev);
    const std::wstring profile = fragment ? L"ps_6_0" : L"vs_6_0";
    IDxcBlob* blob = compile_dxil(hlsl, entry, profile.c_str(), vulkan,
                                  nullptr, 0);
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
    /* Non-Windows (macOS Metal / Linux) shader compilation is NOT
     * implemented yet: DXC emits DXIL/SPIR-V only, and SDL's Metal backend
     * needs MSL. UNVERIFIED - the GPU renderer currently requires the
     * D3D12 or Vulkan backend. */
    (void)dev;
    (void)hlsl;
    (void)entry;
    (void)fragment;
    std::fprintf(stderr,
                 "[gpu] shader compile: only D3D12/Vulkan backends are "
                 "implemented (Metal/MSL is not verified)\n");
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

/* mipmapped texture (blur approximation source; SDL_GPUTextureCreateInfo
 * num_levels > 1 enables SDL_GenerateMipmapsForGPUTexture) */
SDL_GPUTexture* make_texture_mips(SDL_GPUDevice* dev, uint32_t w, uint32_t h,
                                  SDL_GPUTextureFormat fmt, uint32_t usage,
                                  uint32_t levels)
{
    SDL_GPUTextureCreateInfo tci;
    std::memset(&tci, 0, sizeof(tci));
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = fmt;
    tci.width = w;
    tci.height = h;
    tci.layer_count_or_depth = 1;
    tci.num_levels = levels;
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
    g->pipe_shadow_cs = nullptr;
    g->pipe_backdrop_cs = nullptr;
    g->pipe_inset_cs = nullptr;
    g->pipe_solid_flat = nullptr;
    g->sampler = nullptr;
    g->sampler_mip = nullptr;
    g->white_tex = nullptr;
    g->glyph_atlas = nullptr;
    g->target = nullptr;
    g->target_b = nullptr;
    g->target2 = nullptr;
    g->blur_tex = nullptr;
    g->vb_solid = nullptr;
    g->vb_text = nullptr;
    g->vb_shapes = nullptr;
    g->shadow_params_buf = nullptr;
    g->backdrop_params_buf = nullptr;
    g->inset_params_buf = nullptr;
    g->vb_transfer = nullptr;
    g->atlas_transfer = nullptr;
    g->layer_transfer = nullptr;
    g->shadow_transfer = nullptr;
    g->pipe_text_composite = nullptr;
    g->text_layer = nullptr;
    g->layer_dirty = 0;
    g->layer_rx = g->layer_ry = g->layer_rw = g->layer_rh = 0;
    g->fb_w = static_cast<float>(w);
    g->fb_h = static_cast<float>(h);
    g->blur_w = w / kBlurDiv;
    g->blur_h = h / kBlurDiv;
    if (g->blur_w < 1) {
        g->blur_w = 1;
    }
    if (g->blur_h < 1) {
        g->blur_h = 1;
    }
    g->atlas_w = 2048;
    g->atlas_h = 2048;
    g->atlas_cx = 0;
    g->atlas_cy = 0;
    g->atlas_row_h = 0;
    g->atlas_dirty = 0;

    /* shaders (HLSL -> DXIL / SPIR-V at runtime) */
    SDL_GPUShader* vs_s = compile_shader(device, kSolidVS, "main", false);
    SDL_GPUShader* ps_s = compile_shader(device, kSolidPS, "main", true);
    SDL_GPUShader* ps_flat = compile_shader(device, kFlatPS, "main", true);
    SDL_GPUShader* vs_t = compile_shader(device, kTextVS, "main", false);
    SDL_GPUShader* ps_t = compile_shader(device, kTextPS, "main", true);
    if (!vs_s || !ps_s || !ps_flat || !vs_t || !ps_t) {
        goto fail;
    }

    /* vertex layout for the solid pipeline (gpu_vert_solid) */
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
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; /* matches target */
    ct.blend_state = blend;
    SDL_GPUGraphicsPipelineCreateInfo info;
    std::memset(&info, 0, sizeof(info));
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.vertex_input_state = vis;
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &ct;

    /* solid pipeline */
    {
        info.vertex_shader = vs_s;
        info.fragment_shader = ps_s;
        g->pipe_solid = SDL_CreateGPUGraphicsPipeline(device, &info);
    }
    if (!g->pipe_solid) {
        goto fail;
    }

    /* flat pipeline: same vertex layout, plain interpolated color (no
     * rounded-rect SDF) - draws the inset-shadow gradient triangles */
    {
        info.vertex_shader = vs_s;
        info.fragment_shader = ps_flat;
        g->pipe_solid_flat = SDL_CreateGPUGraphicsPipeline(device, &info);
    }
    if (!g->pipe_solid_flat) {
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

    /* mip-aware sampler for the blur texture (linear min/mag + linear mip
     * selection; SampleLevel in the shader picks the level explicitly, so
     * the mip filter only matters for the blend across the 3 samples) */
    SDL_GPUSamplerCreateInfo smi;
    std::memset(&smi, 0, sizeof(smi));
    smi.min_filter = SDL_GPU_FILTER_LINEAR;
    smi.mag_filter = SDL_GPU_FILTER_LINEAR;
    smi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    smi.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    smi.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    smi.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g->sampler_mip = SDL_CreateGPUSampler(device, &smi);
    if (!g->sampler_mip) {
        goto fail;
    }

    /* 1x1 white texture */

    g->white_tex = make_texture(device, 1, 1,
                                SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                                SDL_GPU_TEXTUREUSAGE_SAMPLER);

    g->glyph_atlas = make_texture(device, g->atlas_w, g->atlas_h,
                                  SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                                  SDL_GPU_TEXTUREUSAGE_SAMPLER);

    /* geometry target: R8G8B8A8 - B8G8R8A8 rejects the SIMULTANEOUS
     * read+write usage the backdrop compute pass needs (SDL asserts the
     * format/usage pair) */
    g->target = make_texture(device, static_cast<uint32_t>(w),
                             static_cast<uint32_t>(h),
                             SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                             SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                 SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                                 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
    g->target_b = make_texture(device, static_cast<uint32_t>(w),
                               static_cast<uint32_t>(h),
                               SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                               SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                   SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                   SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                                   SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
    g->geom_cur = g->target;

    g->target2 = make_texture(device, static_cast<uint32_t>(w),
                              static_cast<uint32_t>(h),
                              SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                              SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE |
                                  SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                                  SDL_GPU_TEXTUREUSAGE_SAMPLER);

    /* CPU-rasterized text layer. R8G8B8A8 to match the geometry target:
     * SDL's DXIL reflection decodes Load/Sample channel order from the
     * format NAME, so a B8G8R8A8 layer composited into an R8G8B8A8 target
     * swapped R/B (red text came out blue). */
    g->text_layer = make_texture(device, static_cast<uint32_t>(w),
                                 static_cast<uint32_t>(h),
                                 SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                 SDL_GPU_TEXTUREUSAGE_SAMPLER);

    /* blur source: half-res, mipmapped (box-shadow shapes + backdrop
     * geometry copies live here). R8G8B8A8 to match the geometry target
     * (a different format would silently swap channels on the blit). */
    g->blur_tex = make_texture_mips(device, static_cast<uint32_t>(g->blur_w),
                                    static_cast<uint32_t>(g->blur_h),
                                    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                    kBlurLevels);
    if (!g->white_tex || !g->glyph_atlas || !g->target || !g->target_b ||
        !g->target2 || !g->text_layer || !g->blur_tex) {
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
    g->vb_shapes = make_vertex_buffer(device, 128 * 1024);
    /* blur parameter storage (one float4-per-record layout, see the CS) */
    {
        SDL_GPUBufferCreateInfo bci;
        std::memset(&bci, 0, sizeof(bci));
        bci.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
        bci.size = 64 * 4 * 4 * sizeof(float); /* 64 records * 4 float4 */
        g->shadow_params_buf = SDL_CreateGPUBuffer(device, &bci);
        g->backdrop_params_buf = SDL_CreateGPUBuffer(device, &bci);
        g->inset_params_buf = SDL_CreateGPUBuffer(device, &bci);
    }
    SDL_GPUTransferBufferCreateInfo tbi;
    std::memset(&tbi, 0, sizeof(tbi));
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = 512 * 1024;
    g->vb_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    tbi.size = 128 * 1024;
    g->shadow_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    tbi.size = 2048 * 2048;
    g->atlas_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    tbi.size = static_cast<uint32_t>(static_cast<size_t>(w) * h * 4);
    g->layer_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    if (!g->vb_solid || !g->vb_text || !g->vb_shapes ||
        !g->shadow_params_buf || !g->backdrop_params_buf ||
        !g->inset_params_buf ||
        !g->vb_transfer || !g->shadow_transfer || !g->atlas_transfer ||
        !g->layer_transfer) {
        goto fail;
    }

    /* text-composite compute pipeline: text_layer (t0,t1) -> target2 (u0) */
    {
        IDxcBlob* cs = compile_dxil(kTextCompositeCS, "main", L"cs_6_0",
                                    vulkan_driver(device), nullptr, 0);
        if (!cs) {
            goto fail;
        }
        SDL_GPUComputePipelineCreateInfo cpi;
        std::memset(&cpi, 0, sizeof(cpi));
        cpi.code = static_cast<const Uint8*>(cs->GetBufferPointer());
        cpi.code_size = cs->GetBufferSize();
        cpi.entrypoint = "main";
        cpi.format = vulkan_driver(device) ? SDL_GPU_SHADERFORMAT_SPIRV
                                           : SDL_GPU_SHADERFORMAT_DXIL;
        cpi.num_samplers = 2;
        cpi.num_uniform_buffers = 1; /* composite region offset */
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

    /* blur compute pipelines: blur_tex (t0) + params (t1) -> target (u0) */
    {
        IDxcBlob* cs = compile_dxil(kShadowCS, "main", L"cs_6_0",
                                    vulkan_driver(device), nullptr, 0);
        if (!cs) {
            goto fail;
        }
        SDL_GPUComputePipelineCreateInfo cpi;
        std::memset(&cpi, 0, sizeof(cpi));
        cpi.code = static_cast<const Uint8*>(cs->GetBufferPointer());
        cpi.code_size = cs->GetBufferSize();
        cpi.entrypoint = "main";
        cpi.format = vulkan_driver(device) ? SDL_GPU_SHADERFORMAT_SPIRV
                                           : SDL_GPU_SHADERFORMAT_DXIL;
        cpi.num_samplers = 1;
        cpi.num_readonly_storage_buffers = 1;
        cpi.num_readwrite_storage_textures = 1;
        cpi.threadcount_x = 8;
        cpi.threadcount_y = 8;
        cpi.threadcount_z = 1;
        g->pipe_shadow_cs = SDL_CreateGPUComputePipeline(device, &cpi);
        cs->Release();
        if (!g->pipe_shadow_cs) {
            std::fprintf(stderr, "[gpu] shadow CS pipeline failed: %s\n",
                         SDL_GetError());
            goto fail;
        }
        cs = compile_dxil(kBackdropCS, "main", L"cs_6_0",
                          vulkan_driver(device), nullptr, 0);
        if (!cs) {
            goto fail;
        }
        cpi.code = static_cast<const Uint8*>(cs->GetBufferPointer());
        cpi.code_size = cs->GetBufferSize();
        g->pipe_backdrop_cs = SDL_CreateGPUComputePipeline(device, &cpi);
        cs->Release();
        if (!g->pipe_backdrop_cs) {
            std::fprintf(stderr, "[gpu] backdrop CS pipeline failed: %s\n",
                         SDL_GetError());
            goto fail;
        }
        cs = compile_dxil(kInsetCS, "main", L"cs_6_0",
                          vulkan_driver(device), nullptr, 0);
        if (!cs) {
            goto fail;
        }
        cpi.code = static_cast<const Uint8*>(cs->GetBufferPointer());
        cpi.code_size = cs->GetBufferSize();
        g->pipe_inset_cs = SDL_CreateGPUComputePipeline(device, &cpi);
        cs->Release();
        if (!g->pipe_inset_cs) {
            std::fprintf(stderr, "[gpu] inset CS pipeline failed: %s\n",
                         SDL_GetError());
            goto fail;
        }
    }

    SDL_ReleaseGPUShader(device, vs_s);
    SDL_ReleaseGPUShader(device, ps_s);
    SDL_ReleaseGPUShader(device, ps_flat);
    return g;

fail:
    whaleui_gpu_destroy(g);
    return nullptr;
}

int whaleui_gpu_resize(whaleui_gpu_t* g, int w, int h)
{
    if (!g || w <= 0 || h <= 0) {
        return -1;
    }
    SDL_GPUDevice* dev = g->device;
    if (g->target) {
        SDL_ReleaseGPUTexture(dev, g->target);
    }
    if (g->target_b) {
        SDL_ReleaseGPUTexture(dev, g->target_b);
    }
    if (g->target2) {
        SDL_ReleaseGPUTexture(dev, g->target2);
    }
    if (g->text_layer) {
        SDL_ReleaseGPUTexture(dev, g->text_layer);
    }
    if (g->blur_tex) {
        SDL_ReleaseGPUTexture(dev, g->blur_tex);
    }
    if (g->layer_transfer) {
        SDL_ReleaseGPUTransferBuffer(dev, g->layer_transfer);
    }
    g->blur_w = w / kBlurDiv;
    g->blur_h = h / kBlurDiv;
    if (g->blur_w < 1) {
        g->blur_w = 1;
    }
    if (g->blur_h < 1) {
        g->blur_h = 1;
    }
    const Uint32 wu = static_cast<Uint32>(w);
    const Uint32 hu = static_cast<Uint32>(h);
    /* same formats/usages as whaleui_gpu_create */
    g->target = make_texture(
        dev, wu, hu, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER |
            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
    g->target_b = make_texture(
        dev, wu, hu, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER |
            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
    g->geom_cur = g->target;
    g->target2 = make_texture(
        dev, wu, hu, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE |
            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
            SDL_GPU_TEXTUREUSAGE_SAMPLER);
    g->text_layer = make_texture(dev, wu, hu,
                                 SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                 SDL_GPU_TEXTUREUSAGE_SAMPLER);
    g->blur_tex = make_texture_mips(dev, static_cast<uint32_t>(g->blur_w),
                                    static_cast<uint32_t>(g->blur_h),
                                    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                    kBlurLevels);
    SDL_GPUTransferBufferCreateInfo tbi;
    std::memset(&tbi, 0, sizeof(tbi));
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = static_cast<uint32_t>(static_cast<size_t>(w) * h * 4);
    g->layer_transfer = SDL_CreateGPUTransferBuffer(dev, &tbi);
    g->fb_w = static_cast<float>(w);
    g->fb_h = static_cast<float>(h);
    g->layer_rx = g->layer_ry = g->layer_rw = g->layer_rh = 0;
    g->layer_dirty = 0;
    return (g->target && g->target_b && g->target2 && g->text_layer &&
            g->blur_tex && g->layer_transfer)
               ? 0
               : -1;
}

void whaleui_gpu_destroy(whaleui_gpu_t* g)
{
    if (!g) {
        return;
    }
    if (g->device) {
        if (g->pipe_solid) SDL_ReleaseGPUGraphicsPipeline(g->device, g->pipe_solid);
        if (g->pipe_solid_flat) SDL_ReleaseGPUGraphicsPipeline(g->device, g->pipe_solid_flat);
        if (g->pipe_text) SDL_ReleaseGPUGraphicsPipeline(g->device, g->pipe_text);
        if (g->pipe_text_composite) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_text_composite);
        if (g->pipe_shadow_cs) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_shadow_cs);
        if (g->pipe_backdrop_cs) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_backdrop_cs);
        if (g->pipe_inset_cs) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_inset_cs);
        if (g->sampler) SDL_ReleaseGPUSampler(g->device, g->sampler);
        if (g->sampler_mip) SDL_ReleaseGPUSampler(g->device, g->sampler_mip);
        if (g->white_tex) SDL_ReleaseGPUTexture(g->device, g->white_tex);
        if (g->glyph_atlas) SDL_ReleaseGPUTexture(g->device, g->glyph_atlas);
        if (g->text_layer) SDL_ReleaseGPUTexture(g->device, g->text_layer);
        if (g->blur_tex) SDL_ReleaseGPUTexture(g->device, g->blur_tex);
        if (g->target) SDL_ReleaseGPUTexture(g->device, g->target);
        if (g->target_b) SDL_ReleaseGPUTexture(g->device, g->target_b);
        if (g->target2) SDL_ReleaseGPUTexture(g->device, g->target2);
        if (g->vb_solid) SDL_ReleaseGPUBuffer(g->device, g->vb_solid);
        if (g->vb_text) SDL_ReleaseGPUBuffer(g->device, g->vb_text);
        if (g->vb_shapes) SDL_ReleaseGPUBuffer(g->device, g->vb_shapes);
        if (g->shadow_params_buf) SDL_ReleaseGPUBuffer(g->device, g->shadow_params_buf);
        if (g->backdrop_params_buf) SDL_ReleaseGPUBuffer(g->device, g->backdrop_params_buf);
        if (g->inset_params_buf) SDL_ReleaseGPUBuffer(g->device, g->inset_params_buf);
        if (g->vb_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->vb_transfer);
        if (g->atlas_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->atlas_transfer);
        if (g->layer_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->layer_transfer);
        if (g->shadow_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->shadow_transfer);
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
        /* clip shrinks the quad; scale the corner radius with it, or the
         * SDF reads a radius larger than the visible box and the whole
         * quad falls outside the arc (alpha 0) */
        float sx = (x1 - x0) / w, sy = (y1 - y0) / h;
        float s = sx < sy ? sx : sy;
        radius *= s;
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

void whaleui_gpu_gradient_rect(whaleui_gpu_t* g, float x, float y, float w,
                               float h, unsigned int c0, unsigned int c1,
                               unsigned int c2, unsigned int c3,
                               const int* clip)
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
        return;
    }
    /* corner colors: v[0]=TL v[1]=TR v[2]=BL v[3]=TR v[4]=BL v[5]=BR */
    unsigned int cols[6] = {c0, c1, c2, c1, c2, c3};
    gpu_vert_solid v[6];
    for (int i = 0; i < 6; ++i) {
        v[i].x = x;
        v[i].y = y;
        v[i].r = ((cols[i] >> 16) & 0xFF) / 255.0f;
        v[i].g = ((cols[i] >> 8) & 0xFF) / 255.0f;
        v[i].b = (cols[i] & 0xFF) / 255.0f;
        v[i].a = ((cols[i] >> 24) & 0xFF) / 255.0f;
        v[i].size_x = w;
        v[i].size_y = h;
        v[i].radius = 0;
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

/* push a solid quad into an arbitrary vertex list (blur shapes / backdrop
 * fills); fbw/fbh are the pass viewport size (NDC conversion) */
static void push_solid_into(std::vector<gpu_vert_solid>& out, float x, float y,
                            float w, float h, float radius, unsigned int color,
                            float fbw, float fbh)
{
    if (out.size() + 6 > 65536) {
        return;
    }
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float gg = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;
    gpu_vert_solid v[6];
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
        v[i].fb_w = fbw;
        v[i].fb_h = fbh;
    }
    v[0].x = x;         v[0].y = y;         v[0].u = 0; v[0].v = 0;
    v[1].x = x + w;     v[1].y = y;         v[1].u = 1; v[1].v = 0;
    v[2].x = x;         v[2].y = y + h;     v[2].u = 0; v[2].v = 1;
    v[3].x = x + w;     v[3].y = y;         v[3].u = 1; v[3].v = 0;
    v[4].x = x;         v[4].y = y + h;     v[4].u = 0; v[4].v = 1;
    v[5].x = x + w;     v[5].y = y + h;     v[5].u = 1; v[5].v = 1;
    for (int i = 0; i < 6; ++i) {
        out.push_back(v[i]);
    }
}

void whaleui_gpu_shadow(whaleui_gpu_t* g, float x, float y, float w, float h,
                        float radius, float blur, unsigned int color)
{
    if (!g || blur <= 0 || w <= 0 || h <= 0) {
        return;
    }
    /* the shape goes into the half-res blur_tex (white; the alpha channel
     * after blurring is the shadow mask) */
    push_solid_into(g->shapes, x / static_cast<float>(kBlurDiv),
                    y / static_cast<float>(kBlurDiv),
                    w / static_cast<float>(kBlurDiv),
                    h / static_cast<float>(kBlurDiv),
                    radius / static_cast<float>(kBlurDiv), 0xFFFFFFFF,
                    static_cast<float>(g->blur_w),
                    static_cast<float>(g->blur_h));
    /* compute-pass record: region grown by the blur spread (the shader
     * tests membership against it) */
    if (g->shadows.size() >= 64) {
        return;
    }
    gpu_blur_param p;
    p.x = x - blur;
    p.y = y - blur;
    p.w = w + 2.0f * blur;
    p.h = h + 2.0f * blur;
    p.blur = blur;
    p.r = ((color >> 16) & 0xFF) / 255.0f;
    p.g = ((color >> 8) & 0xFF) / 255.0f;
    p.b = (color & 0xFF) / 255.0f;
    p.a = ((color >> 24) & 0xFF) / 255.0f;
    p.br = p.bg = p.bb = 0;
    g->shadows.push_back(p);
}

void whaleui_gpu_inset(whaleui_gpu_t* g, float x, float y, float w, float h,
                       float radius, float blur, unsigned int color)
{
    (void)radius; /* corner approximation comes from the diagonal split */
    if (!g || blur <= 0 || w <= 0 || h <= 0) {
        return;
    }
    /* split the box along its two diagonals into four triangles (top /
     * bottom / left / right). Each triangle has its two outer edge
     * vertices at full alpha and the shared center vertex at 0, so the
     * rasterizer interpolates a linear ramp from every edge inward; the
     * mipmap blur below only softens it. Corners get both adjacent ramps
     * -> naturally darker, like a real inset shadow. */
    const float bx = x / static_cast<float>(kBlurDiv);
    const float by = y / static_cast<float>(kBlurDiv);
    const float bw = w / static_cast<float>(kBlurDiv);
    const float bh = h / static_cast<float>(kBlurDiv);
    const float cx = bx + bw * 0.5f;
    const float cy = by + bh * 0.5f;
    const float cr = ((color >> 16) & 0xFF) / 255.0f;
    const float cg = ((color >> 8) & 0xFF) / 255.0f;
    const float cb = (color & 0xFF) / 255.0f;
    const float fbw = static_cast<float>(g->blur_w);
    const float fbh = static_cast<float>(g->blur_h);
    /* corner indices: 0=TL 1=TR 2=BR 3=BL */
    const float corners[4][2] = {
        {bx, by}, {bx + bw, by}, {bx + bw, by + bh}, {bx, by + bh}};
    /* triangle corner pairs: top(TL,TR) bottom(BL,BR) left(TL,BL) right(TR,BR) */
    const int tris[4][2] = {{0, 1}, {3, 2}, {0, 3}, {1, 2}};
    for (int t = 0; t < 4 && g->inset_shapes.size() + 3 <= 65536; ++t) {
        gpu_vert_solid v[3];
        for (int i = 0; i < 3; ++i) {
            const bool edge = i < 2;
            v[i].x = edge ? corners[tris[t][i]][0] : cx;
            v[i].y = edge ? corners[tris[t][i]][1] : cy;
            v[i].u = 0;
            v[i].v = 0;
            v[i].r = cr;
            v[i].g = cg;
            v[i].b = cb;
            v[i].a = edge ? 1.0f : 0.0f;
            v[i].size_x = bw;
            v[i].size_y = bh;
            v[i].radius = 0;
            v[i].fb_w = fbw;
            v[i].fb_h = fbh;
        }
        g->inset_shapes.push_back(v[0]);
        g->inset_shapes.push_back(v[1]);
        g->inset_shapes.push_back(v[2]);
    }
    if (g->insets.size() >= 64) {
        return;
    }
    gpu_blur_param p;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.blur = blur;
    p.r = cr;
    p.g = cg;
    p.b = cb;
    p.a = ((color >> 24) & 0xFF) / 255.0f;
    p.br = p.bg = p.bb = 0;
    g->insets.push_back(p);
}

void whaleui_gpu_backdrop(whaleui_gpu_t* g, float x, float y, float w,
                          float h, float radius, float blur,
                          unsigned int body_color)
{
    (void)radius;
    if (!g || w <= 0 || h <= 0) {
        return;
    }
    if (g->backdrops.size() >= 64) {
        return;
    }
    gpu_blur_param p;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.blur = blur > 0 ? blur : 8.0f;
    p.a = 0;
    p.br = ((body_color >> 16) & 0xFF) / 255.0f;
    p.bg = ((body_color >> 8) & 0xFF) / 255.0f;
    p.bb = (body_color & 0xFF) / 255.0f;
    /* body alpha rides in p.a (the shader reads d.x for it) */
    p.a = ((body_color >> 24) & 0xFF) / 255.0f;
    g->backdrops.push_back(p);
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
                            int w, int h, int rx, int ry, int rw, int rh)
{
    if (!g || !pixels || w <= 0 || h <= 0 || !g->layer_transfer) {
        return;
    }
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + rw > w) rw = w - rx;
    if (ry + rh > h) rh = h - ry;
    if (rw <= 0 || rh <= 0) {
        return;
    }
    /* the transfer buffer holds only the region rows (each padded to the
     * full layer width, see pixels_per_row in flush) */
    size_t bytes = static_cast<size_t>(rh) * w * 4;
    void* mapped = SDL_MapGPUTransferBuffer(g->device, g->layer_transfer, false);
    if (!mapped) {
        return;
    }
    /* CPU pixels are 0xAARRGGBB; the RGBA8 layer wants R,G,B,A */
    const unsigned char* src = reinterpret_cast<const unsigned char*>(pixels);
    unsigned char* dst = static_cast<unsigned char*>(mapped);
    for (int yy = ry; yy < ry + rh; ++yy) {
        const unsigned char* srow = src + (static_cast<size_t>(yy) * w + rx) * 4;
        unsigned char* drow = dst + (static_cast<size_t>(yy - ry) * w) * 4;
        for (int xx = 0; xx < rw; ++xx) {
            drow[xx * 4 + 0] = srow[xx * 4 + 2];
            drow[xx * 4 + 1] = srow[xx * 4 + 1];
            drow[xx * 4 + 2] = srow[xx * 4 + 0];
            drow[xx * 4 + 3] = srow[xx * 4 + 3];
        }
    }
    SDL_UnmapGPUTransferBuffer(g->device, g->layer_transfer);
    g->layer_rx = rx;
    g->layer_ry = ry;
    g->layer_rw = rw;
    g->layer_rh = rh;
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
    bool need_vb = !g->solids.empty() || !g->texts.empty() ||
                   g->atlas_dirty || g->layer_dirty;
    bool need_svb = !g->shapes.empty() || !g->inset_shapes.empty() ||
                    !g->shadows.empty() || !g->backdrops.empty() ||
                    !g->insets.empty();
    if (need_vb) {
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
                ti.pixels_per_row = static_cast<Uint32>(fb_w);
                SDL_GPUTextureRegion reg;
                std::memset(&reg, 0, sizeof(reg));
                reg.texture = g->text_layer;
                reg.x = static_cast<Uint32>(g->layer_rx);
                reg.y = static_cast<Uint32>(g->layer_ry);
                reg.w = static_cast<Uint32>(g->layer_rw);
                reg.h = static_cast<Uint32>(g->layer_rh);
                reg.d = 1;
                SDL_UploadToGPUTexture(cp, &ti, &reg, false);
            }
            SDL_EndGPUCopyPass(cp);
        }
    }
    if (need_svb) {
        /* shapes -> vb_shapes; shadow/backdrop records -> the compute
         * param buffers. Each param buffer starts with a header
         * {count, fb_w, fb_h, 0} followed by the packed records. */
        void* m2 = SDL_MapGPUTransferBuffer(g->device, g->shadow_transfer,
                                            false);
        if (m2) {
            size_t off = 0;
            if (!g->shapes.empty()) {
                std::memcpy(static_cast<char*>(m2) + off, g->shapes.data(),
                            g->shapes.size() * sizeof(gpu_vert_solid));
                off += g->shapes.size() * sizeof(gpu_vert_solid);
            }
            if (!g->inset_shapes.empty()) {
                std::memcpy(static_cast<char*>(m2) + off,
                            g->inset_shapes.data(),
                            g->inset_shapes.size() * sizeof(gpu_vert_solid));
                off += g->inset_shapes.size() * sizeof(gpu_vert_solid);
            }
            if (!g->backdrops.empty()) {
                float* hdr = static_cast<float*>(m2) + off / sizeof(float);
                hdr[0] = static_cast<float>(g->backdrops.size());
                hdr[1] = g->fb_w;
                hdr[2] = g->fb_h;
                hdr[3] = 0;
                std::memcpy(static_cast<char*>(m2) + off + 16,
                            g->backdrops.data(),
                            g->backdrops.size() * sizeof(gpu_blur_param));
                off += 16 + g->backdrops.size() * sizeof(gpu_blur_param);
            }
            if (!g->shadows.empty()) {
                float* hdr = static_cast<float*>(m2) + off / sizeof(float);
                hdr[0] = static_cast<float>(g->shadows.size());
                hdr[1] = g->fb_w;
                hdr[2] = g->fb_h;
                hdr[3] = 0;
                std::memcpy(static_cast<char*>(m2) + off + 16,
                            g->shadows.data(),
                            g->shadows.size() * sizeof(gpu_blur_param));
                off += 16 + g->shadows.size() * sizeof(gpu_blur_param);
            }
            if (!g->insets.empty()) {
                float* hdr = static_cast<float*>(m2) + off / sizeof(float);
                hdr[0] = static_cast<float>(g->insets.size());
                hdr[1] = g->fb_w;
                hdr[2] = g->fb_h;
                hdr[3] = 0;
                std::memcpy(static_cast<char*>(m2) + off + 16,
                            g->insets.data(),
                            g->insets.size() * sizeof(gpu_blur_param));
            }
            SDL_UnmapGPUTransferBuffer(g->device, g->shadow_transfer);
        }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        if (cp) {
            SDL_GPUTransferBufferLocation bti;
            std::memset(&bti, 0, sizeof(bti));
            bti.transfer_buffer = g->shadow_transfer;
            bti.offset = 0;
            size_t off = 0;
            if (!g->shapes.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->vb_shapes;
                br.offset = 0;
                br.size = g->shapes.size() * sizeof(gpu_vert_solid);
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
                off += g->shapes.size() * sizeof(gpu_vert_solid);
            }
            if (!g->inset_shapes.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->vb_shapes;
                br.offset = 0;
                br.size = g->inset_shapes.size() * sizeof(gpu_vert_solid);
                bti.offset = off;
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
                off += g->inset_shapes.size() * sizeof(gpu_vert_solid);
            }
            if (!g->backdrops.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->backdrop_params_buf;
                br.offset = 0;
                br.size = 16 + g->backdrops.size() * sizeof(gpu_blur_param);
                bti.offset = off;
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
                off += 16 + g->backdrops.size() * sizeof(gpu_blur_param);
            }
            if (!g->shadows.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->shadow_params_buf;
                br.offset = 0;
                br.size = 16 + g->shadows.size() * sizeof(gpu_blur_param);
                bti.offset = off;
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
                off += 16 + g->shadows.size() * sizeof(gpu_blur_param);
            }
            if (!g->insets.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->inset_params_buf;
                br.offset = 0;
                br.size = 16 + g->insets.size() * sizeof(gpu_blur_param);
                bti.offset = off;
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
            }
            SDL_EndGPUCopyPass(cp);
        }
    }

    /* box-shadow / backdrop blur run only on full repaints - a scrolled
     * frame reuses the previous target and must not overwrite it
     * (ponytail: shadows briefly drop during pure scrolls; the next full
     * repaint restores them). */
    bool do_shadow = !g->shadows.empty() && scroll_dy == 0 && !load_only;

    /* pass A: paint the shadow shapes into the low-res blur texture, then
     * build the mip chain (box-filtered downsamples) */
    if (do_shadow) {
        SDL_GPUColorTargetInfo bct;
        std::memset(&bct, 0, sizeof(bct));
        bct.texture = g->blur_tex;
        bct.clear_color = SDL_FColor{0, 0, 0, 0};
        bct.load_op = SDL_GPU_LOADOP_CLEAR;
        bct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* brp = SDL_BeginGPURenderPass(cmd, &bct, 1, nullptr);
        if (brp) {
            SDL_GPUViewport bvp;
            std::memset(&bvp, 0, sizeof(bvp));
            bvp.w = static_cast<float>(g->blur_w);
            bvp.h = static_cast<float>(g->blur_h);
            bvp.min_depth = 0;
            bvp.max_depth = 1;
            SDL_SetGPUViewport(brp, &bvp);
            SDL_BindGPUGraphicsPipeline(brp, g->pipe_solid);
            SDL_GPUBufferBinding bb;
            std::memset(&bb, 0, sizeof(bb));
            bb.buffer = g->vb_shapes;
            bb.offset = 0;
            SDL_BindGPUVertexBuffers(brp, 0, &bb, 1);
            SDL_GPUTextureSamplerBinding tsb;
            std::memset(&tsb, 0, sizeof(tsb));
            tsb.texture = g->white_tex;
            tsb.sampler = g->sampler;
            SDL_BindGPUFragmentSamplers(brp, 0, &tsb, 1);
            SDL_DrawGPUPrimitives(brp, static_cast<int>(g->shapes.size()), 1,
                                  0, 0);
            SDL_EndGPURenderPass(brp);
        }
        /* SDL_GenerateMipmapsForGPUTexture: must not run inside a pass */
        SDL_GenerateMipmapsForGPUTexture(cmd, g->blur_tex);
    }

    /* compute pass: blur the shapes into the target as the shadow layer */
    if (do_shadow) {
        SDL_GPUStorageTextureReadWriteBinding rw;
        std::memset(&rw, 0, sizeof(rw));
        rw.texture = g->geom_cur;
        rw.mip_level = 0;
        rw.layer = 0;
        rw.cycle = false;
        SDL_GPUComputePass* cps = SDL_BeginGPUComputePass(cmd, &rw, 1, nullptr, 0);
        if (cps) {
            SDL_BindGPUComputePipeline(cps, g->pipe_shadow_cs);
            SDL_GPUTextureSamplerBinding tsb;
            std::memset(&tsb, 0, sizeof(tsb));
            tsb.texture = g->blur_tex;
            tsb.sampler = g->sampler_mip;
            SDL_BindGPUComputeSamplers(cps, 0, &tsb, 1);
            SDL_GPUBuffer* sbb = g->shadow_params_buf;
            SDL_BindGPUComputeStorageBuffers(cps, 0, &sbb, 1);
            SDL_DispatchGPUCompute(cps,
                                   (static_cast<Uint32>(fb_w) + 7) / 8,
                                   (static_cast<Uint32>(fb_h) + 7) / 8, 1);
            SDL_EndGPUComputePass(cps);
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
    ct.load_op = (scroll_dy != 0 || load_only || do_shadow)
                     ? SDL_GPU_LOADOP_LOAD
                     : SDL_GPU_LOADOP_CLEAR;
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

    /* pass C: backdrop-filter. Copy the painted geometry into the blur
     * texture, mip it, then a compute pass replaces each region with the
     * blurred copy and blends the element's own background on top. Runs on
     * partial (animation/hover) frames too, so a fixed translucent header
     * stays blurred while content animates under it. */
    if (!g->backdrops.empty() && scroll_dy == 0) {
        SDL_GPUBlitInfo blit;
        std::memset(&blit, 0, sizeof(blit));
        blit.source.texture = g->geom_cur;
        blit.source.w = static_cast<Uint32>(fb_w);
        blit.source.h = static_cast<Uint32>(fb_h);
        blit.destination.texture = g->blur_tex;
        blit.destination.w = static_cast<Uint32>(g->blur_w);
        blit.destination.h = static_cast<Uint32>(g->blur_h);
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cmd, &blit);
        SDL_GenerateMipmapsForGPUTexture(cmd, g->blur_tex);

        /* read+write the target in one pass (SIMULTANEOUS_READ_WRITE) */
        SDL_GPUStorageTextureReadWriteBinding rw;
        std::memset(&rw, 0, sizeof(rw));
        rw.texture = g->geom_cur;
        rw.mip_level = 0;
        rw.layer = 0;
        rw.cycle = false;
        SDL_GPUComputePass* cps =
            SDL_BeginGPUComputePass(cmd, &rw, 1, nullptr, 0);
        if (cps) {
            SDL_BindGPUComputePipeline(cps, g->pipe_backdrop_cs);
            SDL_GPUTextureSamplerBinding tsb;
            std::memset(&tsb, 0, sizeof(tsb));
            tsb.texture = g->blur_tex;
            tsb.sampler = g->sampler_mip;
            SDL_BindGPUComputeSamplers(cps, 0, &tsb, 1);
            SDL_GPUBuffer* sbb = g->backdrop_params_buf;
            SDL_BindGPUComputeStorageBuffers(cps, 0, &sbb, 1);
            SDL_DispatchGPUCompute(cps,
                                   (static_cast<Uint32>(fb_w) + 7) / 8,
                                   (static_cast<Uint32>(fb_h) + 7) / 8, 1);
            SDL_EndGPUComputePass(cps);
        }
    }

    /* pass A2 + D: inset box-shadows. The four gradient triangles are
     * rasterized into blur_tex (freshly cleared - the backdrop copy above
     * already consumed it), mipmapped, and a compute pass blends the
     * blurred ramp over the painted geometry (inset shadows sit on top of
     * the element background). */
    if (!g->insets.empty() && scroll_dy == 0 && !load_only) {
        SDL_GPUColorTargetInfo ict;
        std::memset(&ict, 0, sizeof(ict));
        ict.texture = g->blur_tex;
        ict.clear_color = SDL_FColor{0, 0, 0, 0};
        ict.load_op = SDL_GPU_LOADOP_CLEAR;
        ict.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* irp = SDL_BeginGPURenderPass(cmd, &ict, 1, nullptr);
        if (irp) {
            SDL_GPUViewport ivp;
            std::memset(&ivp, 0, sizeof(ivp));
            ivp.w = static_cast<float>(g->blur_w);
            ivp.h = static_cast<float>(g->blur_h);
            ivp.min_depth = 0;
            ivp.max_depth = 1;
            SDL_SetGPUViewport(irp, &ivp);
            SDL_BindGPUGraphicsPipeline(irp, g->pipe_solid_flat);
            SDL_GPUBufferBinding ibb;
            std::memset(&ibb, 0, sizeof(ibb));
            ibb.buffer = g->vb_shapes;
            ibb.offset = 0;
            SDL_BindGPUVertexBuffers(irp, 0, &ibb, 1);
            SDL_GPUTextureSamplerBinding itsb;
            std::memset(&itsb, 0, sizeof(itsb));
            itsb.texture = g->white_tex;
            itsb.sampler = g->sampler;
            SDL_BindGPUFragmentSamplers(irp, 0, &itsb, 1);
            SDL_DrawGPUPrimitives(irp, static_cast<int>(g->inset_shapes.size()),
                                  1, 0, 0);
            SDL_EndGPURenderPass(irp);
        }
        SDL_GenerateMipmapsForGPUTexture(cmd, g->blur_tex);

        SDL_GPUStorageTextureReadWriteBinding rw;
        std::memset(&rw, 0, sizeof(rw));
        rw.texture = g->geom_cur;
        rw.mip_level = 0;
        rw.layer = 0;
        rw.cycle = false;
        SDL_GPUComputePass* cps = SDL_BeginGPUComputePass(cmd, &rw, 1, nullptr, 0);
        if (cps) {
            SDL_BindGPUComputePipeline(cps, g->pipe_inset_cs);
            SDL_GPUTextureSamplerBinding tsb;
            std::memset(&tsb, 0, sizeof(tsb));
            tsb.texture = g->blur_tex;
            tsb.sampler = g->sampler_mip;
            SDL_BindGPUComputeSamplers(cps, 0, &tsb, 1);
            SDL_GPUBuffer* sbb = g->inset_params_buf;
            SDL_BindGPUComputeStorageBuffers(cps, 0, &sbb, 1);
            SDL_DispatchGPUCompute(cps,
                                   (static_cast<Uint32>(fb_w) + 7) / 8,
                                   (static_cast<Uint32>(fb_h) + 7) / 8, 1);
            SDL_EndGPUComputePass(cps);
        }
    }

    /* composite the CPU text layer over the geometry (always: without text
     * it is a straight copy of the geometry into the blit source).
     * Dirty-rect frames only re-upload the painted strip (layer_rx/ry/rw/
     * rh), so the composite dispatches just that region - the full-screen
     * composite was the dominant GPU cost at 2k (~14k workgroups per
     * frame even when only a small box animated). */
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
            /* region offset uniform (uint2); the shader adds it to the
             * dispatch thread id to reach global pixel coordinates */
            int rx = g->layer_rx < 0 ? 0 : g->layer_rx;
            int ry = g->layer_ry < 0 ? 0 : g->layer_ry;
            int rwpx = g->layer_rw > 0 ? g->layer_rw : fb_w;
            int rhpx = g->layer_rh > 0 ? g->layer_rh : fb_h;
            if (rwpx > fb_w - rx) {
                rwpx = fb_w - rx;
            }
            if (rhpx > fb_h - ry) {
                rhpx = fb_h - ry;
            }
            if (rwpx < 1) {
                rwpx = 1;
            }
            if (rhpx < 1) {
                rhpx = 1;
            }
            Uint32 off[4] = {static_cast<Uint32>(rx), static_cast<Uint32>(ry),
                             0, 0};
            SDL_PushGPUComputeUniformData(cmd, 0, off, sizeof(off));
            SDL_DispatchGPUCompute(
                cps, (static_cast<Uint32>(rwpx) + 15) / 16,
                (static_cast<Uint32>(rhpx) + 15) / 16, 1);
            SDL_EndGPUComputePass(cps);
        }
        g->layer_dirty = 0;
    }

    g->solids.clear();
    g->texts.clear();
    g->shadows.clear();
    g->backdrops.clear();
    g->insets.clear();
    g->shapes.clear();
    g->inset_shapes.clear();
    return cmd;
}



