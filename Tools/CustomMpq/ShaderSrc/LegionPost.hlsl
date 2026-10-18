// LegionPost{0,1,2} (ps_3_0, full res over the world viewport, before names and the UI): Legion 7.3.5's FFX
// anti-aliasing (ffxAntiAliasingMode) and FFXColorShift (colorblindSimulator) in one pass. Our own HLSL.
// FXAA_STEPS 0 = colour shift only; otherwise FXAA 3.11 "quality" (Timothy Lottes' public algorithm) with that many
// edge search steps (5 = low, 12 = high), then the colour shift.
// s0 a bilinear copy of the finished world viewport. c0 world viewport px (zw = size), c10 = FXAA subpix, edge
// threshold, edge threshold min; c11-c13 the colour matrix rows, applied in linear light (identity = off).
// Draw opaque, colour write RGB, no depth. Flat code (no dynamic branches) so every translator takes it.
#include "vf_common.hlsli"

float4 cFxaa : register(c10);
float4 cColourShift[3] : register(c11);

sampler2D sScene : register(s0);

float3 Tap(float2 uv)
{
    return tex2Dlod(sScene, float4(uv, 0, 0)).rgb;
}

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

float LumaAt(float2 uv)
{
    return Luma(Tap(uv));
}

#if FXAA_STEPS == 5
static const float kStep[5] = { 1.0, 1.5, 2.0, 4.0, 12.0 };                                     // FXAA_QUALITY__PRESET 12
#elif FXAA_STEPS == 12
static const float kStep[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };     // FXAA_QUALITY__PRESET 39
#endif

float3 Fxaa(float2 uv)
{
    float3 rgbM = Tap(uv);
#if FXAA_STEPS
    float2 texel = 1.0 / ViewportSize();
    float lumaM = Luma(rgbM);
    float lumaS = LumaAt(uv + float2(0, 1) * texel);
    float lumaE = LumaAt(uv + float2(1, 0) * texel);
    float lumaN = LumaAt(uv + float2(0, -1) * texel);
    float lumaW = LumaAt(uv + float2(-1, 0) * texel);

    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    bool earlyExit = range < max(cFxaa.z, rangeMax * cFxaa.y);

    float lumaNW = LumaAt(uv + float2(-1, -1) * texel);
    float lumaSE = LumaAt(uv + float2(1, 1) * texel);
    float lumaNE = LumaAt(uv + float2(1, -1) * texel);
    float lumaSW = LumaAt(uv + float2(-1, 1) * texel);

    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz = abs(-2.0 * lumaW + lumaNWSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 + abs(-2.0 * lumaE + lumaNESE);
    float edgeVert = abs(-2.0 * lumaS + lumaSWSE) + abs(-2.0 * lumaM + lumaWE) * 2.0 + abs(-2.0 * lumaN + lumaNWNE);
    bool horzSpan = edgeHorz >= edgeVert;

    // The pair across the edge (N/S for a horizontal edge, W/E for a vertical one) and the side with the steeper step.
    float lumaA = horzSpan ? lumaN : lumaW;
    float lumaB = horzSpan ? lumaS : lumaE;
    float lengthSign = horzSpan ? texel.y : texel.x;
    float gradientA = lumaA - lumaM;
    float gradientB = lumaB - lumaM;
    bool pairA = abs(gradientA) >= abs(gradientB);
    float gradient = max(abs(gradientA), abs(gradientB));
    lengthSign = pairA ? -lengthSign : lengthSign;
    float lumaPair = (pairA ? lumaA : lumaB) + lumaM;

    float subpixA = (lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE;
    float subpixC = saturate(abs(subpixA / 12.0 - lumaM) / max(range, 1e-5));
    float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;

    // Walk both ways along the edge from half a texel across it until the luma leaves the pair's mean.
    float2 posB = uv + (horzSpan ? float2(0, lengthSign * 0.5) : float2(lengthSign * 0.5, 0));
    float2 offNP = horzSpan ? float2(texel.x, 0) : float2(0, texel.y);
    float gradientScaled = gradient * 0.25;
    float lumaHalf = lumaPair * 0.5;
    bool lumaMLTZero = lumaM - lumaHalf < 0.0;
    float2 posN = posB - offNP * kStep[0];
    float2 posP = posB + offNP * kStep[0];
    float lumaEndN = LumaAt(posN) - lumaHalf;
    float lumaEndP = LumaAt(posP) - lumaHalf;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    [unroll] for (int i = 1; i < FXAA_STEPS; i++)
    {
        posN = doneN ? posN : posN - offNP * kStep[i];
        posP = doneP ? posP : posP + offNP * kStep[i];
        float endN = LumaAt(posN) - lumaHalf;
        float endP = LumaAt(posP) - lumaHalf;
        lumaEndN = doneN ? lumaEndN : endN;
        lumaEndP = doneP ? lumaEndP : endP;
        doneN = doneN || abs(lumaEndN) >= gradientScaled;
        doneP = doneP || abs(lumaEndP) >= gradientScaled;
    }

    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    bool directionN = dstN < dstP;
    bool goodSpan = directionN ? ((lumaEndN < 0.0) != lumaMLTZero) : ((lumaEndP < 0.0) != lumaMLTZero);
    float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / max(dstN + dstP, 1e-6) : 0.0;
    float offset = max(pixelOffset, subpixF * subpixF * cFxaa.x);
    float2 uvAa = uv + (horzSpan ? float2(0, offset * lengthSign) : float2(offset * lengthSign, 0));
    rgbM = earlyExit ? rgbM : Tap(uvAa);
#endif
    return rgbM;
}

float4 main(float2 viewportUv : TEXCOORD0) : COLOR0
{
    float3 rgb = Fxaa(viewportUv);
    float3 lin = pow(saturate(rgb), 2.2);
    lin = float3(dot(cColourShift[0].rgb, lin), dot(cColourShift[1].rgb, lin), dot(cColourShift[2].rgb, lin));
    return float4(pow(saturate(lin), 1.0 / 2.2), 1);
}
