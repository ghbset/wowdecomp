// VFog shared helpers: port of coa-vfog shaders/vf_common.hlsli (@ 4e31ddf, GPLv3) with wxl-vfog's
// horizon-band decode, retargeted to the decomp client. Register contract: docs/VFOG_PORT.md section 5.
//
// Pixel position (no VPOS: the Vulkan emitter has no vPos). Every VFog pass is one quad through the
// fixed-function VS that passes TEXCOORD0 = target uv, 0..1 across the target the pass draws into:
//   full-res passes (composite): the world viewport, c0 = (x, y, w, h) in back-buffer pixels;
//   low-res passes (march, temporal, history depth): the whole low-res target, c8 = (w, h, 1/w, 1/h).
// The shader expects uv = (i + 0.5) / size at the centre of pixel i, the D3D10+ rasterisation rule. D3D9's
// half-texel offset is the renderer's job (its quad is shifted half a pixel as FFX::SetupGeo_Quad does).
// The index is taken with floor(), so interpolation error well under half a pixel cannot change it.
float4 cWorldViewportPx : register(c0);
float4 cPixelGrid : register(c1);
float4 cViewToNdc : register(c2);
float4 cDepthToViewZ : register(c3);
row_major float4x4 cViewToWorld : register(c4);
float4 cLowResTarget : register(c8);
float4 cHorizonBand : register(c99);    // depth minZ, maxZ (0.998046875, 0.9990234375), near yd, far yd
float4 cDither : register(c79);         // x = vfogDither strength on 8-bit outputs, y = frame (mod 1024), z = on the fog stores


float2 ViewportOrigin()
{
    return cWorldViewportPx.xy;
}

float2 ViewportSize()
{
    return cWorldViewportPx.zw;
}

float2 ViewportLastPixelCentre()
{
    return ViewportOrigin() + ViewportSize() - 0.5;
}

bool OutsideViewport(float2 pixel)
{
    return any(pixel < ViewportOrigin()) || any(pixel > ViewportOrigin() + ViewportSize());
}

float FullPixelsPerLowResTexel()
{
    return cPixelGrid.x;
}

float FrameIndex()
{
    return cPixelGrid.y;
}

float2 DepthTexelSize()
{
    return cPixelGrid.zw;
}

float2 ViewToNdcScale()
{
    return cViewToNdc.xy;
}

float2 ViewToNdcOffset()
{
    return cViewToNdc.zw;
}

float DepthAtInfiniteViewZ()
{
    return cDepthToViewZ.x;
}

float DepthPerInverseViewZ()
{
    return cDepthToViewZ.y;
}

float MaxFogDistance()
{
    return cDepthToViewZ.z;
}

float DeepestWorldDepth()
{
    return cDepthToViewZ.w;
}

float3 CameraPositionWorld()
{
    return cViewToWorld[3].xyz;
}

float3 ViewToWorldDirection(float3 viewDirection)
{
    return mul(viewDirection, (float3x3)cViewToWorld);
}

float2 LowResSize()
{
    return cLowResTarget.xy;
}

float2 LowResTexelSize()
{
    return cLowResTarget.zw;
}

// Replaces VPOS on the low-res passes: the integer low-res texel this pixel is.
float2 LowResTexelFromUv(float2 uv)
{
    return floor(uv * LowResSize());
}

// Replaces VPOS + 0.5 on the full-res passes: the back-buffer pixel centre this pixel is.
float2 FullPixelFromUv(float2 uv)
{
    return ViewportOrigin() + floor(uv * ViewportSize()) + 0.5;
}

float SampleDepth(sampler2D depthSampler, float2 pixel)
{
    return tex2Dlod(depthSampler, float4(pixel * DepthTexelSize(), 0, 0)).r;
}

// Depth bands (docs/VFOG_PORT.md section 4): main band d <= c3.w (0.94) decodes through c3; the WDL horizon
// band [c99.x, c99.y] has its own projection; 0.94 < d < c99.x is empty in the decomp and counts as horizon
// (t clamps to 0); d > c99.y is sky. wxl-vfog InFarSlice, retargeted.
bool InFarSlice(float depth)
{
    return depth > DeepestWorldDepth() && depth <= cHorizonBand.y;
}

bool BeyondFarClip(float depth)
{
    return depth > DeepestWorldDepth() && !InFarSlice(depth);
}

bool IsSky(float depth)
{
    return depth > cHorizonBand.y;
}

float LinearDepth(float depth)
{
    if (InFarSlice(depth))
    {
        float t = saturate((depth - cHorizonBand.x) / max(cHorizonBand.y - cHorizonBand.x, 1e-6));
        return min(cHorizonBand.z * cHorizonBand.w / (cHorizonBand.w - t * (cHorizonBand.w - cHorizonBand.z)),
                   MaxFogDistance());
    }
    return BeyondFarClip(depth) ? MaxFogDistance()
                                : min(DepthPerInverseViewZ() / (depth - DepthAtInfiniteViewZ()), MaxFogDistance());
}

