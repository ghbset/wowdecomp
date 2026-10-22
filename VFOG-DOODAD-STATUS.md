# vfog grey doodads status (2026-10-22, root cause NOT found, no vfog change made)

Shipped instead: the occlusion flicker fix, cherry-picked to main as febdb524 (OcclusionCull.cpp only), built x64 + x86,
published as v2026.10.22-2005-occlusion-flicker (backups: tswow-client/Wow64.exe/Wow32.exe.bak-pre-vfogfix).

## User facts
- Vulkan and D3D11: stakes under the Thorn Hill huts, a spine arch, a thorn ring look grey/over-fogged, weather does not
  show on them, they pop to clear at some distance, a near one can be foggier than the hill behind it.
- `vfog 0` fixes it. `vfogFadeEarly 0` and `vfogFadeStencil 0` change nothing (both backends). Not occlusion culling.

## Ruled out by reading code
- Fading-model early pass (VolumetricFog.cpp DrawFadingEarly) and its stencil: user test.
- The models themselves: barbs_01/02, quillboar_hut01, thorncanopy_01, quillboarpost (common-2.MPQ) are all opaque or
  alpha-key materials, transparency track 32767 (= 1.0), no colour alpha. So they are NOT pass-1 elements by blend or
  alpha; outside the 10 yd stock fade band they draw in pass 0, before the composite.
- Stale depth copy: Vulkan (frame::DepthWritten, CGxDeviceVulkanDraw.cpp:370) and D3D11 (m_sceneDepthFresh cleared in
  Draw, CGxDeviceD3d11.cpp:3270) both invalidate on every depth-writing draw, and all M2 draws go through Draw().
- Thin objects and the low-res march: vf_composite DepthAwareUpsample falls back to a full-res march at the pixel's own
  depth when no tap matches (FogWithoutMatchingTap), so a thin stake in the depth buffer cannot take the background's fog.
- Bone palette spill into c224+ (shadow matrices) from DrawBatchDoodad's 75-bone cap: caps report 256 VS constants on
  all backends, so maxBones = 64 and instances stop at c222.
- CM2Lighting fog for map doodads (MapLoad.cpp ModelLightingCallback, MapDoodadDef SelectLights) reads DayNight::GetInfo()
  inside CM2Model::SetupLighting, after OnWorldUpdate pushed the stock fog out, so no stock-fog double count there.

## Strongest lead (not proven)
The broken objects are all repeated map doodads; the unique drum and the units next to them are fine. Repeated copies of
one model are drawn INSTANCED (M2Scene.cpp:2259-2296 -> CM2SceneRender::DrawBatchDoodad, CM2SceneRender.cpp:775) in
pass 0. A doodad inside its 10 yd fade band is type 0 instead (transparent -> pass 1), which would be the distance "pop".
Next: find what differs for type-2 draws on the translated (D3D11/Vulkan) shader path: its vertexPermute VS (instanced
bone indices), depth write, and the RT1 normal write mask from the gbuffer merge (1a8fe07e / 57db13bf).

## Ask the user (decisive, D3D11 or Vulkan, at the Thorn Hill hut)
1. `vfogDebug 3` (linear depth, white = 200 yd): are the stakes drawn as their own grey shade or do they show the
   ground/sky depth behind them? Missing -> depth problem (then step 3); present -> fog applied twice (BatchFog/stock).
2. `vfogTransparent 0` (composite after the whole world): if the stakes go right, something draws them after the early
   composite.
3. `d3d11DumpFrame 1` near a stake: gx.log shows whether the stake draw (BatchDoodad, DebugLog) writes depth.

## Session 2 (2026-10-22, vfogTransparent lead) - still no fix shipped, no build, no commit
Budget ran out before a provable root cause. What this session established:
- The ONLY things vfogTransparent=1 changes: composite at OnLiquidDone instead of OnWorldDone (s_late), BatchFog
  (fitted fog in CM2SceneRender::SetupLighting :373) and FogParticles. Pass 0 (incl. instanced DrawBatchDoodad) is
  drawn before OnLiquidDone with s_armed=false, so it never takes the fitted fog. Fog constant is VS c30, bones start c31.
- `vfogDebug != 0` forces s_late (VolumetricFog.cpp:843), so the user's vfogDebug 3 check showed the END-of-world depth,
  not the depth at the early composite. It does not prove the early composite saw the stakes.
- Models re-checked from common-2.MPQ (world/generic/quilboar/passive doodads/...): every material blend 0/1, flags 0 or 4
  (no 0x10), one transparency track = 32767 (decoded *1/32767 = 1.0), no colours. So an element of them is in pass 1
  ONLY through model->m_alpha < 1, i.e. a distance fade.
- Strong candidate for that fade: LEGION objcull (World.cpp:2208, CWorldScene::AddDoodadDefModelToModelScene):
  lodObjectCull fades doodads with small radius beyond lodObjectCullDist. Small repeated props (barbs, posts) fade at
  short range, the big unique drum does not, and inside lodObjectCullDist alpha = 1 -> pass 0 -> "pops clear when close".
  Pass 1 draws after RenderWeather -> "weather does not show on them". Fitted fog (s_fit inside 100 yd) -> grey.
- Open contradiction: such elements match FadingOpaqueElement (type 0, base blend <= 1, no 0x10, m_alpha < 1), so with
  vfogFadeEarly 1 DrawFadingEarly should pull them before the composite, yet the user saw no difference.
## Next decisive user tests (Vulkan or D3D11, Thorn Hill)
1. `lodObjectCull 0`: stakes fine -> confirmed fade route; then find why DrawFadingEarly misses/mis-fogs them.
2. With lodObjectCull 1: `vfogFadeEarly 1` + `vfogFadeStencil 0` vs `vfogFadeEarly 0`, looking at whether rain draws over
   the stakes (over = drawn before the composite, i.e. DrawFadingEarly caught them).

## Session 3 (2026-10-22): fallback shipped, root cause still unproven
- Shipped: vfogTransparent default "0" (VolumetricFog.cpp:725, plus the CVarInt fallback at :172). Built x64 + x86,
  published as v2026.10.22-2030-vfog-transparent0 (backups tswow-client/Wow64.exe/Wow32.exe.bak-pre-vfogfade).
  Cost: transparents (water, particles, blended M2s) take the fog of the scene behind them (coa TransparentFog=0).
- lodObjectCull is NOT the near-range fader: with the defaults (cullSize 15 -> 3 px, cullDist 30, pixScale = H/tan(fov/2)
  ~1300 at 1080p), a 1 yd radius stake only starts fading beyond ~150-200 yd. The stock band (WorldScene.cpp:28/34) fades
  fadeLevel 0 at 25-30 yd and level 1 at 90-100 yd.
- All fade sources write CM2Model::m_fadeAlpha (World.cpp:2252) -> m_alpha = baseAlpha*alpha2*fadeAlpha (M2Model.cpp:4738/4758),
  and that is exactly what makes M2Scene route an element to pass 1 as type 0 (M2Scene.cpp:1946/1964). So FadingOpaqueElement
  should match every fade source; why DrawFadingEarly made no visible difference is still open. Checked and fine: optGeo
  batches keep materialIndex/Layer, depth write stays on for fading batches (CM2SceneRender.cpp:470), the depthSort copy
  (M2Scene.cpp:2070, flag 1 = depth-only prepass) is in pass 1 too and is pulled with it, D3D11/Vulkan depth freshness.
- To restore the fitted path later: find (rig, d3d11DumpFrame) which draw puts the stakes after the early composite.
