// gxNormalBuffer (GBUF-STATUS.md): the world's interpolated view-space normal, written as a second render target by
// the M2 / WMO / terrain pixel shaders (Tools/VulkanShaders/bls/normals.py), rgb = n * 0.5 + 0.5, a = 1 where one was
// written. c9.x = 1 when s4 holds it this frame (host: VolumetricFog::BindNormalBuffer); elsewhere, and on pixels no
// world shader wrote (a = 0: water, fixed function, untranslated shaders), the caller's depth-rebuilt normal is used.
#ifndef VF_GBUFFER_HLSLI
#define VF_GBUFFER_HLSLI

float4 cGBuffer : register(c9);
sampler2D sNormal : register(s4);

// Toward the camera like the depth-rebuilt one (p = view position): a back face's interpolated normal is flipped.
float3 GBufferNormal(float2 pixel, float3 p, float3 rebuilt)
{
    [branch] if (cGBuffer.x > 0)
    {
        float4 g = tex2Dlod(sNormal, float4(pixel * DepthTexelSize(), 0, 0));
        if (g.a > 0.5)
        {
            float3 n = normalize(g.xyz * 2 - 1);
            return dot(n, p) > 0 ? -n : n;
        }
    }
    return rebuilt;
}

#endif
