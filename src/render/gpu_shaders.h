/* GPU rendering shaders.
 *
 * One HLSL source per stage, compiled at runtime to DXIL via D3DCompile
 * (system d3dcompiler_47.dll) for the D3D12 backend. The same sources can
 * be cross-compiled to SPIR-V for Vulkan / MSL for Metal later (SDL
 * shadercross) - the shaders are the single source of truth, no per-backend
 * copies. */

#ifndef WHALEUI_RENDER_GPU_SHADERS_H
#define WHALEUI_RENDER_GPU_SHADERS_H

#include <cstdint>

/* vertex: px position, uv (0..1 within the quad), color, quad size px,
 * corner radius px. SDL 3.4's D3D12 backend maps attribute locations to
 * "TEXCOORD"+location semantics (location 0 is TEXCOORD0, not POSITION).
 * Pixel->NDC conversion happens here via a per-frame uniform, so the CPU
 * never touches vertex data. */
static const char* kSolidVS = R"(
struct VSIn {
    float2 pos : TEXCOORD0;
    float2 uv  : TEXCOORD1;
    float4 col : TEXCOORD2;
    float2 size : TEXCOORD3;
    float radius : TEXCOORD4;
    float2 fb : TEXCOORD5;
};
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : TEXCOORD1;
    float2 size : TEXCOORD2;
    float radius : TEXCOORD3;
};
VSOut main(VSIn v) {
    VSOut o;
    float2 ndc = v.pos / v.fb * 2.0 - 1.0;
    ndc.y = -ndc.y; /* pixel y down, NDC y up */
    o.pos = float4(ndc, 0, 1);
    o.uv = v.uv;
    o.col = v.col;
    o.size = v.size;
    o.radius = v.radius;
    return o;
}
)";

/* pixel: rounded-rect distance field with 1px anti-aliasing; vertex colors
 * interpolate so linear gradients fall out of the rasterizer for free.
 * No texture (SDL 3.4 D3D12 graphics SRV fails with E_INVALIDARG). */
static const char* kSolidPS = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : TEXCOORD1;
    float2 size : TEXCOORD2;
    float radius : TEXCOORD3;
};
float4 main(VSOut i) : SV_Target {
    float4 c = i.col;
    float r = max(i.radius, 0);
    float2 half = i.size * 0.5; /* size is the full quad size; SDF wants half */
    float2 q = abs(i.uv - 0.5) * i.size - (half - r);
    float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
    float a = 1.0 - saturate(d + 0.5);
    c.a *= a;
    return c;
}
)";

/* text sprite: quad with atlas UV, tinted by the vertex color */
static const char* kTextVS = R"(
struct VSIn {
    float2 pos : TEXCOORD0;
    float2 uv  : TEXCOORD1;
    float4 col : TEXCOORD2;
};
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : TEXCOORD1;
};
VSOut main(VSIn v) {
    VSOut o;
    o.pos = float4(v.pos, 0, 1);
    o.uv = v.uv;
    o.col = v.col;
    return o;
}
)";

static const char* kTextPS = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};
float4 main(VSOut i) : SV_Target {
    float a = tex.Sample(samp, i.uv).r; /* alpha-only glyph atlas */
    return float4(i.col.rgb, i.col.a * a);
}
)";

/* 1x1 white texture so the solid pipeline can bind a texture (keeps one
 * blend/format path); the pixel shader uses vertex color only. */
static const unsigned char kSolidPixel[4] = {255, 255, 255, 255};

/* compute: composite the CPU-rasterized text layer (RGBA8, transparent
 * where empty) over the geometry already drawn into the target. SDL 3.4
 * SPIR-V layout: set0 = sampled textures, set1 = read-write storage. The
 * [[vk::binding]] attributes pin the D3D12 register order (t0/t1 space0,
 * u0 space1) to those sets. Load (not Sample): DXIL compute at cs_6_0
 * rejects Sample's implicit derivatives, and 1:1 texel reads don't need a
 * sampler. */
static const char* kTextCompositeCS = R"(
[[vk::binding(0, 0)]] Texture2D geometry : register(t0);
[[vk::binding(1, 0)]] Texture2D text_layer : register(t1);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_tex : register(u0, space1);
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float4 t = text_layer.Load(int3(id.xy, 0));
    if (t.a > 0.001) {
        out_tex[id.xy] = t;
    } else {
        out_tex[id.xy] = geometry.Load(int3(id.xy, 0));
    }
}
)";

#endif /* WHALEUI_RENDER_GPU_SHADERS_H */
