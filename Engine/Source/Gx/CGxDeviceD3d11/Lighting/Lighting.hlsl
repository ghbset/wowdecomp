// Modern lighting post passes (port of Badgermilk0 D3d11Lighting.cpp s_postHlsl): SSAO, height fog, bloom, tonemap.
// Compiled offline by Lighting.py (vkd3d-compiler, as ffshaders/build.py) into LightingShaders.h. The Vulkan
// device runs the same passes as GLSL compute (CGxDeviceVulkan/Shaders/Lighting*.comp); keep the two in step.
cbuffer GxPost : register(b0) {
    row_major float4x4 gxInvProj;  // NDC to view
    row_major float4x4 gxInvView;  // view to world
    float4 gxViewportPx;           // the world viewport: x left, y top, z width, w height (pixels)
    float4 gxDepthRange;           // x min, y max, z 1 / (max - min)
    float4 gxTarget;               // this pass's target: x width, y height, z 1 / width, w 1 / height
    float4 gxAo;                   // x radius (yards), y intensity, z fade start, w fade end
    float4 gxAoProj;               // x pixels per yard at clip w 1
    float4 gxFogColor;             // rgb the engine's fog colour, a scattering
    float4 gxFogParams;            // x start, y end, z density, w 1 = the stock fog is on
    float4 gxSunColor;             // rgb
    float4 gxSunDir;               // xyz toward the sun (world)
    float4 gxHeightFog;            // x density, y falloff, z base height (world z), w most opacity
    float4 gxCamera;               // xyz camera (world), w time
    float4 gxBloom;                // x threshold, y knee, z intensity, w 1 = on
    float4 gxTone;                 // x exposure, y mode (0 clamp, 1 neutral, 2 ACES)
    float4 gxSource;               // the texture sampled: xy 1 / size; zw the blur direction
    float4 gxProjW;                // clip w = dot(float4(view position, 1), gxProjW)
};

#ifdef GX_MSAA
Texture2DMS<float> gxDepth : register(t0);
float GxDepthAt(int2 p) {
    return gxDepth.Load(p, 0);
}
#else
Texture2D<float> gxDepth : register(t0);
float GxDepthAt(int2 p) {
    return gxDepth.Load(int3(p, 0));
}
#endif

Texture2D gxColor : register(t1);
Texture2D gxColor2 : register(t2);
SamplerState gxLinear : register(s0);

float3 GxDec(float3 c) {
    return pow(max(c, 0.0), 2.2);
}

float3 GxEnc(float3 c) {
    return pow(max(c, 0.0), 1.0 / 2.2);
}

// our D3D11 device applies no half-pixel offset (README: Half-pixel), so the pixel centre is the geometry's
float3 GxViewPos(float2 pixel, float depth) {
    float2 ndc = float2((pixel.x - gxViewportPx.x) / gxViewportPx.z * 2.0 - 1.0, 1.0 - (pixel.y - gxViewportPx.y) / gxViewportPx.w * 2.0);
    float4 h = mul(float4(ndc, (depth - gxDepthRange.x) * gxDepthRange.z, 1.0), gxInvProj);
    return h.xyz / h.w;
}

// the sky and the clear colour lie behind the world's depth range
bool GxIsGeometry(float depth) {
    return depth <= gxDepthRange.y + 0.00001 && depth < 0.99999;
}

int2 GxClampPixel(float2 p) {
    return int2(clamp(p, gxViewportPx.xy, gxViewportPx.xy + gxViewportPx.zw - 1.0));
}

float GxClipW(float3 p) {
    return dot(float4(p, 1.0), gxProjW);
}

struct GxPostVaryings {
    float4 pos : SV_Position;
};

