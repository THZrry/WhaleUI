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
#include <cmath>
#include <string>
#include <cwchar>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

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

/* --- compiled-shader cache ---------------------------------------------
 *
 * DXC costs ~500-700ms for the ten shaders the renderer needs, and it ran
 * once per WINDOW (whaleui_gpu_create compiles all of them), so even an
 * empty page took ~600ms to show and a second window paid it again. Two
 * layers fix that:
 *   - an in-process memo: the second window reuses the first window's
 *     bytecode (same device driver, same sources);
 *   - an on-disk cache keyed on a hash of EVERYTHING that determines the
 *     bytecode, so a source edit can never hit a stale entry.
 *
 * The key covers the HLSL text, entry point, target profile, the SPIR-V
 * flag and its extra args, the library version, a cache-format version,
 * and the DXC DLL's size + mtime (upgrading dxcompiler.dll invalidates).
 * A change to any of those produces a different filename, so the old entry
 * is simply never read again. */

const uint32_t kShaderCacheVersion = 1; /* bump on any layout/key change */

uint64_t fnv1a(const void* data, size_t len, uint64_t h = 1469598103934665603ULL)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t fnv1a_str(const char* s, uint64_t h = 1469598103934665603ULL)
{
    return s ? fnv1a(s, std::strlen(s), h) : h;
}

/* "<hex key>.bin" under the cache directory, or empty when caching is off */
std::string shader_cache_path(const std::string& key)
{
    const char* off = SDL_getenv("WHALEUI_SHADER_CACHE");
    if (off && off[0] == '0' && off[1] == '\0') {
        return std::string(); /* explicitly disabled */
    }
    char* dir = nullptr;
    if (off && off[0]) {
        dir = SDL_strdup(off); /* caller-supplied directory */
    } else {
        dir = SDL_GetPrefPath("WhaleUI", "shader-cache");
    }
    if (!dir) {
        return std::string();
    }
    std::string path(dir);
    SDL_free(dir);
    if (!path.empty() && path[path.size() - 1] != '/' &&
        path[path.size() - 1] != '\\') {
        path += '/';
    }
    path += key;
    path += ".bin";
    return path;
}

std::mutex g_shader_memo_mx;
std::map<std::string, std::vector<unsigned char> >& shader_memo()
{
    static std::map<std::string, std::vector<unsigned char> > m;
    return m;
}

bool read_file(const std::string& path, std::vector<unsigned char>& out)
{
    if (path.empty()) {
        return false;
    }
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) {
        return false;
    }
    Sint64 sz = SDL_GetIOSize(io);
    if (sz <= 0 || sz > (64 << 20)) {
        SDL_CloseIO(io);
        return false;
    }
    out.resize(static_cast<size_t>(sz));
    size_t got = SDL_ReadIO(io, out.data(), out.size());
    SDL_CloseIO(io);
    if (got != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

/* write via a temp file + rename so a concurrent reader never sees a
 * half-written entry (two windows can compile the same key at once) */
void write_file_atomic(const std::string& path,
                       const std::vector<unsigned char>& data)
{
    if (path.empty() || data.empty()) {
        return;
    }
    std::string tmp = path + ".tmp";
    SDL_IOStream* io = SDL_IOFromFile(tmp.c_str(), "wb");
    if (!io) {
        return;
    }
    size_t put = SDL_WriteIO(io, data.data(), data.size());
    SDL_CloseIO(io);
    if (put != data.size()) {
        SDL_RemovePath(tmp.c_str());
        return;
    }
    /* rename over an existing entry: remove first (Windows rename fails
     * otherwise); losing a concurrent writer's entry is harmless - the
     * loser just recompiles next run */
    SDL_RemovePath(path.c_str());
    if (SDL_RenamePath(tmp.c_str(), path.c_str()) != 0) {
        SDL_RemovePath(tmp.c_str());
    }
}

/* Everything that can change the emitted bytecode, hashed into one key. */
std::string shader_key(const char* hlsl, const char* entry,
                       const wchar_t* profile, bool spirv,
                       const wchar_t* const* extra, int nextra)
{
    uint64_t h = fnv1a_str(hlsl);
    h = fnv1a_str(entry, h);
    if (profile) {
        h = fnv1a(profile, std::wcslen(profile) * sizeof(wchar_t), h);
    }
    h = fnv1a(&spirv, sizeof(spirv), h);
    for (int i = 0; i < nextra && extra; ++i) {
        if (extra[i]) {
            h = fnv1a(extra[i], std::wcslen(extra[i]) * sizeof(wchar_t), h);
        }
    }
    h = fnv1a_str(WHALEUI_VERSION, h);
    h = fnv1a(&kShaderCacheVersion, sizeof(kShaderCacheVersion), h);
#ifdef _WIN32
    /* a dxcompiler.dll upgrade must invalidate every entry */
    if (g_dxc_dll) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(g_dxc_dll, path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
                h = fnv1a(&fad.nFileSizeLow, sizeof(fad.nFileSizeLow), h);
                h = fnv1a(&fad.ftLastWriteTime, sizeof(fad.ftLastWriteTime), h);
            }
        }
    }
#endif
    char hex[24];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    return std::string(hex);
}

/* compile HLSL to DXIL/SPIR-V bytecode; returns false on failure.
 * profile: L"vs_6_0" / L"ps_6_0" / L"cs_6_0". When spirv is set, emits
 * SPIR-V (-spirv) with the given -fvk-bind extras.
 *
 * Returns BYTES rather than an IDxcBlob so the cache can hand back a decoded
 * entry without recompiling (see the cache block above). */
bool compile_dxil(const char* hlsl, const char* entry,
                  const wchar_t* profile, bool spirv,
                  const wchar_t* const* extra, int nextra,
                  std::vector<unsigned char>& out)
{
    out.clear();
#ifdef _WIN32
    if (!dxc_init()) {
        return false;
    }
    /* memo/disk cache: see the block comment above. Keyed on every input
     * that can change the bytecode, so a source edit is a cache MISS, never
     * a stale hit. */
    const std::string key = shader_key(hlsl, entry, profile, spirv, extra,
                                       nextra);
    {
        std::lock_guard<std::mutex> lk(g_shader_memo_mx);
        std::map<std::string, std::vector<unsigned char> >::iterator mi =
            shader_memo().find(key);
        if (mi != shader_memo().end()) {
            out = mi->second;
            return true;
        }
    }
    if (read_file(shader_cache_path(key), out)) {
        if (!out.empty()) {
            std::lock_guard<std::mutex> lk(g_shader_memo_mx);
            shader_memo()[key] = out;
            return true;
        }
        /* corrupt/truncated entry: fall through and recompile */
        out.clear();
    }
    IDxcCompiler3* compiler = nullptr;
    HRESULT hr = g_dxc_create(CLSID_DxcCompiler, IID_DXC_Compiler3,
                              reinterpret_cast<void**>(&compiler));
    if (FAILED(hr) || !compiler) {
        std::fprintf(stderr, "[gpu] DxcCreateInstance(Compiler) failed: 0x%lx\n",
                     static_cast<unsigned long>(hr));
        return false;
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
        return false;
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
        return false;
    }
    IDxcBlob* blob = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, IID_DXC_Blob,
                      reinterpret_cast<void**>(&blob), nullptr);
    result->Release();
    if (!blob) {
        std::fprintf(stderr, "[gpu] DXC %s: no object blob\n", entry);
        return false;
    }
    const unsigned char* bytes =
        static_cast<const unsigned char*>(blob->GetBufferPointer());
    out.assign(bytes, bytes + blob->GetBufferSize());
    blob->Release();
    if (out.empty()) {
        return false;
    }
    /* persist for the next run; a failed write only costs a recompile */
    write_file_atomic(shader_cache_path(key), out);
    {
        std::lock_guard<std::mutex> lk(g_shader_memo_mx);
        shader_memo()[key] = out;
    }
    return true;
