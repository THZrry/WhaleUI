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
#include <string>

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
    float ctype : TEXCOORD6;
    float2 cc : TEXCOORD7;
    float2 cr : TEXCOORD8;
    float crad : TEXCOORD9;
};
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : TEXCOORD1;
    float2 size : TEXCOORD2;
    float radius : TEXCOORD3;
    float ctype : TEXCOORD4;
    float2 cc : TEXCOORD5;
    float2 cr : TEXCOORD6;
    float crad : TEXCOORD7;
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
    o.ctype = v.ctype;
    o.cc = v.cc;
    o.cr = v.cr;
    o.crad = v.crad;
    return o;
}
)";

/* pixel: rounded-rect distance field with 1px anti-aliasing; vertex colors
 * interpolate so linear gradients fall out of the rasterizer for free.
 * No texture (SDL 3.4 D3D12 graphics SRV fails with E_INVALIDARG).
 * clip-path (ctype/cc/cr/crad, local box coords) intersects a second SDF:
 * the pixel survives only inside BOTH shapes. */
static const char* kSolidPS = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : TEXCOORD1;
    float2 size : TEXCOORD2;
    float radius : TEXCOORD3;
    float ctype : TEXCOORD4;
    float2 cc : TEXCOORD5;
    float2 cr : TEXCOORD6;
    float crad : TEXCOORD7;
};
float4 main(VSOut i) : SV_Target {
    float4 c = i.col;
    float r = max(i.radius, 0);
    float2 half = i.size * 0.5; /* size is the full quad size; SDF wants half */
    float2 q = abs(i.uv - 0.5) * i.size - (half - r);
    float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
    if (i.ctype > 0.5 && i.ctype < 1.5) {
        /* inset rect: center cc + half-size cr (+ corner crad) */
        float rr = max(i.crad, 0);
        float2 qq = abs(i.uv * i.size - i.cc) - (i.cr - rr);
        float dc = length(max(qq, 0)) + min(max(qq.x, qq.y), 0) - rr;
        d = max(d, dc);
    } else if (i.ctype > 1.5 && i.ctype < 2.5) {
        /* circle: distance to center minus radius */
        float dc = length(i.uv * i.size - i.cc) - i.cr.x;
        d = max(d, dc);
    } else if (i.ctype > 2.5) {
        /* ellipse: normalized distance (approx SDF, good for AA) */
        float2 e = (i.uv * i.size - i.cc) / max(i.cr, 0.001);
        float dc = (length(e) - 1.0) * min(i.cr.x, i.cr.y);
        d = max(d, dc);
    }
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

/* blur sampling compute passes (box-shadow / backdrop-filter), the
 * mipmap-approximation of a gaussian: the source is mipmapped into
 * blur_tex, then each pixel re-samples (gaussian weights) with
 * explicit LOD (SampleLevel - compute rejects implicit-derivative Sample,
 * and SDL 3.4 D3D12 graphics-side SRV is broken anyway). Parameters come
 * from a StructuredBuffer: [0]={count,fb_w,fb_h,0}, then per record
 * {x,y,w,h}, {blur,r,g,b}, {a,...} (a = shadow alpha for kShadowCS,
 * backdrop body color+alpha for kBackdropCS).
 *
 * blur_sample is the shared two-tier approximation:
 *  - blur >= 8: the mip ladder - 3 taps across adjacent levels (a 2x2
 *    box-filtered level L has an equivalent radius of ~2^(L+1) px).
 *  - blur < 8: a fixed 3x3 binomial kernel on ONE mip level. The level is
 *    the octave floor(log2(blur)) (clamped to 2), so one texel there spans
 *    2^(L+1) px and the kernel's sigma (1 texel) matches the octave's top
 *    radius; inside the octave the kernel strength ramps from identity
 *    (t=0) to the full kernel (t=1) via log2(blur)-L, keeping the blur
 *    monotonic from ~1px up and seamless against the ladder at 8px.
 *    9 SampleLevel taps at a fixed level, no loop. */

static const std::string kBlurSampleFn = R"(
float4 blur_sample(Texture2D tex, SamplerState s, float2 uv, float blur,
                   float fb_w, float fb_h)
{
    if (blur >= 8.0) {
        float lod = clamp(log2(max(blur, 1.0)) - 1.0, 0.0, 6.0);
        float4 m = 0;
        float ws = 0;
        [unroll] for (int k = -1; k <= 1; ++k) {
            float w = exp(-0.5 * float(k * k));
            m += tex.SampleLevel(s, uv, max(lod + float(k), 0.0)) * w;
            ws += w;
        }
        return m / ws;
    }
    float lb = clamp(log2(max(blur, 1.0)), 0.0, 2.0);
    float lvl = floor(lb);
    float t = lb - lvl; /* 0..1 inside the octave */
    /* one-texel uv offset at that mip level (blur_tex is fb / 2) */
    float2 tx = exp2(lvl) * 2.0 / float2(fb_w, fb_h);
    float w_c = 1.0 - t * 0.75;  /* center: 1 -> 4/16 */
    float w_e = t * 0.125;       /* edge:   0 -> 2/16 */
    float w_x = t * 0.0625;      /* corner: 0 -> 1/16 */
    float4 m = tex.SampleLevel(s, uv, lvl) * w_c;
    m += (tex.SampleLevel(s, uv + float2(-tx.x, -tx.y), lvl) +
          tex.SampleLevel(s, uv + float2(tx.x, -tx.y), lvl) +
          tex.SampleLevel(s, uv + float2(-tx.x, tx.y), lvl) +
          tex.SampleLevel(s, uv + float2(tx.x, tx.y), lvl)) * w_x;
    m += (tex.SampleLevel(s, uv + float2(-tx.x, 0.0), lvl) +
          tex.SampleLevel(s, uv + float2(tx.x, 0.0), lvl) +
          tex.SampleLevel(s, uv + float2(0.0, -tx.y), lvl) +
          tex.SampleLevel(s, uv + float2(0.0, tx.y), lvl)) * w_e;
    return m; /* weights sum to 1 */
}
)";

