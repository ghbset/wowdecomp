// Falling leaves (wxl-treewind Leaves.cpp kLeafPsHlsl): the leaf silhouette times the vertex colour. The plugin's
// alpha test (ALPHAREF 128, GREATEREQUAL) is a clip here, so no backend's fixed-function alpha test is needed.
sampler2D s0 : register(s0);
float4 main(float4 col : COLOR0, float2 uv : TEXCOORD0) : COLOR
{
    float4 t = tex2D(s0, uv);
    clip(t.a - 0.5);
    return float4(t.rgb * col.rgb, t.a * col.a);
}
