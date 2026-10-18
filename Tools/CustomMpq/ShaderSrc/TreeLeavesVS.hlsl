// Falling leaves (wxl-treewind Leaves.cpp kLeafVsHlsl, branch leaves; docs/BACKLOG_PORT.md "Falling leaves").
// One static PNCT2 vertex per quad corner: position = (corner x, corner y, tree row), the slot's eight random
// numbers in normal.xyz, texcoord0.xy, texcoord1.xy and colour.x (the plugin's TEXCOORD0/1 float4s, repacked to a
// stock vertex format). The leaf's place is worked out from (tree constants, time); an idle slot collapses to a
// point outside the clip volume.
float4 gM[4]   : register(c0);    // rows of camera-relative world -> clip (row vectors, the device's native projection)
float4 gCam    : register(c4);    // xyz camera, w time
float4 gP      : register(c5);    // size, fallSpeed, rate, calm
float4 gP2     : register(c6);    // drift, range
float4 gPr     : register(c7);    // |P00|, |P11|, fogStart, 1/(fogEnd-fogStart)
float4 gLight  : register(c8);
float4 gFog    : register(c9);
float4 gT[128] : register(c10);   // per tree: (x,y,z,seed) (crownZ,crownR,top,shed) (windX,windY,speed,gust) (r,g,b,fade)
struct In  { float4 q : POSITION; float3 n : NORMAL; float4 c : COLOR0; float2 t0 : TEXCOORD0; float2 t1 : TEXCOORD1; };
struct Out { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
float H(float n) { return frac(sin(n) * 43758.5453); }
Out main(In i)
{
    Out o;
    int ti = (int)i.q.z * 4;
    float4 A = gT[ti], B = gT[ti + 1], C = gT[ti + 2], D = gT[ti + 3];
    float t = gCam.w;
    // The slot's random numbers, turned by the tree's own seed: a tree's leaves are the same whichever
    // row of the table it sits in (the rows change as the camera moves).
    float4 ra = frac(float4(i.n, i.t0.x) + A.w * float4(17.13, 31.71, 47.37, 61.93));
    float4 rb = frac(float4(i.t0.y, i.t1.x, i.t1.y, i.c.x) + A.w * float4(73.19, 89.53, 97.61, 113.47));
    float ang = ra.x * 6.2831853, rad = B.y * sqrt(ra.y);
    float z0 = B.x + (ra.z - 0.5) * 0.3 * B.z;
    float life = clamp(z0 / gP.y, 2.5, 16.0);
    float period = life * (1.15 + 0.6 * ra.w);
    float tt = t + rb.x * period;
    float cyc = floor(tt / period);
    float age = tt - cyc * period;
    float g = 0.5 + 0.5 * sin((t - age) * 0.37 + A.w * 3.14159265);        // the tree's gust when this leaf let go
    float chance = gP.z * B.w * (gP.w + (1.0 - gP.w) * g * g * C.w);
    float cs = rb.y * 913.0 + cyc * 17.131;
    float u = age / life;
    float fl = 0.35 + 0.3 * H(cs + 1.1), fr = 1.1 + 1.4 * H(cs + 2.3), fp = H(cs + 3.9) * 6.2831853;
    float carry = C.z * gP2.x * age;
    float3 w;
    w.x = A.x + cos(ang) * rad + C.x * carry + sin(age * fr + fp) * fl;
    w.y = A.y + sin(ang) * rad + C.y * carry + cos(age * fr * 0.83 + fp) * fl;
    w.z = A.z + z0 * (1.0 - u) + sin(age * fr * 1.7 + fp) * 0.12;
    float3 rel = w - gCam.xyz;
    float dist = length(rel);
    float4 c = rel.x * gM[0] + rel.y * gM[1] + rel.z * gM[2] + gM[3];
    float spin = age * (1.4 + 2.2 * H(cs + 5.3)) + fp;
    float turn = 0.25 + 0.75 * abs(cos(age * (2.0 + 2.5 * H(cs + 6.1)) + fp));   // edge-on now and then
    float sn, cn;
    sincos(spin, sn, cn);
    // No blending (the leaf writes depth): it grows in and shrinks out where it used to fade.
    float alpha = D.w * saturate(u / 0.06) * saturate((1.0 - u) / 0.1) * saturate((1.0 - dist / gP2.y) / 0.25);
    float size = gP.x * (0.7 + 0.6 * rb.z) * alpha;
    float2 off = float2(i.q.x * cn - i.q.y * sn * turn, i.q.x * sn + i.q.y * cn * turn) * size;
    c.xy += off * gPr.xy;
    float fog = saturate((dist - gPr.z) * gPr.w);
    float tone = 0.8 + 0.4 * rb.w;
    o.col = float4(lerp(D.rgb * tone * gLight.rgb, gFog.rgb, fog), 1.0);
    o.uv = i.q.xy * 0.5 + 0.5;
    bool ok = age <= life && H(cs + 0.7) < chance && dist < gP2.y && dist > 0.6 && c.w > 0.3;
    o.pos = ok ? c : float4(0.0, 0.0, -2.0, 1.0);
    return o;
}
