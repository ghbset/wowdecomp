// LegionLights (ps_3_0, full res over the world viewport; Legion survey 2.8 + backlog #7, lightMode): the frame's
// M2 point lights (lamps, torches, braziers) on the surfaces around them, resolved from the scene depth in one
// screen-space pass and laid over the frame as colour += colour * light (GxBlend_ModAdd: dst * src + dst), so the
// frame's own colour is the albedo proxy: lit surfaces brighten in the lamp's colour and black stays black.
// Our own shader. Falloff after northlight's local_light_effects.hlsl (MIT, /root/wotlk-renderers/northlight):
// the light's own attenuation times a smooth window that reaches 0 at its radius; normals rebuilt from the depth as
// VFogShadowMask does. docs/LEGION_PORT.md "Legion lights".
// TEXCOORD0 = world-viewport uv. s0 scene depth. c0-c3 vf_common (c3.z = lightMaxDist), c10 (strength, 1 / fade
// span, fade start, 0), then 3 registers a light from c11: view position + 1 / radius^2, colour, attenuation c/l/q.
#include "vf_common.hlsli"

static const int kLights = 16;
float4 cLightControl : register(c10);
float4 cLights[kLights * 3] : register(c11);

sampler2D sDepth : register(s0);

float3 ViewAt(float2 pixel)
{
    return ViewRayAtUnitDepth(pixel) * LinearDepth(SampleDepth(sDepth, pixel));
}

float3 Nearer(float3 p, float3 a, float3 b)
{
    return abs(a.z - p.z) < abs(b.z - p.z) ? a - p : p - b;
}

float4 main(float2 viewportUv : TEXCOORD0) : COLOR0
{
    float2 pixel = FullPixelFromUv(viewportUv);
    float depth = SampleDepth(sDepth, pixel);
    if (depth > DeepestWorldDepth())    // sky and the far WDL horizon
        discard;
    float3 p = ViewRayAtUnitDepth(pixel) * LinearDepth(depth);
    float fade = saturate((cLightControl.z - length(p)) * cLightControl.y);
    if (fade <= 0)
        discard;

    float3 dx = Nearer(p, ViewAt(pixel + float2(1, 0)), ViewAt(pixel - float2(1, 0)));
    float3 dy = Nearer(p, ViewAt(pixel + float2(0, 1)), ViewAt(pixel - float2(0, 1)));
    float3 n = cross(dx, dy);
    n *= dot(n, p) > 0 ? -1 : 1;    // toward the camera
    n = normalize(n);

    // Literal register indices only (no relative addressing for the translators); unused slots carry colour 0.
    float3 light = 0;
    [unroll] for (int i = 0; i < kLights; ++i)
    {
        float3 v = cLights[i * 3].xyz - p;
        float d2 = dot(v, v);
        float d = sqrt(d2);
        float3 att = cLights[i * 3 + 2].xyz;
        float window = saturate(1 - d2 * cLights[i * 3].w);
        float falloff = min(1 / max(att.x + att.y * d + att.z * d2, 1e-3), 1.5) * window * window;
        float ndl = saturate(dot(n, v) / max(d, 1e-3));
        light += cLights[i * 3 + 1].rgb * (falloff * ndl);
    }
    light *= cLightControl.x * fade;
    if (max(light.r, max(light.g, light.b)) < 1.0 / 512)
        discard;
    return float4(saturate(light), 1);
}