#else
    (void)hlsl;
    (void)entry;
    (void)profile;
    (void)spirv;
    (void)extra;
    (void)nextra;
    (void)out;
    return false;
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
    std::vector<unsigned char> bytecode;
    if (!compile_dxil(hlsl, entry, profile.c_str(), vulkan, nullptr, 0,
                      bytecode)) {
        return nullptr;
    }
    SDL_GPUShaderCreateInfo sci;
    std::memset(&sci, 0, sizeof(sci));
    sci.code = bytecode.data();
    sci.code_size = bytecode.size();
    sci.entrypoint = entry;
    sci.format = vulkan ? SDL_GPU_SHADERFORMAT_SPIRV
                        : SDL_GPU_SHADERFORMAT_DXIL;
    sci.stage = fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT
                         : SDL_GPU_SHADERSTAGE_VERTEX;
    sci.num_samplers = fragment ? 1 : 0;
    SDL_GPUShader* sh = SDL_CreateGPUShader(dev, &sci);
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
    g->pipe_filter_cs = nullptr;
    g->pipe_mixblend_cs = nullptr;
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
    g->vb_solid_late = nullptr;
    g->shadow_params_buf = nullptr;
    g->backdrop_params_buf = nullptr;
    g->inset_params_buf = nullptr;
    g->filter_params_buf = nullptr;
    g->mixblend_params_buf = nullptr;
    g->vb_transfer = nullptr;
    g->atlas_transfer = nullptr;
    g->layer_transfer[0] = nullptr;
    g->layer_transfer[1] = nullptr;
    g->layer_cur = 0;
    g->shadow_transfer = nullptr;
    g->pipe_text_composite = nullptr;
    g->text_layer = nullptr;
    g->layer_dirty = 0;
    g->late_mode = 0;
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
    SDL_GPUShader* vs_t = compile_shader(device, kTextVS, "main", false);
    SDL_GPUShader* ps_t = compile_shader(device, kTextPS, "main", true);
    if (!vs_s || !ps_s || !vs_t || !ps_t) {
        goto fail;
    }

    /* vertex layout for the solid pipeline (gpu_vert_solid) */
    SDL_GPUVertexAttribute attrs[10];
    std::memset(attrs, 0, sizeof(attrs));
    attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0};
    attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8};
    attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16};
    attrs[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 32};
    attrs[4] = {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 40};
    attrs[5] = {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 44};
    attrs[6] = {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 52};
    attrs[7] = {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 56};
    attrs[8] = {8, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 64};
    attrs[9] = {9, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 72};
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
    vis.num_vertex_attributes = 10;
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

    /* CPU-rasterized text layer. R8G8B8A8, and the upload swaps R/B into
     * that order (whaleui_gpu_text_layer). The layer format must NOT be
     * B8G8R8A8: SDL's DXIL reflection decodes Load/Sample channel order
     * from the format NAME, so a B8G8R8A8 layer carries a second swap on
     * the D3D12 backend and the two cancel out (red text came out blue on
     * Windows while Vulkan was correct - a cross-backend difference no
     * single shader line can express). Swapping once on the CPU keeps every
     * backend reading plain RGBA. */
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
    g->vb_solid_late = make_vertex_buffer(device, 256 * 1024);
    /* blur parameter storage: header {count,fb_w,fb_h,0} + up to 64
     * records. shadow/backdrop records are 12 floats (3 float4), inset
     * records 16 floats (4 float4), filter records 28 floats (7 float4) -
     * each buffer fits its own stride, or the compute upload overruns the
     * buffer and the D3D12 backend fails the command list close with
     * E_INVALIDARG (a full-black frame). */
    {
        SDL_GPUBufferCreateInfo bci;
        std::memset(&bci, 0, sizeof(bci));
        bci.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
        bci.size = (64 * 12 + 4) * sizeof(float);
        g->shadow_params_buf = SDL_CreateGPUBuffer(device, &bci);
        g->backdrop_params_buf = SDL_CreateGPUBuffer(device, &bci);
        bci.size = (64 * 16 + 4) * sizeof(float);
        g->inset_params_buf = SDL_CreateGPUBuffer(device, &bci);
        bci.size = (64 * 24 + 4) * sizeof(float);
        g->filter_params_buf = SDL_CreateGPUBuffer(device, &bci);
        bci.size = (64 * 12 + 4) * sizeof(float);
        g->mixblend_params_buf = SDL_CreateGPUBuffer(device, &bci);
    }
    SDL_GPUTransferBufferCreateInfo tbi;
    std::memset(&tbi, 0, sizeof(tbi));
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    /* solids + late bodies + texts segments */
    tbi.size = 1024 * 1024;
    g->vb_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    tbi.size = 128 * 1024;
    g->shadow_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    tbi.size = 2048 * 2048;
    g->atlas_transfer = SDL_CreateGPUTransferBuffer(device, &tbi);
    tbi.size = static_cast<uint32_t>(static_cast<size_t>(w) * h * 4);
    g->layer_transfer[0] = SDL_CreateGPUTransferBuffer(device, &tbi);
    g->layer_transfer[1] = SDL_CreateGPUTransferBuffer(device, &tbi);
    if (!g->vb_solid || !g->vb_text || !g->vb_shapes || !g->vb_solid_late ||
        !g->shadow_params_buf || !g->backdrop_params_buf ||
        !g->inset_params_buf || !g->filter_params_buf ||
        !g->mixblend_params_buf ||
        !g->vb_transfer || !g->shadow_transfer || !g->atlas_transfer ||
        !g->layer_transfer[0] || !g->layer_transfer[1]) {
        goto fail;
    }

    /* text-composite compute pipeline: text_layer (t0,t1) -> target2 (u0) */
    {
        std::vector<unsigned char> cs;
        if (!compile_dxil(kTextCompositeCS, "main", L"cs_6_0",
                          vulkan_driver(device), nullptr, 0, cs)) {
            goto fail;
        }
        SDL_GPUComputePipelineCreateInfo cpi;
        std::memset(&cpi, 0, sizeof(cpi));
        cpi.code = cs.data();
        cpi.code_size = cs.size();
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
        if (!g->pipe_text_composite) {
            std::fprintf(stderr, "[gpu] text composite pipeline failed: %s\n",
                         SDL_GetError());
            goto fail;
        }
    }

    /* blur compute pipelines: blur_tex (t0) + params (t1) -> target (u0) */
    {
        std::vector<unsigned char> cs;
        if (!compile_dxil(kShadowCS.c_str(), "main", L"cs_6_0",
                          vulkan_driver(device), nullptr, 0, cs)) {
            goto fail;
        }
        SDL_GPUComputePipelineCreateInfo cpi;
        std::memset(&cpi, 0, sizeof(cpi));
        cpi.code = cs.data();
        cpi.code_size = cs.size();
        cpi.entrypoint = "main";
        cpi.format = vulkan_driver(device) ? SDL_GPU_SHADERFORMAT_SPIRV
                                           : SDL_GPU_SHADERFORMAT_DXIL;
        cpi.num_samplers = 1;
        cpi.num_readonly_storage_buffers = 1;
        cpi.num_readwrite_storage_textures = 1;
        cpi.num_uniform_buffers = 1; /* dispatch region offset */
        cpi.threadcount_x = 8;
        cpi.threadcount_y = 8;
        cpi.threadcount_z = 1;
        g->pipe_shadow_cs = SDL_CreateGPUComputePipeline(device, &cpi);
        if (!g->pipe_shadow_cs) {
            std::fprintf(stderr, "[gpu] shadow CS pipeline failed: %s\n",
                         SDL_GetError());
            goto fail;
        }
        if (!compile_dxil(kBackdropCS.c_str(), "main", L"cs_6_0",
                          vulkan_driver(device), nullptr, 0, cs)) {
            goto fail;
        }
        cpi.code = cs.data();
        cpi.code_size = cs.size();
        g->pipe_backdrop_cs = SDL_CreateGPUComputePipeline(device, &cpi);
        if (!g->pipe_backdrop_cs) {
            std::fprintf(stderr, "[gpu] backdrop CS pipeline failed: %s\n",
                         SDL_GetError());
            goto fail;
        }
        if (!compile_dxil(kInsetCS.c_str(), "main", L"cs_6_0",
                          vulkan_driver(device), nullptr, 0, cs)) {
            goto fail;
        }
        cpi.code = cs.data();
        cpi.code_size = cs.size();
        g->pipe_inset_cs = SDL_CreateGPUComputePipeline(device, &cpi);
        if (!g->pipe_inset_cs) {
            std::fprintf(stderr, "[gpu] inset CS pipeline failed: %s\n",
                         SDL_GetError());
            goto fail;
        }
        if (!compile_dxil(kFilterCS.c_str(), "main", L"cs_6_0",
                          vulkan_driver(device), nullptr, 0, cs)) {
            goto fail;
        }
        cpi.code = cs.data();
        cpi.code_size = cs.size();
        g->pipe_filter_cs = SDL_CreateGPUComputePipeline(device, &cpi);
        if (!g->pipe_filter_cs) {
            std::fprintf(stderr, "[gpu] filter CS pipeline failed: %s\n",
                         SDL_GetError());
            goto fail;
        }
        if (!compile_dxil(kMixBlendCS.c_str(), "main", L"cs_6_0",
                          vulkan_driver(device), nullptr, 0, cs)) {
            goto fail;
        }
        cpi.code = cs.data();
        cpi.code_size = cs.size();
        cpi.num_samplers = 1;
        g->pipe_mixblend_cs = SDL_CreateGPUComputePipeline(device, &cpi);
        if (!g->pipe_mixblend_cs) {
            std::fprintf(stderr, "[gpu] mixblend CS pipeline failed: %s\n",
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
    if (g->layer_transfer[0]) {
        SDL_ReleaseGPUTransferBuffer(dev, g->layer_transfer[0]);
    }
    if (g->layer_transfer[1]) {
        SDL_ReleaseGPUTransferBuffer(dev, g->layer_transfer[1]);
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
    g->layer_transfer[0] = SDL_CreateGPUTransferBuffer(dev, &tbi);
    g->layer_transfer[1] = SDL_CreateGPUTransferBuffer(dev, &tbi);
    g->fb_w = static_cast<float>(w);
    g->fb_h = static_cast<float>(h);
    g->layer_rx = g->layer_ry = g->layer_rw = g->layer_rh = 0;
    g->layer_dirty = 0;
    return (g->target && g->target_b && g->target2 && g->text_layer &&
            g->blur_tex && g->layer_transfer[0] && g->layer_transfer[1])
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
        if (g->pipe_text) SDL_ReleaseGPUGraphicsPipeline(g->device, g->pipe_text);
        if (g->pipe_text_composite) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_text_composite);
        if (g->pipe_shadow_cs) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_shadow_cs);
        if (g->pipe_backdrop_cs) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_backdrop_cs);
        if (g->pipe_inset_cs) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_inset_cs);
        if (g->pipe_filter_cs) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_filter_cs);
        if (g->pipe_mixblend_cs) SDL_ReleaseGPUComputePipeline(g->device, g->pipe_mixblend_cs);
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
        if (g->vb_solid_late) SDL_ReleaseGPUBuffer(g->device, g->vb_solid_late);
        if (g->shadow_params_buf) SDL_ReleaseGPUBuffer(g->device, g->shadow_params_buf);
        if (g->backdrop_params_buf) SDL_ReleaseGPUBuffer(g->device, g->backdrop_params_buf);
        if (g->inset_params_buf) SDL_ReleaseGPUBuffer(g->device, g->inset_params_buf);
        if (g->filter_params_buf) SDL_ReleaseGPUBuffer(g->device, g->filter_params_buf);
        if (g->mixblend_params_buf) SDL_ReleaseGPUBuffer(g->device, g->mixblend_params_buf);
        if (g->vb_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->vb_transfer);
        if (g->atlas_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->atlas_transfer);
        if (g->layer_transfer[0]) SDL_ReleaseGPUTransferBuffer(g->device, g->layer_transfer[0]);
        if (g->layer_transfer[1]) SDL_ReleaseGPUTransferBuffer(g->device, g->layer_transfer[1]);
        if (g->shadow_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->shadow_transfer);
    }
    delete g;
}

