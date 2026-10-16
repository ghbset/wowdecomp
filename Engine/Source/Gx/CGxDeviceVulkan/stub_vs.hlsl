cbuffer c : register(b0) { float4x4 mvp; };
float4 main(float3 pos : POSITION) : SV_Position { return mul(float4(pos, 1.0), mvp); }
