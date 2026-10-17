// FarFoliage (vs_3_0): the far tree and bush cards (WoW/Source/WorldClient/FarFoliage.cpp, docs/FARFIELD_FOG.md).
// One quad a placement in a static per-block buffer (PNCT); the card turns to face the camera here, so the
// buffer never changes with the camera. The registers the terrain uses keep their meaning (c0-c7, fog c12,
// sun c24-c26: FarField::TerrainConstants), so the card fogs and lights exactly as the far ground does.
//   position = the card's bottom centre (block coords)
//   normal   = x: corner side (sign) x (1 + model turn in turns), y: corner height (0 / 1) + 2 for a 128 px
//              picture, z: card side (yd)
//   color    = ground tint (MCCV, 0.5 = none) rgb, a = 1 lit / 0 in the ground's baked shadow
//   texcoord = the model's atlas row (8 views, left to right)
row_major float4x4 cWorldView : register(c0);
row_major float4x4 cProj      : register(c4);
float4 cCam   : register(c8);   // xyz: the camera in block coords
float4 cFade  : register(c9);   // x, y: fade = saturate(viewZ * x + y); z: view depth out to which the model
                                // may still be drawn (the card stands behind it until there); w: 1 / ease band
float4 cCard  : register(c10);  // x, y: a view's width in uv (64 px, 128 px pictures)
float4 cFog   : register(c12);
float4 cSun   : register(c24);  // view space, toward the sun

struct VsOut {
    float4 pos  : POSITION;
    float4 tint : COLOR0;
    float4 sun  : COLOR1;      // the sun in the card's frame (right, up, toward camera) x 0.5 + 0.5
    float4 uv   : TEXCOORD0;   // the two nearest views' uv
    float4 aux  : TEXCOORD1;   // xy: dissolve cell coords (card-anchored), z: mix to the second view, w: fade
    float  fog  : FOG;
};

// atan2 in turns, (-0.5, 0.5]; max error ~1e-5 turn.
float Turns(float y, float x)
{
    float ax = abs(x), ay = abs(y);
    float t = min(ax, ay) / max(max(ax, ay), 1e-6);
    float s = t * t;
    float r = ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * t + t;
    r = ay > ax ? 1.5707963 - r : r;
    r = x < 0.0 ? 3.1415927 - r : r;
    r = y < 0.0 ? -r : r;
    return r * 0.15915494;
}

VsOut main(float3 base : POSITION, float3 n : NORMAL, float4 col : COLOR0, float2 slot : TEXCOORD0)
{
    VsOut o;
    float sx    = n.x < 0.0 ? -0.5 : 0.5;
    float turn  = abs(n.x) - 1.0;
    float big   = n.y > 1.5 ? 1.0 : 0.0;
    float sy    = n.y - 2.0 * big;
    float side  = n.z;
    float vw    = big > 0.5 ? cCard.y : cCard.x;

    float3 toCam = cCam.xyz - base;
    float2 flat  = normalize(toCam.xy + float2(1e-3, 0.0));
    float3 right = float3(-flat.y, flat.x, 0.0);
    float3 p     = base + right * (sx * side) + float3(0.0, 0.0, sy * side);

    // The two views nearest the camera's bearing round the tree (view k: the model's +x turned k x 45 deg) and
    // how far between them: the same for all four corners (from the card's base), mixed per pixel.
    float rel = frac(Turns(flat.y, flat.x) - turn) * 8.0;
    float k0  = floor(rel);
    float f   = rel - k0;
    float k1  = k0 > 6.5 ? 0.0 : k0 + 1.0;
    float u   = (sx + 0.5) * vw;
    float v   = slot.y + (1.0 - sy) * vw;
    o.uv = float4(slot.x + k0 * vw + u, v, slot.x + k1 * vw + u, v);

    // While the model may still be drawn the card stands behind it along the view ray, scaled about the camera
    // so it covers the same pixels: the tree hides it, it fills the tree's holes as the tree fades.
    float zBase = mul(float4(base, 1.0), cWorldView).z;
    float dist  = length(toCam);
    float back  = 0.5 * side * saturate((cFade.z - zBase) * cFade.w);
    p = cCam.xyz + (p - cCam.xyz) * (1.0 + back / max(dist, 1.0));

    float4 view = mul(float4(p, 1.0), cWorldView);
    o.pos = mul(view, cProj);
    o.fog = min(pow(max(view.z * cFog.x + cFog.y, 0.0), cFog.z), 1.0);

    // Sun into the card's frame: world = rows of the view rotation dotted with the view-space vector.
    float3 sunW = mul((float3x3)cWorldView, cSun.xyz);
    float3 sunC = float3(dot(sunW, right), sunW.z, dot(sunW.xy, flat));
    o.sun  = float4(sunC * 0.5 + 0.5, 1.0);
    o.tint = col;
    float seed = floor(frac(dot(base.xy, float2(0.1373, 0.2719))) * 97.0);
    o.aux = float4((sx + 0.5) * 16.0 + seed * 16.0, (1.0 - sy) * 16.0, f, saturate(zBase * cFade.x + cFade.y));
    return o;
}
