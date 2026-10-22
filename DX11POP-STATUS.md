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

## Session 2 (2026-10-22, rebased onto main 1299e859, no fix, no build)

Re-read the screenshots: the far hut's *stakes* (blue-banded wooden spikes) are pale/grey-white while the same
hut's roof and platform stay dark. So the switch is per doodad and depends on doodad size, not per pixel.

Read and ruled out:
- No M2 skin-LOD by distance. GetSkinProfileIndex picks by bone count only (M2Shared.cpp:1644).
- lodObjectCull (World.cpp:2213): with defaults (size 15, so 3 px, band 3 px) a doodad with r = 2 yd only fades past about 900 yd.
- TreeLeaves kRange 70 is falling-leaf particles, not model shading. M2TreeSway has no distance gate.
- A2C (CGxDeviceD3d11.cpp:3212) only touches GxBlend_AlphaKey/Opaque with AlphaRef > 0. M2 opaque sets AlphaRef 0
  (CM2SceneRender.cpp:455), so opaque M2s never get A2C.
- The vfog depth bands (vf_common.hlsli:126) are pixel-level, and world depth stays <= 0.94, so they cannot make one object switch.

Best candidate (not proven): the stock doodad distance fade combined with fade levels by size.
- MapDoodadDef.cpp:98 sets the level by radius: <= 4 yd is level 1, fading at (100 x environmentDetail) - 10 yd
  (WorldScene.cpp:146, detail clamped to 0.5..1.5, so 40-150 yd). The hut body is level 2 or 3 and fades much farther out.
- World.cpp:2197-2205: inside the band alpha < 1, which sends the doodad to M2 pass 1 (CM2SceneRender.cpp:448
  s_blendModes[pass]: opaque becomes alpha blend). At alpha > 0.99 it snaps to 1 and returns to the opaque path, which
  is the hard pop. In pass 1 it is blended and gets vfog fog from the depth behind it (the vfog snapshot is taken
  before fading draws). vfogFadeEarly 0 leaves all of this in place, so the user's test does not rule it out.
- Weak point: the band is only 10 yd wide. If the user sees washed stakes across a much wider range, this is not it.

Ask the user (D3D11, at Thorn Hill), in order:
1. `objectFade 0`: if the stakes stay solid at all distances (they pop in at the edge instead), the cause is the
   fade path. Next look at what pass-1 alpha blend does to opaque M2 combiners on D3D11 (PS alpha output).
2. `environmentDetail 0.5` then `1.5`: if the snap distance moves with it, the fade band is confirmed.
3. If neither changes it: `vfog 0`, then `extShadowQuality 0` (shadow band and caster cut-offs, ShadowQuery.cpp:913
   maxDoodadSize / IsBeyondFadeDistance: the hut platform may stop shadowing its stakes past a band).

## Session 3 (2026-10-22): occlusionCulling, fix committed, compile-checked only

User test: unchecking Advanced > "Occlusion culling" (CVar `occlusionCulling`, default 0, Vulkan only:
OcclusionCull.cpp Enabled() requires GxApi_Vulkan) stops the unfocused-window flicker. So item 3 was on Vulkan.
Not the stock `occlusion` CVar (sun-glare query only, DayNight.cpp:3347).

Mechanism (OcclusionCull.cpp, d2605ac0 + 4d4185aa), answers are 1-3 frames old:
- BeginFrame reset every answer when a frame took > 250 ms (kStallMs). Near that frame time, culling turned on and
  off every other frame. Removed: the jump test plus the per-answer position gate already cover stale answers.
- An "occluded" answer was kept through camera turns: the gate checked position only. The box is only rasterised
  where it is on screen, against on-screen depth, so after a turn the answer can be about a different part of the
  box (objects pop in late at screen edges). Answers now also need the view direction within 0.8 degrees (camVec dot).
Does NOT explain item 1: D3D11 never runs this system. Item 4 (identity giants): no link found in code; ask for an A/B.
Note: World.cpp:3681 sets m_culled = 0 before the GPU test, so occluded doodads still look "not culled" to other code.

Build: OcclusionCull.cpp compiled clean with the wt-gbuf-x64 flags against this tree; no full Wow link (this
worktree has no build dir in /root/wowdecomp-xbuild).
