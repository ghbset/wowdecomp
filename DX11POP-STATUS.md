# dx11-texpop status (2026-10-22, stopped at budget, no fix, no build)

Symptom in dx11/09-26-50..09-27-09: large thorn-tree doodads at Thorn Hill render as see-through ghosts (overlapping
silhouettes, hill texture visible through them) and turn solid at a distance threshold. This is blending, not a mip.

Ruled out by reading code:
- Texgen rename in 9d04ece0: value-preserving (Types.h GxTexGen_World=2/View=3 = old EyeLinear/Unk3), same in D3D9/D3D11/Vulkan.
- D3D11 sampler (CGxDeviceD3d11.cpp ~2790): MinLOD 0, MaxLOD FLT_MAX for mip filters, MipLODBias = texLodBias. OK.
- Texture mips (CGxDeviceD3d11Texture.cpp:109): full chain from baseMip, 565 widened with alpha 0xFF. OK.
- Blend tables match D3D9 s_srcBlend/s_dstBlend; Unk84 blend factor sets Dirty_Blend; stencil states set Dirty_Depth.
- lodObjectCull (World.cpp:2213) only touches radius < 10 yd doodads.
- Stock doodad fade (World.cpp:2193) is unchanged: 20 yd band at s_fadeDist*environmentDetail.

Leading suspects (all distance-gated, all end in a pop when alpha snaps to 1 at 0.99 and the model leaves the fade route):
1. vfog fade-early + fade-stencil cross-fade (shared code, all backends): VolumetricFog.cpp:1000-1037 draws fading M2s
   early and stencil-marks alpha (top 3 bits); VolumetricFogRenderer.cpp:947-955 cross-fades the model over the
   background fog in 7 steps. On D3D11/Vulkan "with" depth = the scene snapshot (Renderer.cpp:1112). D3D11's
   m_sceneDepthFresh cache (CGxDeviceD3d11.cpp:1392) is only cleared by depth-writing draws.
2. D3D11 plain alpha-to-coverage (CGxDeviceD3d11.cpp:3050): no alpha sharpening, Vulkan sharpens (AlphaTest.glsl:11).

In-game bisect (D3D11): vfogFadeStencil 0, then vfogFadeEarly 0, then vfog 0, then gxAlphaToCoverage 0.
Whichever clears the ghosts names the path; then d3d11DumpFrame 1 near a ghost tree logs its draws to gx.log.