void whaleui_gpu_rect(whaleui_gpu_t* g, float x, float y, float w, float h,
                      float radius, unsigned int color, const int* clip,
                      float ctype, float ccx, float ccy, float cr0, float cr1,
                      float crad)
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
    /* late_mode routes a backdrop-filter element's own body into the late
     * draw list (drawn after the blur passes) */
    std::vector<gpu_vert_solid>& out = g->late_mode ? g->solids_late
                                                    : g->solids;
    if (out.size() + 6 > 65536) {
        return; /* vertex buffer cap */
    }
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float gg = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;
    /* clamp the corner radius to half the smaller side: the SDF computes
     * d = length(q) - radius with q >= -(half - radius), so a radius
     * larger than half (999px pill, common for chips/tags) makes every
     * pixel's d large and positive -> alpha 0 -> the element's background
     * vanished (it showed the layer below). The CPU path clamps; the GPU
     * vertex path must too. */
    float hr = w < h ? w : h;
    if (radius > hr * 0.5f) {
        radius = hr * 0.5f;
    }
    gpu_vert_solid v[6];
    /* two triangles, UV 0..1, same color, size + radius + fb size.
     * Clip shape rides along (local coords of this quad). */
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
        v[i].ctype = ctype;
        v[i].ccx = ccx;
        v[i].ccy = ccy;
        v[i].cr0 = cr0;
        v[i].cr1 = cr1;
        v[i].crad = crad;
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

