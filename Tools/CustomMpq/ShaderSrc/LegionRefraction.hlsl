// LegionRefraction (ps_3_0; Legion survey 2.6, refraction): heat haze over one tagged fire emitter, one draw per source
// scissored to its screen rect, opaque over the world after the fog composite and before the blended passes. Our own
// shader; Legion's refraction / refractionapply were read for their role only (offsets from a distortion source,
// applied to a scene copy, depth tested).
// The scene copy is read at an offset: rising two-axis sine noise in the source's own yards, faded to the rect's
// edge, kept off pixels in front of the source and off taps that land in front of it (no foreground smear).
// TEXCOORD0 = world-viewport uv. s0 scene depth, s1 scene copy (the world viewport).
// c10 rect px (x0, y0, x1, y1), c11 (source near depth yd, offset px, seconds, high), c12 (rect size yd: x, y, 0, 0).
#include "vf_common.hlsli"

float4 cRefractRect : register(c10);
float4 cRefractHaze : register(c11);
float4 cRefractSize : register(c12);

sampler2D sDepth : register(s0);
sampler2D sScene : register(s1);

float4 main(float2 viewportUv : TEXCOORD0) : COLOR0
{
    float2 pixel = FullPixelFromUv(viewportUv);
    float2 local = saturate((pixel - cRefractRect.xy) / max(cRefractRect.zw - cRefractRect.xy, 1));   // 0 top left
    float2 e = local * 2 - 1;
    float mask = saturate(1 - dot(e, e));
    mask *= mask;
    float near = cRefractHaze.x;
    mask *= saturate(LinearDepth(SampleDepth(sDepth, pixel)) - near + 1);     // in front of the source: untouched

    // ~3 cycles per yard; +t on the downward local y moves the pattern up the screen (heat rises).
    float t = cRefractHaze.z;
    float2 q = local * cRefractSize.xy * 3;
    q.y += t * 2.5;
    float2 wobble = float2(sin(q.y * 2.1 + sin(q.x * 1.7 + t) * 1.5), cos(q.x * 2.3 + q.y * 0.9 + t * 1.3) * 0.6);
    wobble += cRefractHaze.w * 0.5 * float2(sin(q.y * 4.7 + q.x * 1.3 - t * 1.9), cos(q.x * 5.1 - q.y * 2.2 + t * 2.7));
    float2 offset = wobble * (cRefractHaze.y * mask);
    float2 tap = clamp(pixel + offset, ViewportOrigin() + 0.5, ViewportLastPixelCentre());
    offset = LinearDepth(SampleDepth(sDepth, tap)) < near - 0.5 ? 0 : tap - pixel;
    return float4(tex2Dlod(sScene, float4((pixel + offset - ViewportOrigin()) / ViewportSize(), 0, 0)).rgb, 1);
}
