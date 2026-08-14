/* FSR 1.0 RCAS - contrast-adaptive sharpening (HLSL, D3D12 DXIL) */
Texture2D<float4> uInput : register(t0);
RWTexture2D<float4> uOutput : register(u0, space1);
cbuffer PC : register(b0, space2) { float4 cparams; } /* sharpness, pad0, pad1, pad2 */

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 imgSize;
    uInput.GetDimensions(imgSize.x, imgSize.y);
    if (dtid.x >= imgSize.x || dtid.y >= imgSize.y) return;

    /* Read 3x3 neighbourhood */
    float4 p[9];
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            uint2 tc = uint2(clamp((int)dtid.x + i, 0, (int)imgSize.x - 1),
                             clamp((int)dtid.y + j, 0, (int)imgSize.y - 1));
            p[(j + 1) * 3 + (i + 1)] = uInput[tc];
        }
    }

    float4 centre = p[4];
    float scale = cparams.x * 2.0;
    float4 outC = centre;

    for (int c = 0; c < 3; ++c) {
        float n0 = p[1][c], n1 = p[3][c], n2 = p[5][c], n3 = p[7][c];
        float mn = min(min(n0, n1), min(n2, n3));
        float mx = max(max(n0, n1), max(n2, n3));
        float range = mx - mn;
        float cv = centre[c];
        if (range > 1e-6 && cv >= mn && cv <= mx) {
            float amount = 2.0 * (1.0 - max(range, 0.5));
            amount = clamp(amount * scale, 0.0, scale);
            outC[c] = cv + (cv - (n0 + n1 + n2 + n3) * 0.25) * amount;
        }
    }
    outC.a = centre.a;
    uOutput[dtid.xy] = clamp(outC, 0.0, 1.0);
}