void whaleui_gpu_late_begin(whaleui_gpu_t* g)
{
    if (g) {
        ++g->late_mode;
    }
}

void whaleui_gpu_late_end(whaleui_gpu_t* g)
{
    if (g && g->late_mode > 0) {
        --g->late_mode;
    }
}

void whaleui_gpu_rect_rot(whaleui_gpu_t* g, float x, float y, float w,
                          float h, float radius, unsigned int color,
                          float rot_deg, const int* clip, float ax, float ay,
                          float skx_deg, float sky_deg, float ctype,
                          float ccx, float ccy, float cr0, float cr1,
                          float crad)
{
    if (!g || w <= 0 || h <= 0) {
        return;
    }
    /* no clip-rect pre-shrink here: a rotated box's intersection with the
     * clip is not a rectangle; off-viewport pixels are dropped by the
     * rasterizer's viewport instead */
    std::vector<gpu_vert_solid>& out = g->late_mode ? g->solids_late
                                                    : g->solids;
    if (out.size() + 6 > 65536) {
        return; /* vertex buffer cap */
    }
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float gg = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;
    float hr = w < h ? w : h;
    if (radius > hr * 0.5f) {
        radius = hr * 0.5f;
    }
    /* rotate the local corner offsets around the anchor (screen coords,
     * y down: a positive angle turns clockwise like CSS rotate). The
     * default anchor is the box center; transform-origin moves it.
     * Skew applies first (shear around the same anchor), then rotation -
     * the composed order matches the accumulated eval approximately. */
    const float PI_F = 3.14159265358979f;
    float rad = rot_deg * PI_F / 180.0f;
    float cs = std::cos(rad);
    float sn = std::sin(rad);
    float tx = 0, ty = 0; /* skew tangents (0 = no skew) */
    if (skx_deg != 0.0f) {
        tx = std::tan(skx_deg * PI_F / 180.0f);
        if (tx > 10.0f) {
            tx = 10.0f;
        } else if (tx < -10.0f) {
            tx = -10.0f;
        }
    }
    if (sky_deg != 0.0f) {
        ty = std::tan(sky_deg * PI_F / 180.0f);
        if (ty > 10.0f) {
            ty = 10.0f;
        } else if (ty < -10.0f) {
            ty = -10.0f;
        }
    }
    float cx = (ax >= 0.0f) ? ax : x + w * 0.5f;
    float cy = (ay >= 0.0f) ? ay : y + h * 0.5f;
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    gpu_vert_solid v[6];
    for (int i = 0; i < 6; ++i) {
        v[i].r = r;
        v[i].g = gg;
        v[i].b = b;
        v[i].a = a;
        v[i].size_x = w;
        v[i].size_y = h;
        v[i].radius = radius;
        v[i].fb_w = g->fb_w;
        v[i].fb_h = g->fb_h;
        v[i].ctype = ctype;
        v[i].ccx = ccx;
        v[i].ccy = ccy;
        v[i].cr0 = cr0;
        v[i].cr1 = cr1;
        v[i].crad = crad;
    }
    /* corner k: (dx,dy) local offset, (u,v) its 0..1 box coordinate - the
     * pairing must stay TL,TR,BL,TR,BL,BR so the SDF frame survives.
     * dx/dy are relative to the box center; (ox0, oy0) shifts them onto
     * the anchor so an off-center origin rotates correctly. NOTE: the clip
     * shape (cc/cr, local coords of this quad) rides the SAME corner
     * transform implicitly - u/v/size stay local, so the SDF clip follows
     * rotation/skew exactly like the body rounding. */
    const float ox0 = cx - (x + hw);
    const float oy0 = cy - (y + hh);
    float dx[4] = {-hw, hw, -hw, hw};
    float dy[4] = {-hh, -hh, hh, hh};
    float uu[4] = {0, 1, 0, 1};
    float vv[4] = {0, 0, 1, 1};
    float px[4];
    float py[4];
    for (int k = 0; k < 4; ++k) {
        float rx = dx[k] - ox0;
        float ry = dy[k] - oy0;
        /* skewX shifts x by tan*y, skewY shifts y by tan*x (around the
         * anchor, so the anchor pixel stays put) */
        rx += tx * ry;
        ry += ty * rx;
        px[k] = cx + rx * cs - ry * sn;
        py[k] = cy + rx * sn + ry * cs;
    }
    const int idx[6] = {0, 1, 2, 1, 2, 3};
    for (int i = 0; i < 6; ++i) {
        v[i].x = px[idx[i]];
        v[i].y = py[idx[i]];
        v[i].u = uu[idx[i]];
        v[i].v = vv[idx[i]];
        out.push_back(v[i]);
    }
}