GxPostVaryings FullscreenVs(uint id : SV_VertexID) {
    GxPostVaryings o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// SSAO at half resolution: x the unoccluded fraction, y the pixel's clip w (0 = sky)
float4 AoPs(GxPostVaryings i) : SV_Target0 {
    float2 pixel = gxViewportPx.xy + floor(i.pos.xy) * 2.0 + 0.5;
    float d = GxDepthAt(GxClampPixel(pixel));

    if (!GxIsGeometry(d)) {
        return float4(1.0, 0.0, 0.0, 0.0);
    }

    float3 p = GxViewPos(pixel, d);
    float w = GxClipW(p);
    float fade = 1.0 - saturate((w - gxAo.z) / max(gxAo.w - gxAo.z, 1.0));

    if (fade <= 0.0) {
        return float4(1.0, w, 0.0, 0.0);
    }

    // the normal from the nearer neighbour on each axis, so it does not bend over an edge
    float3 l = GxViewPos(pixel - float2(1.0, 0.0), GxDepthAt(GxClampPixel(pixel - float2(1.0, 0.0))));
    float3 r = GxViewPos(pixel + float2(1.0, 0.0), GxDepthAt(GxClampPixel(pixel + float2(1.0, 0.0))));
    float3 u = GxViewPos(pixel - float2(0.0, 1.0), GxDepthAt(GxClampPixel(pixel - float2(0.0, 1.0))));
    float3 b = GxViewPos(pixel + float2(0.0, 1.0), GxDepthAt(GxClampPixel(pixel + float2(0.0, 1.0))));
    float3 dx = dot(r - p, r - p) < dot(p - l, p - l) ? r - p : p - l;
    float3 dy = dot(b - p, b - p) < dot(p - u, p - u) ? b - p : p - u;
    float3 n = normalize(cross(dy, dx));
    n = dot(n, p) > 0.0 ? -n : n;

    float radius = gxAo.x;
    // the cap follows the screen: a fixed 96 shrank the radius at 4K to a fraction of a yard
    float radiusPx = clamp(radius * gxAoProj.x / max(w, 0.01), 3.0, gxViewportPx.w * 0.15);
    float noise = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
    // two depth-buffer steps at this distance: a neighbour nearer than that may be the same surface, quantised
    float bias = 2.0 * gxAoProj.y * w * w;
    float sum = 0.0;
    const uint count = 12;

    [unroll] for (uint s = 0; s < count; s++) {
        float t = (s + 0.5) / count;
        float angle = (t * 7.0 + noise) * 6.2831853;
        float2 q = pixel + float2(cos(angle), sin(angle)) * (radiusPx * t);
        int2 qi = GxClampPixel(q);
        float3 v = GxViewPos(float2(qi) + 0.5, GxDepthAt(qi)) - p;
        float vv = dot(v, v);
        float range = saturate(1.0 - vv / (radius * radius * 4.0));
        sum += saturate((dot(v, n) - bias) * rsqrt(max(vv, 0.0001)) - 0.05) * range;
    }

    float ao = saturate(1.0 - gxAo.y * sum / count);
    return float4(lerp(1.0, ao, fade), w, 0.0, 0.0);
}

// a depth-aware Gaussian along gxSource.zw
float4 BlurPs(GxPostVaryings i) : SV_Target0 {
    int2 p = int2(i.pos.xy);
    float2 c = gxColor.Load(int3(p, 0)).xy;

    if (c.y <= 0.0) {
        return float4(1.0, 0.0, 0.0, 0.0);
    }

    float sum = c.x;
    float total = 1.0;
    int2 dir = int2(gxSource.zw);
    int2 last = int2(gxTarget.xy) - 1;

    [unroll] for (int k = -4; k <= 4; k++) {
        if (k == 0) {
            continue;
        }

        float2 s = gxColor.Load(int3(clamp(p + dir * k, int2(0, 0), last), 0)).xy;
        float weight = exp(-(k * k) / 8.0) * (s.y > 0.0 ? saturate(1.0 - abs(s.y - c.y) / (0.04 * c.y + 0.1)) : 0.0);
        sum += s.x * weight;
        total += weight;
    }

    return float4(sum / total, c.y, 0.0, 0.0);
}

// the occlusion into the scene, a multiply: upsampled from half resolution by depth
float4 AoCompositePs(GxPostVaryings i) : SV_Target0 {
    float d = GxDepthAt(int2(i.pos.xy));
    float w = GxClipW(GxViewPos(i.pos.xy, d));

    // lightingDebug 4: red geometry, green clip w / 200, blue the half-resolution occlusion under the pixel
    if (gxTone.z > 3.5) {
        float2 h4 = gxColor.Load(int3(int2((i.pos.xy - gxViewportPx.xy) * 0.5), 0)).xy;
        return float4(GxIsGeometry(d) ? 1.0 : 0.0, gxTone.z > 4.5 ? d : saturate(w / 200.0), h4.x, 1.0);
    }

    if (!GxIsGeometry(d)) {
        return 1.0;
    }
    float2 h = (i.pos.xy - gxViewportPx.xy - 0.5) * 0.5;
    int2 base = int2(floor(h));
    float2 f = h - base;
    int2 last = int2(1.0 / gxSource.xy + 0.5) - 1;
    float sum = 0.0;
    float total = 0.0;

    [unroll] for (int y = 0; y < 2; y++) {
        [unroll] for (int x = 0; x < 2; x++) {
            float2 s = gxColor.Load(int3(clamp(base + int2(x, y), int2(0, 0), last), 0)).xy;
            float bilinear = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
            float weight = bilinear * (s.y > 0.0 ? exp(-abs(s.y - w) / (0.05 * w + 0.1)) : 0.0) + bilinear * 0.0001;
            sum += s.x * weight;
            total += weight;
        }
    }

    return float4((sum / max(total, 0.00001)).xxx, 1.0);
}

// height fog and the sun's scattering, blended over the scene by alpha
float4 FogPs(GxPostVaryings i) : SV_Target0 {
    float d = GxDepthAt(int2(i.pos.xy));

    if (!GxIsGeometry(d)) {
        discard;
    }

    float3 p = GxViewPos(i.pos.xy, d);
    float3 pw = mul(float4(p, 1.0), gxInvView).xyz;
    float3 ray = pw - gxCamera.xyz;
    float dist = length(ray);
    ray /= max(dist, 0.0001);

    // the stock fog's visibility here: the vertex shaders' formula on view z
    float visible = 1.0;

    if (gxFogParams.w > 0.0) {
        visible = min(pow(max((gxFogParams.y - p.z) / max(gxFogParams.y - gxFogParams.x, 0.001), 0.0), gxFogParams.z), 1.0);
    }

    float sun = pow(saturate(dot(ray, gxSunDir.xyz)), 8.0) * gxFogColor.a;
    float a1 = (1.0 - visible) * sun;
    float3 c1 = gxSunColor.rgb;
    float a2 = 0.0;
    float3 c2 = lerp(gxFogColor.rgb, gxSunColor.rgb, sun * 0.5);

    if (gxHeightFog.x > 0.0) {
        // exponential fog falling off with height, integrated from the camera to the pixel
        float k = gxHeightFog.y;
        float kd = k * (pw.z - gxCamera.z);
        float integral = gxHeightFog.x * exp(clamp(-k * (gxCamera.z - gxHeightFog.z), -40.0, 20.0)) * dist * (abs(kd) > 0.0001 ? (1.0 - exp(clamp(-kd, -40.0, 40.0))) / kd : 1.0);
        a2 = min(1.0 - exp(-max(integral, 0.0)), gxHeightFog.w);
    }

    float a = 1.0 - (1.0 - a1) * (1.0 - a2);

    if (a <= 0.0001) {
        discard;
    }

    return float4((c1 * (a1 * (1.0 - a2)) + c2 * a2) / a, a);
}

// Call of Duty's 13-tap downsample
float3 GxDown13(float2 uv, float2 t, bool decode) {
    static const float2 offsets[13] = { float2(-2, -2), float2(0, -2), float2(2, -2), float2(-1, -1), float2(1, -1), float2(-2, 0), float2(0, 0), float2(2, 0), float2(-1, 1), float2(1, 1), float2(-2, 2), float2(0, 2), float2(2, 2) };
    static const float weights[13] = { 0.03125, 0.0625, 0.03125, 0.125, 0.125, 0.0625, 0.125, 0.0625, 0.125, 0.125, 0.03125, 0.0625, 0.03125 };
    float3 sum = 0.0;

    [unroll] for (uint k = 0; k < 13; k++) {
        float3 c = gxColor.SampleLevel(gxLinear, uv + offsets[k] * t, 0.0).rgb;
        // the scene's own colour clamped, so one hot pixel does not flood the screen
        sum += (decode ? min(GxDec(c) * gxTone.x, 16.0) : c) * weights[k];
    }

    return sum;
}

float4 BloomPrefilterPs(GxPostVaryings i) : SV_Target0 {
    float3 c = GxDown13(i.pos.xy * gxTarget.zw, gxSource.xy, true);
    float bright = max(c.r, max(c.g, c.b));
    float knee = gxBloom.y;
    float soft = clamp(bright - gxBloom.x + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.00001);
    return float4(c * (max(soft, bright - gxBloom.x) / max(bright, 0.00001)), 1.0);
}

float4 BloomDownPs(GxPostVaryings i) : SV_Target0 {
    return float4(GxDown13(i.pos.xy * gxTarget.zw, gxSource.xy, false), 1.0);
}

// a 3x3 tent, added into the larger level
float4 BloomUpPs(GxPostVaryings i) : SV_Target0 {
    float2 uv = i.pos.xy * gxTarget.zw;
    float2 t = gxSource.xy;
    float3 sum = gxColor.SampleLevel(gxLinear, uv, 0.0).rgb * 4.0;
    sum += (gxColor.SampleLevel(gxLinear, uv + float2(-t.x, 0.0), 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(t.x, 0.0), 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(0.0, -t.y), 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(0.0, t.y), 0.0).rgb) * 2.0;
    sum += gxColor.SampleLevel(gxLinear, uv - t, 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + t, 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(t.x, -t.y), 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(-t.x, t.y), 0.0).rgb;
    return float4(sum / 16.0, 1.0);
}

float3 GxNeutral(float3 c) {
    // the stock colour up to the shoulder, then a curve that approaches white
    const float shoulder = 0.8;
    float m = max(c.r, max(c.g, c.b));

    if (m <= shoulder) {
        return c;
    }

    float mapped = shoulder + (1.0 - shoulder) * (1.0 - exp(-(m - shoulder) / (1.0 - shoulder)));
    return c * (mapped / m);
}

float3 GxAces(float3 c) {
    return saturate(c * (2.51 * c + 0.03) / (c * (2.43 * c + 0.59) + 0.14));
}

float4 TonemapPs(GxPostVaryings i) : SV_Target0 {
    float4 s = gxColor.Load(int3(i.pos.xy, 0));
    float3 c = GxDec(s.rgb) * gxTone.x;

    if (gxBloom.w > 0.0) {
        // the chain adds every level into the one above: its mean, times the intensity
        c += gxColor2.SampleLevel(gxLinear, i.pos.xy * gxSource.xy, 0.0).rgb * (gxBloom.z / gxSource.z);
    }

    // no NaN or negative light reaches the frame
    c = max((asuint(c) & 0x7FFFFFFFu) > 0x7F800000u ? 0.0 : c, 0.0);

    if (gxTone.y > 1.5) {
        c = GxAces(c);
    } else if (gxTone.y > 0.5) {
        c = GxNeutral(c);
    } else {
        c = saturate(c);
    }

    return float4(GxEnc(c), s.a);
}