static const std::string kShadowCS = std::string(R"(
[[vk::binding(0, 0)]] Texture2D blur_tex : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> params : register(t1);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_tex : register(u0, space1);
SamplerState samp : register(s0);
)") + kBlurSampleFn + R"(
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
            float4 m = blur_sample(blur_tex, samp, uv, c.x, hdr.y, hdr.z);
            acc = float4(c.yzw, d.x) * m.a;
            break;
        }
    }
    out_tex[id.xy] = acc;
}
)";

static const std::string kBackdropCS = std::string(R"(
[[vk::binding(0, 0)]] Texture2D blur_tex : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> params : register(t1);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_tex : register(u0, space1);
SamplerState samp : register(s0);
)") + kBlurSampleFn + R"(
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
            /* framebuffer px -> blur_tex uv. The backdrop source arrives by
             * a FULLSCREEN blit (geom_cur -> blur_tex), so a fb pixel maps
             * to uv = px/fb - NOT the *0.5 of the shadow shapes, whose
             * vertices are pre-divided by kBlurDiv before rasterization.
             * With the *0.5 the sampler read the top-left QUARTER of the
             * frame: a card near the top-left showed that area's blur (all
             * red on the probe page) instead of what sits under it. */
            float2 uv = px / float2(hdr.y, hdr.z);
            float4 m = blur_sample(blur_tex, samp, uv, c.x, hdr.y, hdr.z);
            /* blurred background + the element's own body color. Record
             * layout: {x,y,w,h} {blur,radius,0,0} {a,br,bg,bb} - for a
             * backdrop record a = body alpha, br/bg/bb = body color. The
             * body itself paints in the LATE pass (on top of this blur);
             * when used the blend here must be disabled (a = 0). */
            float ba = d.x;
            float3 blurred = m.rgb;
            float3 body = float3(d.y, d.z, d.w);
            float3 outcol = lerp(blurred, body, ba);
            float outa = m.a * (1.0 - ba) + ba;
            float4 outc = float4(outcol, outa);
            /* rounded-corner clip: outside the rounded box the backdrop
             * must not replace the painted pixel (the body's own SDF is
             * transparent there and the raw backdrop has to show through) */
            float rad = c.y;
            if (rad > 0.0) {
                float2 hf = r.zw * 0.5;
                float2 q = abs(px - (r.xy + hf)) - (hf - rad);
                float dd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
                outc = lerp(acc, outc, 1.0 - saturate(dd + 0.5));
            }
            acc = outc;
            break;
        }
    }
    out_tex[id.xy] = acc;
}
)";

