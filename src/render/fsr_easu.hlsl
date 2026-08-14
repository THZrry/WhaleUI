/* FSR 1.0 EASU - Edge Adaptive Spatial Upsampling (HLSL, D3D12 DXIL) */
Texture2D<float4> uInput : register(t0);
RWTexture2D<float4> uOutput : register(u0, space1);
cbuffer PC : register(b0, space2) { float4 cparams; } /* inW, inH, scaleX, scaleY */

static float lanczos2(float x)
{
    x = abs(x);
    if (x < 1e-6) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265359 * x;
    return (sin(px) * sin(px * 0.5)) / (px * px * 0.5);
}

static float lum(float4 c) { return dot(c.rgb, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 oSz, iSz;
    uOutput.GetDimensions(oSz.x, oSz.y);
    uInput.GetDimensions(iSz.x, iSz.y);
    if (dtid.x >= oSz.x || dtid.y >= oSz.y) return;

    float inW = cparams.x, inH = cparams.y;
    float scaleX = cparams.z, scaleY = cparams.w;
    (void)inW; (void)inH;
    float px = (float(dtid.x) + 0.5) * scaleX - 0.5;
    float py = (float(dtid.y) + 0.5) * scaleY - 0.5;
    int bx = (int)floor(px), by = (int)floor(py);

    /* 4x4 neighbourhood with clamp */
    float4 p[16];
    for (int j = -1; j <= 2; ++j) {
        for (int i = -1; i <= 2; ++i) {
            uint2 tc = uint2(clamp(bx + i, 0, (int)iSz.x - 1),
                             clamp(by + j, 0, (int)iSz.y - 1));
            p[(j + 1) * 4 + (i + 1)] = uInput[tc];
        }
    }

    /* 3x3 Sobel luminance gradients */
    float l[9];
    for (int dj = 0; dj < 3; ++dj) {
        for (int di = 0; di < 3; ++di) {
            l[dj * 3 + di] = lum(p[(dj + 1) * 4 + (di + 1)]);
        }
    }
    float gx = (l[2] - l[0]) + 2.0 * (l[5] - l[3]) + (l[8] - l[6]);
    float gy = (l[0] - l[6]) + 2.0 * (l[1] - l[7]) + (l[2] - l[8]);
    float edgeLen = length(float2(gx, gy)) + 1e-6;
    float ex = gx / edgeLen, ey = gy / edgeLen;
    float edgeStr = clamp(edgeLen * 0.25, 0.0, 1.0);

    /* Anisotropic Lanczos2 weighting */
    float sumW = 0.0;
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    for (int j = -1; j <= 2; ++j) {
        for (int i = -1; i <= 2; ++i) {
            int idx = (j + 1) * 4 + (i + 1);
            float dx = float(bx + i) + 0.5 - px;
            float dy = float(by + j) + 0.5 - py;
            float dPara = dx * ex + dy * ey;
            float dPerp = dx * (-ey) + dy * ex;
            float stretch = 1.0 + 1.5 * edgeStr;
            float dist = sqrt(dPara * dPara + (dPerp * stretch) * (dPerp * stretch));
            float w = lanczos2(dist);
            sumW += w;
            acc += w * p[idx];
        }
    }
    uOutput[dtid.xy] = (sumW > 1e-6) ? (acc / sumW) : float4(0.0, 0.0, 0.0, 0.0);
}
