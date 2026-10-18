// DetailDoodad (vs_3_0): the stock Shaders\Vertex\vs_3_0\DetailDoodad.bls, all 6 permutations, rewritten in HLSL
// with wxl-grasswind's motion block (GrassWind.cpp kMotionInject): wind sway and the grass parting around the player.
// Permutation = 3 * LIGHTS + SHADOW (CDetailDoodad::SetupVertexShader). LIGHTS 1 adds 3 point lights (c14-c22);
// SHADOW 1/2 writes the view position, view normal and 1 or 3 shadow-map coordinates (c23-c34, CShadowCache).
// Constants as CDetailDoodad::SShaderConstants: c0-c3 worldView (rows), c4-c7 projection, c8 fog, c9 fade,
// c10 sun dir (view), c11 ambient, c12 diffuse. Grass motion: c35-c42 (gw[], DetailDoodad.cpp GrassWindConstants);
// all zero = the stock blade position. Same math, register for register, as the plugin's injection (c14-c21 there).

float4 wv[4]    : register(c0);
float4 proj[4]  : register(c4);
float4 fogP     : register(c8);
float4 fade     : register(c9);
float4 sunDir   : register(c10);
float4 sunAmb   : register(c11);
float4 sunDiff  : register(c12);
float4 pl[9]    : register(c14);   // per light: position (world-relative, rotated here), colour, attenuation
float4 sh[12]   : register(c23);
float4 gw[8]    : register(c35);

struct VsOut {
    float4 pos : POSITION0;
    float4 col : COLOR0;
    float4 spec : COLOR1;
    float2 uv : TEXCOORD0;
#if SHADOW
    float3 viewPos : TEXCOORD1;
    float3 viewNrm : TEXCOORD2;
#if SHADOW == 1
    float3 sh0 : TEXCOORD3;
#else
    float4 sh0 : TEXCOORD3;
    float4 sh1 : TEXCOORD4;
    float4 sh2 : TEXCOORD5;
#endif
#endif
    float fog : FOG;
};

float3 Rot(float3 v)
{
    return v.x * wv[0].xyz + v.y * wv[1].xyz + v.z * wv[2].xyz;
}

VsOut main(float4 v : POSITION0, float3 n : NORMAL0, float4 c : COLOR0, float2 uv : TEXCOORD0)
{
    VsOut o;
    float4 p = v.x * wv[0] + v.y * wv[1] + v.z * wv[2] + wv[3];

    // grass motion (wxl-grasswind): view -> camera-relative world, offset in the world plane, back to view
    float3 rel = float3(dot(wv[0].xyz, p.xyz), dot(wv[1].xyz, p.xyz), dot(wv[2].xyz, p.xyz));
    float2 w = rel.xy + gw[0].xy;                                   // world xy
    float hw = saturate((rel.z + gw[0].z - gw[2].z - gw[1].z) * gw[1].x);  // height cone above the player's feet
    float2 d = w - gw[2].xy;
    float dd = dot(d, d) + gw[1].w;
    float inv = rsqrt(dd);
    float dist = dd * inv;
    float f = (saturate(dist * gw[1].y) * gw[3].x + gw[3].y) * (dist < gw[3].z ? 1.0 : 0.0) * hw * inv;
    float2 off = d * f;                                             // parting around the player
    float bend = saturate((1.0 - uv.y + gw[7].z) * gw[7].w);        // 0 below the anchor, 1 at the tip
    bend *= bend;
    bend /= 1.0 + p.z * gw[6].w;                                    // view-distance fade
    float j = frac(w.x * gw[6].x + w.y * gw[6].y);                  // per-blade hash
    bend *= j * gw[7].y + gw[7].x;                                  // per-blade amplitude variance
    j *= gw[6].z;                                                   // per-blade phase jitter
    float s1 = sin(w.x * gw[4].z + w.y * gw[4].w + j + gw[3].w);
    float s2 = sin(w.x * gw[5].z + w.y * gw[5].w + j + gw[0].w);
    off += (gw[4].xy * (s1 + gw[2].w) + gw[5].xy * s2) * bend;      // two travelling waves + downwind lean
    p.xyz += wv[0].xyz * off.x + wv[1].xyz * off.y;

    o.pos = p.x * proj[0] + p.y * proj[1] + p.z * proj[2] + p.w * proj[3];
    o.spec = float4(saturate(p.z * fade.x + fade.y), 0, 0, 0);
    o.fog = min(pow(max(p.z * fogP.x + fogP.y, 0.0), fogP.z), 1.0);

    float3 nv = Rot(n);
    float3 light = sunDiff.xyz * saturate(dot(sunDir.xyz, nv)) + sunAmb.xyz;
#if LIGHTS
    [unroll] for (int i = 0; i < 3; i++) {
        float3 L = Rot(pl[3 * i].xyz) - p.xyz;
        float il = rsqrt(dot(L, L));
        float dl = 1.0 / il;
        float3 lit = saturate(dot(L * il, nv)) * pl[3 * i + 1].xyz;
        light += lit / (pl[3 * i + 2].x + pl[3 * i + 2].y * dl + pl[3 * i + 2].z * dl * dl);
    }
#endif
    o.col = float4(min(light, 1.0) * c.xyz, c.w);
    o.uv = uv;
#if SHADOW
    float4 p1 = float4(p.xyz, 1.0);
    o.viewPos = p.xyz;
    o.viewNrm = nv;
#if SHADOW == 1
    o.sh0 = float3(dot(p1, sh[0]), dot(p1, sh[1]), dot(p1, sh[2]));
#else
    o.sh0 = float4(dot(p1, sh[0]), dot(p1, sh[1]), dot(p1, sh[2]), dot(p1, sh[9]));
    o.sh1 = float4(dot(p1, sh[3]), dot(p1, sh[4]), dot(p1, sh[5]), dot(p1, sh[10]));
    o.sh2 = float4(dot(p1, sh[6]), dot(p1, sh[7]), dot(p1, sh[8]), dot(p1, sh[11]));
#endif
#endif
    return o;
}