float2 LowResTexelToFullPixel(float2 lowResTexel)
{
    float2 pixel = ViewportOrigin() + lowResTexel * FullPixelsPerLowResTexel() +
                   floor(FullPixelsPerLowResTexel() * 0.5) + 0.5;
    return min(pixel, ViewportLastPixelCentre());
}

float2 FullPixelToLowResTexel(float2 pixel)
{
    return (pixel - ViewportOrigin() - floor(FullPixelsPerLowResTexel() * 0.5) - 0.5) / FullPixelsPerLowResTexel();
}

float2 LowResTexelToUv(float2 lowResTexel)
{
    return (lowResTexel + 0.5) * LowResTexelSize();
}

float2 PixelToNdc(float2 pixel)
{
    return float2((pixel.x - ViewportOrigin().x) / ViewportSize().x * 2 - 1,
                  1 - (pixel.y - ViewportOrigin().y) / ViewportSize().y * 2);
}

float2 NdcToPixel(float2 ndc)
{
    return ViewportOrigin() + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * ViewportSize();
}

float3 ViewRayAtUnitDepth(float2 pixel)
{
    float2 ndc = PixelToNdc(pixel);
    return float3((ndc.x - ViewToNdcOffset().x) / ViewToNdcScale().x,
                  (ndc.y - ViewToNdcOffset().y) / ViewToNdcScale().y, 1);
}

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// vfogDither (ALU only, no texture fetch).
// Triangular dither (the difference of two IGN values), +-strength/255: 1 is the standard amplitude for an 8-bit
// store, 2 twice that. `offset` decorrelates the passes; the frame term animates it so the temporal blend averages
// it out instead of accumulating a fixed bias.
float4 DitheredBy(float strength, float4 colour, float2 position, float2 offset, float frame)
{
    [branch] if (strength > 0)
    {
        float2 p = position + offset + frame * 5.588238;
        colour.rgb += (InterleavedGradientNoise(p) - InterleavedGradientNoise(p + float2(37.0, 61.0))) * (strength / 255);
    }
    return colour;
}

// The 8-bit outputs: composite, grade and water onto the back buffer.
float4 Dithered(float4 colour, float2 position, float2 offset, float frame)
{
    return DitheredBy(cDither.x, colour, position, offset, frame);
}

// The low-res fog stores (march, temporal). cDither.z = 0 on half-float targets, which need no dither. The
// saturate keeps the 0..1 range an 8-bit store clamps to, so both storages feed the composite the same values.
float4 DitheredStore(float4 colour, float2 position, float2 offset, float frame)
{
    return saturate(DitheredBy(cDither.z, colour, position, offset, frame));
}

// tex2Dlod at level 0 for uv in 0..1. The lod operand is saturate(uv - 2) = 0 rather than a literal 0: fxc
// moves a literal (uv, 0, 0)'s zw out of the step loop into a temp of its own per fetch site, held for the
// whole loop, and the noisy march ran out of ps_3_0's 32 temps on those alone.
float4 Tex2DLevel0(sampler2D s, float2 uv)
{
    return tex2Dlod(s, float4(uv, saturate(uv - 2)));
}

// A noise volume of n^3 texels packed as a 2D atlas (no volume textures in the Gx API; docs/VFOG_PORT.md
// "Noise atlases"). Slice z sits in tile (z % cols, z / cols), tiles of `tile` = n + 2 texels, row 0 at the
// top; tile texel (1 + x, 1 + y) holds voxel (x, y, z) and the 1-texel border holds the wrapped neighbour,
// so bilinear filtering inside a tile is the volume's wrap filtering. The slice lerp happens here, which
// makes the result trilinear with wrap addressing on all three axes, as coa's tex3Dlod.
// p is in volume texels, p = uvw * n (texel centres at i + 0.5); the atlas is (cols * tile) x (n / cols * tile).
float4 SampleNoiseAtlas(sampler2D s, float3 p, float n, float cols, float tile)
{
    float2 inTile = frac(p.xy / n) * n + 1;     // texel in the tile, past its border
    float z = p.z - 0.5;                        // slice-centre space
    float f = frac(z);
    float slice = z - f;
    slice -= n * floor(slice / n);              // wrap to 0..n-1
    float2 atlas = float2(cols, n / cols) * tile;
    float row = floor(slice / cols);
    float4 a = Tex2DLevel0(s, (float2(slice - row * cols, row) * tile + inTile) / atlas);
    slice = slice + 1 < n ? slice + 1 : 0;
    row = floor(slice / cols);
    float4 b = Tex2DLevel0(s, (float2(slice - row * cols, row) * tile + inTile) / atlas);
    return lerp(a, b, f);
}