void whaleui_gpu_gradient_rect(whaleui_gpu_t* g, float x, float y, float w,
                               float h, unsigned int c0, unsigned int c1,
                               unsigned int c2, unsigned int c3,
                               const int* clip, float ctype, float ccx,
                               float ccy, float cr0, float cr1, float crad,
                               float radius)
{
    if (!g || w <= 0 || h <= 0) {
        return;
    }
    std::vector<gpu_vert_solid>& out = g->late_mode ? g->solids_late
                                                    : g->solids;
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
    if (out.size() + 6 > 65536) {
        return;
    }
    /* corner colors: v[0]=TL v[1]=TR v[2]=BL v[3]=TR v[4]=BL v[5]=BR.
     * radius is normally 0 (square gradient tiles); the masked rounded-rect
     * path passes the real radius so one draw carries both the corner SDF
     * and the per-corner mask alpha (they multiply in the shader). */
    unsigned int cols[6] = {c0, c1, c2, c1, c2, c3};
    gpu_vert_solid v[6];
    float hr = w < h ? w : h;
    float rad = radius;
    if (rad > hr * 0.5f) {
        rad = hr * 0.5f;
    }
    for (int i = 0; i < 6; ++i) {
        v[i].x = x;
        v[i].y = y;
        v[i].r = ((cols[i] >> 16) & 0xFF) / 255.0f;
        v[i].g = ((cols[i] >> 8) & 0xFF) / 255.0f;
        v[i].b = (cols[i] & 0xFF) / 255.0f;
        v[i].a = ((cols[i] >> 24) & 0xFF) / 255.0f;
        v[i].size_x = w;
        v[i].size_y = h;
        v[i].radius = rad;
        v[i].fb_w = g->fb_w;
        v[i].fb_h = g->fb_h;
        v[i].ctype = ctype;
        v[i].ccx = ccx;
        v[i].ccy = ccy;
        v[i].cr0 = cr0;
        v[i].cr1 = cr1;
        v[i].crad = crad;
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
        v[i].ctype = 0;
        v[i].ccx = v[i].ccy = v[i].cr0 = v[i].cr1 = v[i].crad = 0;
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
                       float radius, float blur, float ox, float oy,
                       float spread, unsigned int color)
{
    if (!g || w <= 0 || h <= 0) {
        return;
    }
    if (g->insets.size() >= 64) {
        return;
    }
    /* the mask source is the box SHRUNK by spread (a white rounded rect):
     * the shadow is 1 - blurred_mask(px - offset) inside the element box
     * (see kInsetCS) - the rim outside the covering box is the shadow.
     * blur 0 is legal - the CS samples the crisp level-0 mask. */
    const float s = spread > 0 ? spread : 0.0f;
    const float gw = w - 2.0f * s;
    const float gh = h - 2.0f * s;
    if (gw > 0 && gh > 0) {
        float gr = radius - s;
        if (gr < 0) {
            gr = 0;
        }
        const float ghalf = (gw < gh ? gw : gh) * 0.5f;
        if (gr > ghalf) {
            gr = ghalf;
        }
        push_solid_into(g->inset_shapes, (x + s) / static_cast<float>(kBlurDiv),
                        (y + s) / static_cast<float>(kBlurDiv),
                        gw / static_cast<float>(kBlurDiv),
                        gh / static_cast<float>(kBlurDiv),
                        gr / static_cast<float>(kBlurDiv), 0xFFFFFFFF,
                        static_cast<float>(g->blur_w),
                        static_cast<float>(g->blur_h));
    } /* shrunk away entirely: no mask -> the whole box is shadow */
    gpu_inset_param p;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.blur = blur > 0 ? blur : 0.0f;
    p.r = ((color >> 16) & 0xFF) / 255.0f;
    p.g = ((color >> 8) & 0xFF) / 255.0f;
    p.b = (color & 0xFF) / 255.0f;
    p.a = ((color >> 24) & 0xFF) / 255.0f;
    p.ox = ox;
    p.oy = oy;
    p.spread = s;
    p.radius = radius;
    const float half = (w < h ? w : h) * 0.5f;
    if (p.radius > half) {
        p.radius = half;
    }
    p.pad0 = p.pad1 = p.pad2 = 0;
    g->insets.push_back(p);
}

void whaleui_gpu_backdrop(whaleui_gpu_t* g, float x, float y, float w,
                          float h, float radius, float blur,
                          unsigned int body_color)
{
    (void)body_color; /* the body paints in the late pass, over the blur */
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
    /* radius rides in the unused record color slot (the CS SDF-clips the
     * region to the rounded box); the body blend is off (a = 0) */
    p.r = radius > 0 ? radius : 0.0f;
    p.g = p.b = 0;
    p.a = 0;
    p.br = p.bg = p.bb = 0;
    g->backdrops.push_back(p);
}

void whaleui_gpu_filter(whaleui_gpu_t* g, float x, float y, float w,
                        float h, float radius, float blur,
                        const float m[4][5])
{
    if (!g || w <= 0 || h <= 0 || !m) {
        return;
    }
    if (g->filters.size() >= 64) {
        return;
    }
    gpu_filter_param p;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.blur = blur > 0 ? blur : 0.0f;
    p.radius = radius > 0 ? radius : 0.0f;
    p.pad0 = p.pad1 = 0;
    for (int i = 0; i < 4; ++i) {
        p.r0[i] = m[0][i];
        p.r1[i] = m[1][i];
        p.r2[i] = m[2][i];
        p.r3[i] = m[3][i];
    }
    /* bias rides in [3] (the shader reads m.w as bias) */
    p.r0[3] = m[0][4];
    p.r1[3] = m[1][4];
    p.r2[3] = m[2][4];
    p.r3[3] = m[3][4];
    g->filters.push_back(p);
}

void whaleui_gpu_mixblend(whaleui_gpu_t* g, float x, float y, float w,
                           float h, float radius, int mode,
                           unsigned int body_color)
{
    if (!g || w <= 0 || h <= 0 || mode < 0) {
        return;
    }
    if (g->mixblends.size() >= 64) {
        return;
    }
    gpu_mixblend_param p;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.mode = static_cast<float>(mode);
    p.radius = radius > 0 ? radius : 0.0f;
    p.pad0 = p.pad1 = 0;
    p.sr = ((body_color >> 16) & 0xFF) / 255.0f;
    p.sg = ((body_color >> 8) & 0xFF) / 255.0f;
    p.sb = (body_color & 0xFF) / 255.0f;
    p.sa = ((body_color >> 24) & 0xFF) / 255.0f;
    g->mixblends.push_back(p);
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
    if (!g || !pixels || w <= 0 || h <= 0 || !g->layer_transfer[0] ||
        !g->layer_transfer[1]) {
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
    SDL_GPUTransferBuffer* lt = g->layer_transfer[g->layer_cur];
    void* mapped = SDL_MapGPUTransferBuffer(g->device, lt, false);
    if (!mapped) {
        return;
    }
    /* CPU pixels are 0xAARRGGBB, the layer is R8G8B8A8: swap R/B per pixel
     * on the way in. Doing it here, once, on the dirty region keeps every
     * backend reading plain RGBA - a B8G8R8A8 layer plus a shader swap
     * cancels out on D3D12, whose Load already decodes by the format name. */
    unsigned char* dst = static_cast<unsigned char*>(mapped);
    for (int yy = ry; yy < ry + rh; ++yy) {
        const unsigned int* s = pixels + static_cast<size_t>(yy) * w + rx;
        unsigned int* d = reinterpret_cast<unsigned int*>(
            dst + static_cast<size_t>(yy - ry) * w * 4);
        for (int xx = 0; xx < rw; ++xx) {
            const unsigned int v = s[xx];
            d[xx] = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) |
                    ((v & 0x000000FFu) << 16);
        }
    }
    SDL_UnmapGPUTransferBuffer(g->device, lt);
    g->layer_cur = 1 - g->layer_cur; /* next frame uploads into the other */
    g->layer_rx = rx;
    g->layer_ry = ry;
    g->layer_rw = rw;
    g->layer_rh = rh;
    g->layer_dirty = 1;
}

/* bounding region of a record list (each record starts with float4
 * {x,y,w,h}, `stride4` float4 per record), clamped to the framebuffer.
 * Returns false when nothing is on screen - the caller skips the dispatch.
 * The region passes to the compute shader as the thread-id offset, so a
 * 4k frame with one hovered button dispatches ~200 workgroups instead of
 * ~32k (the per-pixel record bounds check stays as the inner filter). */
static bool region_of_records(const float* recs, size_t count,
                              size_t stride4, Uint32 fb_w, Uint32 fb_h,
                              Uint32 out[4])
{
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (size_t i = 0; i < count; ++i, recs += stride4 * 4) {
        const float x = recs[0], y = recs[1], w = recs[2], h = recs[3];
        if (!(w > 0.0f) || !(h > 0.0f)) {
            continue;
        }
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + w > x1) x1 = x + w;
        if (y + h > y1) y1 = y + h;
    }
    if (x1 <= 0.0f || y1 <= 0.0f || x0 >= static_cast<float>(fb_w) ||
        y0 >= static_cast<float>(fb_h)) {
        return false;
    }
    if (x0 < 0.0f) x0 = 0.0f;
    if (y0 < 0.0f) y0 = 0.0f;
    if (x1 > static_cast<float>(fb_w)) x1 = static_cast<float>(fb_w);
    if (y1 > static_cast<float>(fb_h)) y1 = static_cast<float>(fb_h);
    /* a thread's sample point is p + 0.5: start at floor, end at ceil */
    out[0] = static_cast<Uint32>(x0);
    out[1] = static_cast<Uint32>(y0);
    out[2] = static_cast<Uint32>(x1) - out[0];
    out[3] = static_cast<Uint32>(y1) - out[1];
    if (out[2] < 1) {
        out[2] = 1;
    }
    if (out[3] < 1) {
        out[3] = 1;
    }
    if (out[0] + out[2] > fb_w) {
        out[2] = fb_w - out[0];
    }
    if (out[1] + out[3] > fb_h) {
        out[3] = fb_h - out[1];
    }
    return out[2] > 0 && out[3] > 0;
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
                   !g->solids_late.empty() ||
                   g->atlas_dirty || g->layer_dirty;
    bool need_svb = !g->shapes.empty() || !g->inset_shapes.empty() ||
                    !g->shadows.empty() || !g->backdrops.empty() ||
                    !g->insets.empty() || !g->filters.empty() ||
                    !g->mixblends.empty();
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
                off += g->texts.size() * sizeof(gpu_vert_text);
            }
            if (!g->solids_late.empty()) {
                std::memcpy(static_cast<char*>(mapped) + off,
                            g->solids_late.data(),
                            g->solids_late.size() * sizeof(gpu_vert_solid));
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
                bti.offset = br.size;
            }
            if (!g->texts.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->vb_text;
                br.offset = 0;
                br.size = g->texts.size() * sizeof(gpu_vert_text);
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
                bti.offset += br.size;
            }
            if (!g->solids_late.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->vb_solid_late;
                br.offset = 0;
                br.size = g->solids_late.size() * sizeof(gpu_vert_solid);
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
                ti.transfer_buffer = g->layer_transfer[1 - g->layer_cur];
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
                            g->insets.size() * sizeof(gpu_inset_param));
                off += 16 + g->insets.size() * sizeof(gpu_inset_param);
            }
            if (!g->filters.empty()) {
                float* hdr = static_cast<float*>(m2) + off / sizeof(float);
                hdr[0] = static_cast<float>(g->filters.size());
                hdr[1] = g->fb_w;
                hdr[2] = g->fb_h;
                hdr[3] = 0;
                std::memcpy(static_cast<char*>(m2) + off + 16,
                            g->filters.data(),
                            g->filters.size() * sizeof(gpu_filter_param));
                off += 16 + g->filters.size() * sizeof(gpu_filter_param);
            }
            if (!g->mixblends.empty()) {
                float* hdr = static_cast<float*>(m2) + off / sizeof(float);
                hdr[0] = static_cast<float>(g->mixblends.size());
                hdr[1] = g->fb_w;
                hdr[2] = g->fb_h;
                hdr[3] = 0;
                std::memcpy(static_cast<char*>(m2) + off + 16,
                            g->mixblends.data(),
                            g->mixblends.size() * sizeof(gpu_mixblend_param));
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
                br.size = 16 + g->insets.size() * sizeof(gpu_inset_param);
                bti.offset = off;
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
                off += 16 + g->insets.size() * sizeof(gpu_inset_param);
            }
            if (!g->filters.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->filter_params_buf;
                br.offset = 0;
                br.size = 16 + g->filters.size() * sizeof(gpu_filter_param);
                bti.offset = off;
                SDL_UploadToGPUBuffer(cp, &bti, &br, false);
                off += 16 + g->filters.size() * sizeof(gpu_filter_param);
            }
            if (!g->mixblends.empty()) {
                SDL_GPUBufferRegion br;
                std::memset(&br, 0, sizeof(br));
                br.buffer = g->mixblend_params_buf;
                br.offset = 0;
                br.size = 16 + g->mixblends.size() * sizeof(gpu_mixblend_param);
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
        /* the region-scoped shadow CS no longer writes the pixels outside
         * its records: clear the target with a fixed-function pass instead
         * (the old full-screen compute "clear" was 8M read-modify-write
         * threads at 4k before the first real pixel). Transparent black,
         * exactly what the CS used to write. */
        SDL_GPUColorTargetInfo cct;
        std::memset(&cct, 0, sizeof(cct));
        cct.texture = g->geom_cur;
        cct.clear_color = SDL_FColor{0, 0, 0, 0};
        cct.load_op = SDL_GPU_LOADOP_CLEAR;
        cct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* ccp = SDL_BeginGPURenderPass(cmd, &cct, 1, nullptr);
        if (ccp) {
            SDL_EndGPURenderPass(ccp);
        }
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
        Uint32 sreg[4] = {0, 0, static_cast<Uint32>(fb_w),
                          static_cast<Uint32>(fb_h)};
        bool have_sreg = region_of_records(
            reinterpret_cast<const float*>(g->shadows.data()),
            g->shadows.size(), 3, static_cast<Uint32>(fb_w),
            static_cast<Uint32>(fb_h), sreg);
        if (have_sreg) {
            SDL_GPUStorageTextureReadWriteBinding rw;
            std::memset(&rw, 0, sizeof(rw));
            rw.texture = g->geom_cur;
            rw.mip_level = 0;
            rw.layer = 0;
            rw.cycle = false;
            SDL_GPUComputePass* cps =
                SDL_BeginGPUComputePass(cmd, &rw, 1, nullptr, 0);
            if (cps) {
                SDL_BindGPUComputePipeline(cps, g->pipe_shadow_cs);
                SDL_GPUTextureSamplerBinding tsb;
                std::memset(&tsb, 0, sizeof(tsb));
                tsb.texture = g->blur_tex;
                tsb.sampler = g->sampler_mip;
                SDL_BindGPUComputeSamplers(cps, 0, &tsb, 1);
                SDL_GPUBuffer* sbb = g->shadow_params_buf;
                SDL_BindGPUComputeStorageBuffers(cps, 0, &sbb, 1);
                SDL_PushGPUComputeUniformData(cmd, 0, sreg, sizeof(sreg));
                SDL_DispatchGPUCompute(cps, (sreg[2] + 7) / 8, (sreg[3] + 7) / 8,
                                       1);
                SDL_EndGPUComputePass(cps);
            }
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
    /* partial (hover/animation) frames repaint exactly the strip: scissor
     * the pass so the rasterizer discards everything outside it in fixed
     * function (the emitters already clip in the shader - this only skips
     * the tile sweep over the untouched rest of the target) */
    if (load_only && g->layer_rw > 0 && g->layer_rh > 0) {
        SDL_Rect sc;
        sc.x = g->layer_rx < 0 ? 0 : g->layer_rx;
        sc.y = g->layer_ry < 0 ? 0 : g->layer_ry;
        sc.w = g->layer_rw;
        sc.h = g->layer_rh;
        if (sc.x + sc.w > fb_w) {
            sc.w = fb_w - sc.x;
        }
        if (sc.y + sc.h > fb_h) {
            sc.h = fb_h - sc.y;
        }
        if (sc.w > 0 && sc.h > 0) {
            SDL_SetGPUScissor(rp, &sc);
        }
    }

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
     * blurred copy (the element's own body paints in the LATE pass after
     * it). Runs on partial (animation/hover) frames too, so a fixed
     * translucent header stays blurred while content animates under it. */
    if (!g->backdrops.empty() && scroll_dy == 0) {
        Uint32 dreg[4] = {0, 0, static_cast<Uint32>(fb_w),
                          static_cast<Uint32>(fb_h)};
        bool have_dreg = region_of_records(
            reinterpret_cast<const float*>(g->backdrops.data()),
            g->backdrops.size(), 3, static_cast<Uint32>(fb_w),
            static_cast<Uint32>(fb_h), dreg);
        if (have_dreg) {
            /* the blur source only needs the records' boxes plus the mip
             * footprint of the widest blur: level<=7 texels span 128
             * blur-tex px (256 fb px) and bilinear pulls the neighbour, so
             * a big blur needs ~520 fb px of margin - small blurs only
             * reach level 2 (<=4 blur-tex px). A header at the top no
             * longer copies the whole 4k framebuffer every animation
             * frame. */
            float max_blur = 0.0f;
            for (size_t i = 0; i < g->backdrops.size(); ++i) {
                if (g->backdrops[i].blur > max_blur) {
                    max_blur = g->backdrops[i].blur;
                }
            }
            int pad = max_blur >= 8.0f ? 520 : 16;
            int bx0 = static_cast<int>(dreg[0]) - pad;
            int by0 = static_cast<int>(dreg[1]) - pad;
            int bx1 = static_cast<int>(dreg[0] + dreg[2]) + pad;
            int by1 = static_cast<int>(dreg[1] + dreg[3]) + pad;
            if (bx0 < 0) bx0 = 0;
            if (by0 < 0) by0 = 0;
            if (bx1 > fb_w) bx1 = fb_w;
            if (by1 > fb_h) by1 = fb_h;
            /* even alignment: blur_tex is fb/kBlurDiv, so even source
             * coords map 1:1 onto destination texels (same convention as
             * the full-frame blit this replaces) */
            bx0 &= ~1;
            by0 &= ~1;
            bx1 = (bx1 + 1) & ~1;
            by1 = (by1 + 1) & ~1;
            if (bx1 > fb_w) bx1 = fb_w & ~1;
            if (by1 > fb_h) by1 = fb_h & ~1;
            if (bx1 > bx0 && by1 > by0) {
                SDL_GPUBlitInfo blit;
                std::memset(&blit, 0, sizeof(blit));
                blit.source.texture = g->geom_cur;
                blit.source.x = static_cast<Uint32>(bx0);
                blit.source.y = static_cast<Uint32>(by0);
                blit.source.w = static_cast<Uint32>(bx1 - bx0);
                blit.source.h = static_cast<Uint32>(by1 - by0);
                blit.destination.texture = g->blur_tex;
                blit.destination.x = static_cast<Uint32>(bx0 / kBlurDiv);
                blit.destination.y = static_cast<Uint32>(by0 / kBlurDiv);
                blit.destination.w = static_cast<Uint32>((bx1 - bx0) / kBlurDiv);
                blit.destination.h = static_cast<Uint32>((by1 - by0) / kBlurDiv);
                blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                blit.filter = SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(cmd, &blit);
                SDL_GenerateMipmapsForGPUTexture(cmd, g->blur_tex);

                /* read+write the target in one pass (SIMULTANEOUS) */
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
                    SDL_PushGPUComputeUniformData(cmd, 0, dreg,
                                                  sizeof(dreg));
                    SDL_DispatchGPUCompute(cps, (dreg[2] + 7) / 8,
                                           (dreg[3] + 7) / 8, 1);
                    SDL_EndGPUComputePass(cps);
                }
            }
        }
    }

    /* pass E: the bodies of backdrop-filter elements (solid background +
     * gradient + border), routed here by whaleui_gpu_late_begin. They draw
     * AFTER the backdrop compute pass (which would otherwise replace the
     * region - and the body with it) but BEFORE the inset pass, so an
     * element that has both still gets its inset shadow on top of its own
     * body. load_op LOAD: this pass never clears. */
    if (!g->solids_late.empty()) {
        SDL_GPUColorTargetInfo lct;
        std::memset(&lct, 0, sizeof(lct));
        lct.texture = g->geom_cur;
        lct.load_op = SDL_GPU_LOADOP_LOAD;
        lct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* lrp = SDL_BeginGPURenderPass(cmd, &lct, 1, nullptr);
        if (lrp) {
            SDL_GPUViewport lvp;
            std::memset(&lvp, 0, sizeof(lvp));
            lvp.w = static_cast<float>(fb_w);
            lvp.h = static_cast<float>(fb_h);
            lvp.min_depth = 0;
            lvp.max_depth = 1;
            SDL_SetGPUViewport(lrp, &lvp);
            SDL_BindGPUGraphicsPipeline(lrp, g->pipe_solid);
            SDL_GPUBufferBinding lbb;
            std::memset(&lbb, 0, sizeof(lbb));
            lbb.buffer = g->vb_solid_late;
            lbb.offset = 0;
            SDL_BindGPUVertexBuffers(lrp, 0, &lbb, 1);
            SDL_GPUTextureSamplerBinding ltsb;
            std::memset(&ltsb, 0, sizeof(ltsb));
            ltsb.texture = g->white_tex;
            ltsb.sampler = g->sampler;
            SDL_BindGPUFragmentSamplers(lrp, 0, &ltsb, 1);
            SDL_DrawGPUPrimitives(lrp,
                                  static_cast<int>(g->solids_late.size()), 1,
                                  0, 0);
            SDL_EndGPURenderPass(lrp);
        }
    }

    /* pass A2 + D: inset box-shadows. The spread-grown white box masks are
     * rasterized into blur_tex (freshly cleared - the backdrop copy above
     * already consumed it), mipmapped, and a compute pass blends
     * 1 - blurred_mask(px - offset) over the painted geometry (inset
     * shadows sit on top of the element background). */
    if (!g->insets.empty() && scroll_dy == 0 && !load_only) {
        Uint32 ireg[4] = {0, 0, static_cast<Uint32>(fb_w),
                          static_cast<Uint32>(fb_h)};
        bool have_ireg = region_of_records(
            reinterpret_cast<const float*>(g->insets.data()),
            g->insets.size(), 4, static_cast<Uint32>(fb_w),
            static_cast<Uint32>(fb_h), ireg);
        if (have_ireg) {
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
                SDL_BindGPUGraphicsPipeline(irp, g->pipe_solid);
                SDL_GPUBufferBinding ibb;
                std::memset(&ibb, 0, sizeof(ibb));
                ibb.buffer = g->vb_shapes;
                /* the mask run starts AFTER the shadow shapes in vb_shapes */
                ibb.offset = g->shapes.size() * sizeof(gpu_vert_solid);
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
                SDL_PushGPUComputeUniformData(cmd, 0, ireg, sizeof(ireg));
                SDL_DispatchGPUCompute(cps, (ireg[2] + 7) / 8, (ireg[3] + 7) / 8,
                                       1);
                SDL_EndGPUComputePass(cps);
            }
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
                             static_cast<Uint32>(rwpx),
                             static_cast<Uint32>(rhpx)};
            SDL_PushGPUComputeUniformData(cmd, 0, off, sizeof(off));
            SDL_DispatchGPUCompute(
                cps, (static_cast<Uint32>(rwpx) + 15) / 16,
                (static_cast<Uint32>(rhpx) + 15) / 16, 1);
            SDL_EndGPUComputePass(cps);
        }
        g->layer_dirty = 0;
    }

    /* pass E: element filters. Runs AFTER the text composite (target2 holds
     * the full frame incl. text): the filter applies to the element's own
     * painted content. When blur > 0 the composited frame is blitted into
     * blur_tex + mipmapped first (like the backdrop pass), then the compute
     * pass applies blur + the 4x5 color matrix in each region. Skipped on
     * scroll frames like the other blur passes. Dispatch + source blit are
     * region-scoped (see pass C). */
    if (!g->filters.empty() && scroll_dy == 0) {
        Uint32 freg[4] = {0, 0, static_cast<Uint32>(fb_w),
                          static_cast<Uint32>(fb_h)};
        bool have_freg = region_of_records(
            reinterpret_cast<const float*>(g->filters.data()),
            g->filters.size(), 6, static_cast<Uint32>(fb_w),
            static_cast<Uint32>(fb_h), freg);
        bool need_blur = false;
        float max_fblur = 0.0f;
        for (size_t fi = 0; fi < g->filters.size(); ++fi) {
            if (g->filters[fi].blur > 0) {
                need_blur = true;
                if (g->filters[fi].blur > max_fblur) {
                    max_fblur = g->filters[fi].blur;
                }
            }
        }
        if (have_freg && need_blur) {
            int pad = max_fblur >= 8.0f ? 520 : 16;
            int bx0 = static_cast<int>(freg[0]) - pad;
            int by0 = static_cast<int>(freg[1]) - pad;
            int bx1 = static_cast<int>(freg[0] + freg[2]) + pad;
            int by1 = static_cast<int>(freg[1] + freg[3]) + pad;
            if (bx0 < 0) bx0 = 0;
            if (by0 < 0) by0 = 0;
            if (bx1 > fb_w) bx1 = fb_w;
            if (by1 > fb_h) by1 = fb_h;
            bx0 &= ~1;
            by0 &= ~1;
            bx1 = (bx1 + 1) & ~1;
            by1 = (by1 + 1) & ~1;
            if (bx1 > fb_w) bx1 = fb_w & ~1;
            if (by1 > fb_h) by1 = fb_h & ~1;
            SDL_GPUBlitInfo blit;
            std::memset(&blit, 0, sizeof(blit));
            blit.source.texture = g->target2;
            blit.source.x = static_cast<Uint32>(bx0);
            blit.source.y = static_cast<Uint32>(by0);
            blit.source.w = static_cast<Uint32>(bx1 - bx0);
            blit.source.h = static_cast<Uint32>(by1 - by0);
            blit.destination.texture = g->blur_tex;
            blit.destination.x = static_cast<Uint32>(bx0 / kBlurDiv);
            blit.destination.y = static_cast<Uint32>(by0 / kBlurDiv);
            blit.destination.w = static_cast<Uint32>((bx1 - bx0) / kBlurDiv);
            blit.destination.h = static_cast<Uint32>((by1 - by0) / kBlurDiv);
            blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
            blit.filter = SDL_GPU_FILTER_LINEAR;
            SDL_BlitGPUTexture(cmd, &blit);
            SDL_GenerateMipmapsForGPUTexture(cmd, g->blur_tex);
        }
        if (have_freg) {
            SDL_GPUStorageTextureReadWriteBinding rw;
            std::memset(&rw, 0, sizeof(rw));
            rw.texture = g->target2;
            rw.mip_level = 0;
            rw.layer = 0;
            rw.cycle = false;
            SDL_GPUComputePass* cps =
                SDL_BeginGPUComputePass(cmd, &rw, 1, nullptr, 0);
            if (cps) {
                SDL_BindGPUComputePipeline(cps, g->pipe_filter_cs);
                SDL_GPUTextureSamplerBinding tsb;
                std::memset(&tsb, 0, sizeof(tsb));
                tsb.texture = g->blur_tex;
                tsb.sampler = g->sampler_mip;
                SDL_BindGPUComputeSamplers(cps, 0, &tsb, 1);
                SDL_GPUBuffer* sbb = g->filter_params_buf;
                SDL_BindGPUComputeStorageBuffers(cps, 0, &sbb, 1);
                SDL_PushGPUComputeUniformData(cmd, 0, freg, sizeof(freg));
                SDL_DispatchGPUCompute(cps, (freg[2] + 7) / 8,
                                       (freg[3] + 7) / 8, 1);
                SDL_EndGPUComputePass(cps);
            }
        }
    }

    /* pass F: mix-blend-mode. Runs with the filter pass (after the text
     * composite, on target2): the recorded body color blends over the
     * painted frame with the mode formula. Needs the frame as a sampled
     * texture AND read-write target: target2 has SIMULTANEOUS usage, and
     * the shader reads it via frame_tex Load (same texture, no copy). */
    if (!g->mixblends.empty() && scroll_dy == 0) {
        Uint32 mreg[4] = {0, 0, static_cast<Uint32>(fb_w),
                          static_cast<Uint32>(fb_h)};
        if (region_of_records(
                reinterpret_cast<const float*>(g->mixblends.data()),
                g->mixblends.size(), 3, static_cast<Uint32>(fb_w),
                static_cast<Uint32>(fb_h), mreg)) {
            SDL_GPUStorageTextureReadWriteBinding rw;
            std::memset(&rw, 0, sizeof(rw));
            rw.texture = g->target2;
            rw.mip_level = 0;
            rw.layer = 0;
            rw.cycle = false;
            SDL_GPUComputePass* cps =
                SDL_BeginGPUComputePass(cmd, &rw, 1, nullptr, 0);
            if (cps) {
                SDL_BindGPUComputePipeline(cps, g->pipe_mixblend_cs);
                /* sampler slot declared but unused (Load-only shader) */
                SDL_GPUTextureSamplerBinding tsb;
                std::memset(&tsb, 0, sizeof(tsb));
                tsb.texture = g->white_tex;
                tsb.sampler = g->sampler;
                SDL_BindGPUComputeSamplers(cps, 0, &tsb, 1);
                SDL_GPUBuffer* sbb = g->mixblend_params_buf;
                SDL_BindGPUComputeStorageBuffers(cps, 0, &sbb, 1);
                SDL_PushGPUComputeUniformData(cmd, 0, mreg, sizeof(mreg));
                SDL_DispatchGPUCompute(cps, (mreg[2] + 7) / 8,
                                       (mreg[3] + 7) / 8, 1);
                SDL_EndGPUComputePass(cps);
            }
        }
    }

    g->solids.clear();
    g->texts.clear();
    g->shadows.clear();
    g->backdrops.clear();
    g->insets.clear();
    g->filters.clear();
    g->mixblends.clear();
    g->shapes.clear();
    g->inset_shapes.clear();
    g->solids_late.clear();
    return cmd;
}