/* inset box-shadow compute pass. The shadow source is the box SHRUNK by
 * spread, painted white into blur_tex (whaleui_gpu_inset); the shadow
 * alpha at a pixel is 1 - blurred_mask(px - offset): the rim outside the
 * shifted shrunk box has mask 0 -> full shadow, deep inside it saturates
 * to 1 -> no shadow. Shifting the SAMPLE point realizes the offset exactly
 * (positive ox lights the left edge, like CSS), which replaces the older
 * four-diagonal-triangles approximation wholesale - no per-quadrant
 * weights, no seams to blend. The mask itself bounds the shadow to ~blur
 * around the covering edge (the clean middle stays at zero), which stands
 * in for the "shrink by a blur radius" carve; the element's rounded
 * corners are the explicit clip below (layer 1 of the double clip).
 * Record layout (4 float4, note the *4 stride):
 *   {x,y,w,h} {blur,r,g,b} {a,ox,oy,spread} {radius,0,0,0}. */
static const std::string kInsetCS = std::string(R"(
[[vk::binding(0, 0)]] Texture2D blur_tex : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> params : register(t1);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_tex : register(u0, space1);
SamplerState samp : register(s0);
)") + kBlurSampleFn + R"(
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float2 px = float2(id.xy) + 0.5;
    float4 hdr = params[0];
    int n = int(hdr.x);
    float4 acc = out_tex[id.xy]; /* keep the painted background */
    for (int i = 0; i < n; ++i) {
        float4 r = params[1 + i * 4];
        float4 c = params[2 + i * 4];
        float4 d = params[3 + i * 4];
        float4 e = params[4 + i * 4];
        if (px.x >= r.x && px.x < r.x + r.z &&
            px.y >= r.y && px.y < r.y + r.w) {
            /* sample the shrunk box mask at the offset point; the mask
             * shapes are pushed at px / kBlurDiv, so a fb pixel maps to
             * uv = px/fb (see kShadowCS) */
            float2 uv = (px - float2(d.y, d.z)) / float2(hdr.y, hdr.z);
            float4 m = blur_sample(blur_tex, samp, uv, c.x, hdr.y, hdr.z);
            float sh = (1.0 - m.a) * d.x;
            /* rounded-corner clip: kill the shadow outside the element's
             * rounded box (blur 0 still leaves the crisp SDF-AA'd edge) */
            float rad = e.x;
            if (rad > 0.0) {
                float2 hf = r.zw * 0.5;
                float2 q = abs(px - (r.xy + hf)) - (hf - rad);
                float dd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
                sh *= 1.0 - saturate(dd + 0.5);
            }
            if (sh > 0.0) {
                acc = lerp(acc, float4(c.yzw, 1.0), sh);
            }
            break;
        }
    }
    out_tex[id.xy] = acc;
}
)";

