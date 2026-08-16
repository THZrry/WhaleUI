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

/* flat pixel shader: interpolated vertex color only (no rounded-rect SDF).
 * Used for the inset-shadow gradient triangles drawn into blur_tex - the
 * alpha ramp falls out of the rasterizer for free. VSOut must match
 * kSolidVS's outputs exactly (D3D12 rejects mismatched signatures). */
static const char* kFlatPS = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : TEXCOORD1;
    float2 size : TEXCOORD2;
    float radius : TEXCOORD3;
};
float4 main(VSOut i) : SV_Target {
    return i.col;
}
)";

/* blur sampling compute passes (box-shadow / backdrop-filter), the
 * mipmap-approximation of a gaussian: the source is mipmapped into
 * blur_tex, then each pixel re-samples 3 levels (gaussian weights) with
 * explicit LOD (SampleLevel - compute rejects implicit-derivative Sample,
 * and SDL 3.4 D3D12 graphics-side SRV is broken anyway). Parameters come
 * from a StructuredBuffer: [0]={count,fb_w,fb_h,0}, then per record
 * {x,y,w,h}, {blur,r,g,b}, {a,...} (a = shadow alpha for kShadowCS,
 * backdrop body color+alpha for kBackdropCS). */

static const char* kShadowCS = R"(
[[vk::binding(0, 0)]] Texture2D blur_tex : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> params : register(t1);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_tex : register(u0, space1);
SamplerState samp : register(s0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float2 px = float2(id.xy) + 0.5;
    float4 hdr = params[0];
    int n = int(hdr.x);
    float4 acc = 0;
    for (int i = 0; i < n; ++i) {
        float4 r = params[1 + i * 3];
        float4 c = params[2 + i * 3];
        float4 d = params[3 + i * 3];
        if (px.x >= r.x && px.x < r.x + r.z &&
            px.y >= r.y && px.y < r.y + r.w) {
            /* framebuffer px -> blur_tex uv (blur tex is fb / 2) */
            float2 uv = px / float2(hdr.y, hdr.z) * (1.0 / 2.0);
            /* blur radius -> mip level: a 2x2 box-filtered level L has an
             * equivalent radius of ~2^L blur_texels = 2^(L+1) px */
            float lod = clamp(log2(max(c.x, 1.0)) - 1.0, 0.0, 6.0);
            float4 m = 0;
            float ws = 0;
            for (int k = -1; k <= 1; ++k) {
                float w = exp(-0.5 * float(k * k));
                m += blur_tex.SampleLevel(samp, uv, max(lod + float(k), 0.0)) * w;
                ws += w;
            }
            m /= ws;
            acc = float4(c.yzw, d.x) * m.a;
            break;
        }
    }
    out_tex[id.xy] = acc;
}
)";

static const char* kBackdropCS = R"(
[[vk::binding(0, 0)]] Texture2D blur_tex : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> params : register(t1);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_tex : register(u0, space1);
SamplerState samp : register(s0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float2 px = float2(id.xy) + 0.5;
    float4 hdr = params[0];
    int n = int(hdr.x);
    /* default: keep the painted pixel (target is SIMULTANEOUS read+write) */
    float4 acc = out_tex[id.xy];
    for (int i = 0; i < n; ++i) {
        float4 r = params[1 + i * 3];
        float4 c = params[2 + i * 3];
        float4 d = params[3 + i * 3];
        if (px.x >= r.x && px.x < r.x + r.z &&
            px.y >= r.y && px.y < r.y + r.w) {
            float2 uv = px / float2(hdr.y, hdr.z) * (1.0 / 2.0);
            float lod = clamp(log2(max(c.x, 1.0)) - 1.0, 0.0, 6.0);
            float4 m = 0;
            float ws = 0;
            for (int k = -1; k <= 1; ++k) {
                float w = exp(-0.5 * float(k * k));
                m += blur_tex.SampleLevel(samp, uv, max(lod + float(k), 0.0)) * w;
                ws += w;
            }
            m /= ws;
            /* blurred background + the element's own body color. Record
             * layout: {x,y,w,h} {blur,r,g,b} {a,br,bg,bb} - for a backdrop
             * record a = body alpha and br/bg/bb = body color (d.yzw) */
            float ba = d.x;
            float3 blurred = m.rgb;
            float3 body = float3(d.y, d.z, d.w);
            float3 outcol = lerp(blurred, body, ba);
            float outa = m.a * (1.0 - ba) + ba;
            acc = float4(outcol, outa);
            break;
        }
    }
    out_tex[id.xy] = acc;
}
)";

/* inset box-shadow compute pass: blend the blurred gradient (the four
 * diagonal triangles in blur_tex) over the already-painted geometry.
 * Runs AFTER the geometry + backdrop passes, so `acc` starts from the
 * painted pixel (read+write) and the shadow color is blended on top.
 * Record layout mirrors kShadowCS: {x,y,w,h} {blur,r,g,b} {a,0,0,0}. */
static const char* kInsetCS = R"(
[[vk::binding(0, 0)]] Texture2D blur_tex : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> params : register(t1);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_tex : register(u0, space1);
SamplerState samp : register(s0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float2 px = float2(id.xy) + 0.5;
    float4 hdr = params[0];
    int n = int(hdr.x);
    float4 acc = out_tex[id.xy]; /* keep the painted background */
    for (int i = 0; i < n; ++i) {
        float4 r = params[1 + i * 3];
        float4 c = params[2 + i * 3];
        float4 d = params[3 + i * 3];
        if (px.x >= r.x && px.x < r.x + r.z &&
            px.y >= r.y && px.y < r.y + r.w) {
            float2 uv = px / float2(hdr.y, hdr.z) * (1.0 / 2.0);
            float lod = clamp(log2(max(c.x, 1.0)) - 1.0, 0.0, 6.0);
            float4 m = 0;
            float ws = 0;
            for (int k = -1; k <= 1; ++k) {
                float w = exp(-0.5 * float(k * k));
                m += blur_tex.SampleLevel(samp, uv, max(lod + float(k), 0.0)) * w;
                ws += w;
            }
            m /= ws;
            float sa = d.x * m.a; /* shadow alpha = color alpha * mask */
            acc = lerp(acc, float4(c.yzw, 1.0), sa);
            break;
        }
    }
    out_tex[id.xy] = acc;
}
)";

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
