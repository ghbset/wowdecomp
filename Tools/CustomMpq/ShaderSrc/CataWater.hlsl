// CataWater (vs_3_0): Cataclysm 4.3.4's ProcWater vertex shader on the 3.3.5a water draw (waterStyle 1,
// docs/BACKLOG_PORT.md "Cata water"). Ported from wxl-water src/Shaders.hpp kVertex (@ 17cf9af) minus the far ocean
// instancing and the land-shadow lookup; Cata decoded in /root/wxl-wowforever/water-research-W1-cata.md section 7.
// Replaces vsLiquidWater / vsLiquidWaterNoSpec. The stock registers the material uploads are read as they are
// (Liquid::VSConstants): c0..c3 projection, c4 fog, c5..c8 modelview, c13/c14/c16 the ramp texture matrix,
// c33 sun direction in view space, c34 ambient, c35 diffuse. CataWater.cpp adds:
//   c50..c52  inverse view rotation rows (view space -> camera-relative world)
//   c53       (camera x, camera y wrapped, time in seconds, tc0 scale 0.06)
//   c54       (P3, P4, P5 in radians, P8)
float4 gP0 : register(c0);  float4 gP1 : register(c1);  float4 gP2 : register(c2);  float4 gP3 : register(c3);
float4 gFog : register(c4);
float4 gM0 : register(c5);  float4 gM1 : register(c6);  float4 gM2 : register(c7);  float4 gM3 : register(c8);
float4 gU0 : register(c13); float4 gU1 : register(c14); float4 gU2 : register(c16);
float4 gLight : register(c33); float4 gAmbient : register(c34); float4 gDiffuse : register(c35);
float4 gInv0 : register(c50); float4 gInv1 : register(c51); float4 gInv2 : register(c52);
float4 gCam  : register(c53);
float4 gTex  : register(c54);

struct VsIn  { float4 pos : POSITION; float3 nrm : NORMAL; float4 col : COLOR0; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };
struct VsOut
{
    float4 pos    : POSITION;
    float4 col    : COLOR0;      // lit vertex colour (Cata v8)
    float4 coarse : TEXCOORD0;   // normal map, coarse pair (Cata TC0)
    float4 fine   : TEXCOORD1;   // normal map, fine pair (Cata TC1)
    float4 ramp   : TEXCOORD2;   // xy = depth-colour ramp uv (Cata TC3), z = fog factor, w = clip w (view depth)
    float4 world  : TEXCOORD3;   // camera-relative world position (Cata TC4)
    float4 nrm    : TEXCOORD4;   // world vertex normal (Cata TC5)
    float4 clip   : TEXCOORD5;   // xy = clip xy (the screen position; the Vulkan translator has no VPOS)
    float4 foam   : TEXCOORD7;   // foam/chop uv pair (Cata TC2)
};

float2 Rot(float2 a, float r) { float c = cos(r), s = sin(r); return float2(a.x * c - a.y * s, a.x * s + a.y * c); }

VsOut main(VsIn i)
{
    VsOut o;
    float4 v = i.pos.x * gM0 + i.pos.y * gM1 + i.pos.z * gM2 + gM3;
    o.pos = v.x * gP0 + v.y * gP1 + v.z * gP2 + v.w * gP3;

    float3 w = v.x * gInv0.xyz + v.y * gInv1.xyz + v.z * gInv2.xyz;
    o.world = float4(w, 0.0);
    float3 n = i.nrm.x * gM0.xyz + i.nrm.y * gM1.xyz + i.nrm.z * gM2.xyz;
    o.nrm = float4(n.x * gInv0.xyz + n.y * gInv1.xyz + n.z * gInv2.xyz, 0.0);

    // Cata tiles the normal map from tc0 / 32 with tc0 = world xy * 0.06 (Cata 0x00771DC0, 3.3.5a 0x007CE390),
    // rebuilt from camera-relative world + camera. P3 / P4 scale the two coarse layers, P5 turns them, the fine
    // pair repeats P8 * 32 times per coarse tile, and the set scrolls every 66.7 s.
    float2 tc  = (w.xy + gCam.xy) * gCam.w;
    float2 uvA = Rot(tc / 32.0 * gTex.x, gTex.z);
    float2 uvB = Rot(-(tc + 0.5) / 32.0 * gTex.y, gTex.z);
    float  scroll = frac(gCam.z * 0.015);
    float  fineScale = 32.0 * gTex.w;
    o.coarse = float4(uvA.y, uvA.x + scroll, uvB.y, uvB.x + scroll);
    o.fine   = float4(uvA.y * fineScale, uvA.x * fineScale + scroll, uvB.y * fineScale, uvB.x * fineScale + scroll);

    // Cata TC2: the coarse uv at 228x, wobbling (5 s in x, 10 s in y, 2 pi yd in space) instead of scrolling.
    float2 ph = frac((3.14159265 * frac(gCam.z * 0.1) * float2(4.0, 2.0) + tc / gCam.w) * 0.15915494 + 0.5)
              * 6.28318531 - 3.14159265;
    float2 sc = float2(sin(ph.x), cos(ph.y));
    o.foam = float4(228.0 * uvA + 0.1 * sc, 228.0 * uvA + 0.05 * sc + 0.5);

    o.ramp.xy = i.uv1.x * gU0.xy + i.uv1.y * gU1.xy + gU2.xy;
    o.ramp.z  = min(pow(max(v.z * gFog.x + gFog.y, 0.0), gFog.z), 1.0);   // the stock fog, exponent included
    o.ramp.w  = o.pos.w;
    o.clip    = float4(o.pos.xy, 0.0, 0.0);

    float lit = saturate(dot(-gLight.xyz, n));
    o.col = float4(gDiffuse.rgb * lit + gAmbient.rgb, 1.0) * i.col;
    return o;
}