/* filter (element) compute pass: blur + 4x5 color matrix over the
 * element's own painted content (geometry + text, i.e. AFTER the text
 * composite into target2). blur reuses the mip pyramid (target2 is blitted
 * into blur_tex first, like the backdrop pass); the matrix applies to the
 * blurred sample. Rounded-corner SDF clip like the backdrop pass.
 * Record layout (6 float4, note the *6 stride):
 *   {x,y,w,h} {blur,radius,0,0} {r0} {r1} {r2} {r3}
 * where each r row is [c0,c1,c2,bias]. */
static const std::string kFilterCS = std::string(R"(
[[vk::binding(0, 0)]] Texture2D blur_tex : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> params : register(t1);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_tex : register(u0, space1);
SamplerState samp : register(s0);
)") + kBlurSampleFn + R"(
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float2 px = float2(id.xy) + 0.5;
    float4 hdr = params[0];
    int n = int(hdr.x);
    float4 acc = out_tex[id.xy]; /* keep everything outside the regions */
    for (int i = 0; i < n; ++i) {
        float4 r = params[1 + i * 6];
        float4 c = params[2 + i * 6];
        float4 m0 = params[3 + i * 6];
        float4 m1 = params[4 + i * 6];
        float4 m2 = params[5 + i * 6];
        float4 m3 = params[6 + i * 6];
        if (px.x >= r.x && px.x < r.x + r.z &&
            px.y >= r.y && px.y < r.y + r.w) {
            float4 src;
            if (c.x > 0.0) {
                /* blurred self (fullscreen blit target2 -> blur_tex) */
                float2 uv = px / float2(hdr.y, hdr.z);
                src = blur_sample(blur_tex, samp, uv, c.x, hdr.y, hdr.z);
            } else {
                src = acc;
            }
            float3 rgb = src.rgb;
            float a = src.a;
            float3 outc;
            outc.r = dot(rgb, m0.xyz) + m0.w * a;
            outc.g = dot(rgb, m1.xyz) + m1.w * a;
            outc.b = dot(rgb, m2.xyz) + m2.w * a;
            float outa = dot(float4(rgb, a), m3.xyzw);
            float4 outv = float4(outc, outa);
            float rad = c.y;
            if (rad > 0.0) {
                float2 hf = r.zw * 0.5;
                float2 q = abs(px - (r.xy + hf)) - (hf - rad);
                float dd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
                outv = lerp(acc, outv, 1.0 - saturate(dd + 0.5));
            }
            acc = outv;
            break;
        }
    }
    out_tex[id.xy] = acc;
}
)";

/* mix-blend-mode compute pass: blend the element's own body color /
 * gradient (src, from the record) over the painted backdrop (dst, from
 * the composited frame) with the separable blend formulas + Porter-Duff.
 * Like the backdrop pass this runs on the composited frame (target2); the
 * src is the body's own color (a solid/gradient approximation - subtree
 * content does not participate, documented). The backdrop is read from
 * the same SIMULTANEOUS texture via out_tex Load (no second binding, so
 * no sampler slot is needed). Record layout (3 float4):
 *   {x,y,w,h} {mode,radius,0,0} {sr,sg,sb,sa}
 * mode = blend_channel id (0..11; 12..15 luminance-mix approx). */
static const std::string kMixBlendCS = std::string(R"(
[[vk::binding(1, 0)]] StructuredBuffer<float4> params : register(t1);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_tex : register(u0, space1);
SamplerState samp : register(s0); /* unused (Load-only); keeps the
    pipeline signature identical to the other blur passes */
float blend_ch(int mode, float cb, float cs)
{
    if (mode <= 0) { return cs; }
    if (mode == 1) { return cb * cs; }
    if (mode == 2) { return 1.0 - (1.0 - cb) * (1.0 - cs); }
    if (mode == 3) {
        return cb < 0.5 ? 2.0 * cb * cs
                        : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
    }
    if (mode == 4) { return min(cb, cs); }
    if (mode == 5) { return max(cb, cs); }
    if (mode == 6) {
        if (cb <= 0.0) { return 0.0; }
        if (cs >= 1.0) { return 1.0; }
        return min(cb / (1.0 - cs), 1.0);
    }
    if (mode == 7) {
        if (cb >= 1.0) { return 1.0; }
        if (cs <= 0.0) { return 0.0; }
        return max(1.0 - (1.0 - cb) / cs, 0.0);
    }
    if (mode == 8) {
        return cs < 0.5 ? 2.0 * cb * cs
                        : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
    }
    if (mode == 9) {
        float d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb
                             : sqrt(cb);
        return cs <= 0.5 ? cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb)
                         : cb + (2.0 * cs - 1.0) * (d - cb);
    }
    if (mode == 10) { return abs(cb - cs); }
    if (mode == 11) { return cb + cs - 2.0 * cb * cs; }
    return cs; /* 12..15 never reach here (handled in main) */
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float2 px = float2(id.xy) + 0.5;
    float4 hdr = params[0];
    int n = int(hdr.x);
    float4 acc = out_tex[id.xy];
    for (int i = 0; i < n; ++i) {
        float4 r = params[1 + i * 3];
        float4 c = params[2 + i * 3];
        float4 s = params[3 + i * 3];
        if (px.x >= r.x && px.x < r.x + r.z &&
            px.y >= r.y && px.y < r.y + r.w) {
            float4 dst = acc;
            int mode = int(c.x + 0.5);
            float sa = s.a;
            float da = dst.a;
            float oa = sa + da * (1.0 - sa);
            float3 outc = dst.rgb;
            float sr = s.r, sg = s.g, sb = s.b;
            if (mode >= 12) {
                float lum_s = 0.2126 * sr + 0.7152 * sg + 0.0722 * sb;
                float lum_d = 0.2126 * dst.r + 0.7152 * dst.g + 0.0722 * dst.b;
                if (mode == 15) {
                    float k = lum_d > 0.0 ? lum_s / lum_d : 1.0;
                    outc = dst.rgb * k;
                } else {
                    outc = (dst.rgb + float3(sr, sg, sb)) * 0.5;
                    if (mode == 13) {
                        float m = (lum_s + lum_d) * 0.5 - lum_d;
                        outc += m;
                    }
                }
            } else if (mode > 0) {
                outc.r = blend_ch(mode, dst.r, sr);
                outc.g = blend_ch(mode, dst.g, sg);
                outc.b = blend_ch(mode, dst.b, sb);
            } else {
                outc = float3(sr, sg, sb);
            }
            float3 fin = (outc * sa + dst.rgb * da * (1.0 - sa)) /
                         max(oa, 1e-5);
            float4 outv = float4(fin, oa);
            float rad = c.y;
            if (rad > 0.0) {
                float2 hf = r.zw * 0.5;
                float2 q = abs(px - (r.xy + hf)) - (hf - rad);
                float dd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
                outv = lerp(acc, outv, 1.0 - saturate(dd + 0.5));
            }
            acc = outv;
            break;
        }
    }
    out_tex[id.xy] = acc;
}
)");

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
cbuffer Params : register(b0, space2) { uint4 offset; };
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy + offset.xy);
    float4 t = text_layer.Load(int3(p, 0));
    /* text_layer is R8G8B8A8 (the CPU upload swaps the 0xAARRGGBB bytes
     * into that order), same channel order as the geometry target - no
     * swap here: the format name already fixes the decode on both
     * backends and a second swap reversed red/blue on D3D12 */
    float4 g = geometry.Load(int3(p, 0));
    /* text_layer holds straight (non-premultiplied) color + real alpha:
     * glyph edges must blend over the geometry, not replace it */
    out_tex[p] = float4(t.rgb * t.a + g.rgb * (1.0 - t.a), 1.0);
}
)";

#endif /* WHALEUI_RENDER_GPU_SHADERS_H */
