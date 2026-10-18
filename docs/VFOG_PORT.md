# Volumetric fog: native coa-vfog port

Branch `vfog` (worktree `/root/d3d11-port/wt-vfog`, from `custom-mpq` @ 1ad26f5).
Source: coa-vfog `/root/wotlk-renderers/coa-vfog` @ 4e31ddf (GPLv3). The wxl plugin port
`/root/wxl-vfog` is a reference for the lessons it learned, not for its code layout.
Forever-derived data (`fogdata.bin`) is allowed for this port.

## 0. Starting state

Commit 1ad26f5 removed the whole far-field stack: `FarField::*`, `horizonFog`, the FogStretch, FarBlend,
FarFoliage, farTerrainLod and the CustomMpq own-shader folder. `docs/archive/FARFIELD_FOG.md` describes
code that is gone. That leaves:

- stock fog: `DayNight::UpdateFog` fills `DNInfo::m_fog`, `m_fogInterior` and `m_fogGroup` with no stretch.
  Every consumer reads those: terrain VS c12, M2/WMO `CM2Lighting::SetFog`, liquids, detail doodads, and
  `ApplyFogState`'s fixed-function `GxRs_Fog*`. On Vulkan the fixed-function fog is Ff.frag, which the stock WDL uses.
- stock WDL horizon: `CWorldScene::RenderHorizon` draws in depth band [0.998046875, 0.9990234375] with
  its own projection, `Exact(fov*0.6, aspect, farZ-50, horizonFarclipScale*farclip)` (World.cpp:2045).
  `horizonFarclipScale` defaults to 4.
- the custom shader pipeline has to be restored. WP2 brings back `build_shaders.py`, the `Folder` archive
  in `Tools/VulkanShaders/bls/archive.py` and the `Shaders` entry in `build_custom_mpq.py`, all from e5ef0b0.

"Integrate with far-field fog" therefore means: **vfog is the single fog for every depth band**. While it
draws, the stock fog pushes out of range for every consumer at once, because all of them read DNInfo. The
composite then fogs the main band, the horizon band and the sky at their true distances. Any future far pass
that reads DNInfo fog (as the old FarField did) is covered automatically.

## 1. Frame, insertion points

`CGWorldFrame::OnWorldRender` (WoW/Source/Ui/WorldFrame.cpp:1659). coa-vfog's address for each step is in brackets.

| When | Where | Call | coa-vfog |
|---|---|---|---|
| world update | `CWorld::Update` World.cpp:1559, right after `CWorld::farFog = ...m_fog.m_end` | `VolumetricFog::OnWorldUpdate()` decides `Active()`, snapshots `m_fog`/`m_fogInterior`, pushes start/end to 50000/100000 | 0x4FB03D push |
| opaque done | WorldFrame.cpp:1746, after `scene->Draw(M2PASS_0)` | `OnOpaqueDone()` captures the view, projection, viewport and point lights | 0x4F911D |
| early composite | WorldFrame.cpp:1764, after `RenderLiquidPass()` in the **above-water** branch | `OnLiquidDone()` runs BuildFogParams then RenderFog | 0x4F9170 + TransparentFog |
| late transparents | `CM2SceneRender::SetupLighting`, before `CShaderEffect::SetFogParams` | `BatchFog()` gives a fitted linear fog | 0x81FD15 |
| particles | `CM2SceneRender::DrawParticle` | while `Active()`, emitters with blend <= 2 get fog flag 2 (wxl-vfog 0.6.12) | 0x8214E0 |
| fading M2s | `OnLiquidDone()`, before the glare and the composite | `DrawFadingEarly()`: the pass-1 elements of models fading at their draw distance that write depth are taken out of `m_passElements[M2PASS_1]` (order kept, list compacted) and drawn with `CM2SceneRender::Draw(M2PASS_1, ...)` | wxl-vfog 0.6.0 DrawFadingModelsBeforeFog |
| liquid depth | MaterialWater.cpp:149/228/308 | `GxRs_DepthWrite = stock \|\| LiquidDepthWrite()` | LiquidDepth |
| world done | end of OnWorldRender (before `FFX::Finish`) | `OnWorldDone()` restores DNInfo fog, decides the grade | 0x4F9281 restore |
| world rendered | `CGWorldFrame::RenderWorld`, after `OnWorldRender` (glow done), before `WorldTextRenderBatch` | `OnWorldRendered()` draws the grade | 0x4FB042 |

The decisions:
- **Push-out with no lag.** coa-vfog guesses from last frame, and wxl needed an 8-frame hold. Natively,
  `OnWorldUpdate` knows this frame's answer before any fog constant is built. `Active()` is true when: CVar
  `vfog` is set, `RendererReady()`, the camera is not in liquid (`World::SceneCamLiquidStatus`), and FFX/fog
  master enable allow it. The push happens after `farFog` is read, so object fog culling stays stock.
- **Early composite by default.** coa's default draws at FFX::Finish. We use its TransparentFog
  mode, which wxl-vfog shipped as its default. `vfogTransparent 0` is coa's TransparentFog=0: one composite in
  `OnWorldDone`, after the whole world, so transparents take the fog of the scene behind them. In the early mode
  the sun/moon glare (`DayNight::RenderGlares`) is drawn just before the composite and the stock call skipped
  (coa DrawGlareBeforeTheFog). The interior blend counts only when a camera WMO group lacks flags 0x48 (coa
  CaptureInteriorUnsafe). With stock fog pushed out, anything drawn after the composite
  would otherwise be bare. Transparents drawn after it (weather, M2PASS_1, spell visuals, particles, GxuCat_1)
  get the fitted linear fog. The fit uses the screen-wide fit, or the per-model fit when the model is past the
  fit range (wxl-vfog 0.4/0.6.13).
- **Under water: off** by default, with stock fog (coa `Underwater=0`). `vfogUnderwater 1` (vfog13, coa Underwater=1):
  the stock fog stays (no push-out) and the fog is composited once in `OnWorldDone`, glow compensation 0 (coa
  engine.cpp:409); no modern water, no grade. A camera that goes under mid-frame (`OnViewerLiquid`) switches to that.
- **Late composite** (vfog13, coa FogDrawsInPlaceOfStockFog / EarlyCompositeUnready): the early composite and the
  fitted fog run only above water with `vfogStockFog 1`, `vfogDebug 0` and `vfogSunMarker 0`; otherwise the fog is
  drawn once after the world. `vfogStockFog 0` keeps the stock fog, drops the distance layer (coa fog_model.cpp:214)
  and gives the water shader the stock fog constants with no volumetric reflection fog (coa stockFogApplies).
- **ForeverGlow** (vfog13, `vfogForeverGlow`, coa forever_look.cpp FeedGlow): `CGWorldFrame::UpdateScreenEffectParams`
  reads the glow amount through `VolumetricFog::GlowAmount`, which while vfog draws above water returns
  `lerp(client, look.glow, saturate((coverage - 0.5) / 0.5))` (the authored glow already blended in ResolveFogData);
  the glow compensation uses the delivered amount (coa UseDeliveredGlow). Natively the float is replaced before the
  byte is packed, so there is no glow-byte RE and nothing to restore. Camera position is last frame's.
- **Fading doodads** (vfog8, `vfogFadeEarly 1`): coa has no such step. This is wxl-vfog 0.6.0's DrawFadingModelsBeforeFog,
  to the RE spec in `/root/RE/render-path/doodad-fade-frame.md` 5b-5d and `doodad-fade-route.md`. A model with
  `CM2Model::m_alpha < 0.99999` has its batches routed by the client from pass 0 to the blended pass 1, which draws after
  the early composite. Before this change those batches got only the fitted fog, and their depth was missing from the
  composite. Now every type-0 element in the pass-1 list whose base material (`m_materialIndex - m_materialLayer`)
  has `m_blendMode <= 1`, and whose material lacks flag 0x10 (no depth write), is drawn in `OnLiquidDone` before the
  composite. The ZFill clones are included. The composite then fogs these batches at their own depth, as it does
  once the model is solid. The step works the same on every backend, because it only reorders stock draws.
  **Alpha cross-fade** (`vfogFadeStencil 1`, wxl-vfog 0.6.3 KeepDepthBehind / MarkLateDepth / DrawOverMarked): before
  the early batches draw, the scene depth is copied into an R32F target the size of the depth texture (**VFogDepthCopy**,
  `KeepDepthBehindFading`) and the stencil is cleared; the batches then draw with stencil ALWAYS / REPLACE,
  ref = `M2Element::m_alpha` * 255 (one `CM2SceneRender::Draw` per run of equal alpha). RenderFog copies the depth again
  (with the models) and marches, filters and composites from the depth *behind* them; then, over a scene copy taken
  before the composite, the composite at the live depth goes over the marked pixels by their alpha in seven steps
  (stencil EQUAL `step << 5` under read mask 0xE0, `GxBlend_ConstantAlpha` with the Unk84 blend factor (2 step + 1) / 16,
  1 at step 7; step 0 left as is). So a model takes none of its own fog at first sight and all of it once solid.
  **With MSAA (vfog16; wxl-vfog turns it off there)**: the stencil is split between the two users, fade alpha in the
  top three bits (written with write mask 0xE0, read with 0xE0), the sample split in the low five (near 1, far 0x1F,
  written and read with 0x1F). The marks are written per sample, as the stencil is. The fading pass clears the whole
  stencil and writes only 0xE0, so the low bits are 0 already: the split skips its clear on such frames and keeps the
  marks. The depth behind the models is `DeviceCopySceneDepth`, the single-sample depth every vfog pass reads (sample 0:
  D3D9 RESZ, D3D11 `blit_depth_ms_ps`, Vulkan's SAMPLE_ZERO depth resolve), so it is the same depth the split's
  silhouette and composites use. The split runs on the depth behind the models (the frame is fogged from behind them),
  so a model's edge samples get the background's fog first. The overlay then uses the split's near-side shader (c80 = 0,
  nothing clipped), which fogs a marked sample at the nearest depth of its 3x3, which is the model's own depth when
  sample 0 is the background behind its edge. With the plain composite those edge samples would take the background's
  fog (a halo). Deviation: a fading model's samples behind a nearer edge (a solid object in front of it) take that
  object's depth in the overlay. Cost under MSAA: nothing beyond the non-MSAA cross-fade (the overlay's seven quads use
  a 9-tap shader in place of the 1-tap one).
  Off (the old path: the background seen through a fading model takes the model's fog, 5d) with no stencil bits, no
  blend-factor cap, no R32F target, the world drawn into a target, or the CVar 0. The world-into-a-target guard (from
  wxl, where another plugin can redirect the world) is never hit in this client: the world frame always draws to the
  back buffer (only portraits and the shadow query set targets, and neither runs vfog), and `CopySceneBeforeFog` and
  the stencil both belong to the back buffer, so it stays. Cost when fading models are in view: two full-size R32F depth copies, a scene copy, seven stencil-tested
  composite quads.
  It only runs with `vfogTransparent 1`. With 0, one composite after the world covers the models anyway.
- **God rays** (vfog4): see section 2a. On by default (`vfogGodRays 1`, coa's "on" strength); coa ships `GodRays=0`.
- **Water** (vfog5): see section 2b. On by default, as coa (`Water=1`). Ripples and wakes (vfog7): section 2b, ripples.
- **Colour grading** (vfog6): see section 2c. Off by default, as coa (`ColorGrading=0`).
- **Half-float fog targets** (vfog10): the march, history[2] and rays[2] are `GxTex_Abgr16161616F` (D3D9/9Ex
  D3DFMT_A16B16G16R16F where CheckDeviceFormat passes RENDERTARGET | QUERY_FILTER, D3D11 R16G16B16A16_FLOAT, Vulkan
  R16G16B16A16_SFLOAT where it is a sampled colour attachment; `CGxCaps::m_rttAbgr16161616F`, in the spare slot at
  0x114 because wxl pins the caps and device layout), else Argb8888; a failed half-float create drops to 8 bit
  for the session. The 8-bit low-res fog banded once it was upsampled bilinearly. `vfogHdrTargets 0` forces 8 bit.
  The march and temporal outputs are saturated (`DitheredStore`), so float storage keeps the 0..1 range the 8-bit
  store clamped to. History depth (16-bit viewZ packed in two bytes) and the scene copy (of the 8-bit back buffer)
  stay Argb8888. Extra memory: 4 bytes per low-res texel x 3 plus 4 per ray texel x 2, about 7 MB at 1080p quality 2.
- **Dither** (vfog6, strength since vfog10): `vfogDither` 0 = off, 1 = +-1/255 triangular dither (interleaved
  gradient noise from the pixel position, ALU only, no fetch; vf_common `DitheredBy`), 2 = +-2/255. c79.x is the
  strength on the 8-bit outputs (composite, grade, water onto the back buffer), c79.z on the march and temporal stores:
  the strength with 8-bit targets, 0 with half-float ones. Against fog banding. Each pass has its own offset; the frame index animates it so the
  temporal blend averages it instead of accumulating a fixed bias (h = c + d/(1-w)). The composite dithers the colour it
  writes, i.e. after the conversion back to display space in LinearOverSceneCopy. coa's own dither was removed earlier
  as an fps regression that was **never isolated** (wxl-vfog/wxl-light composites, 2026-10-10), so it is a CVar:
  `vfogDither 0` skips it on a uniform branch (c79.x = c79.z = 0), the old path.
- **MSAA** (vfog9): see section 3, scene depth per backend; coa's sample split (`vfogMsaaSplit`, all three backends since vfog14): section 3, MSAA.
- **Not ported:** the GPU density probe and the log (diagnostics only), the overlay (CVars), wxl's TopUp, coa's
  `Multisampling` setting + depth-copy self-test, NVAPI, FarClipMax (Ascension's cap; the decomp has none). See
  /root/d3d11-port/vfog-gap-audit.md.

## 2. Passes (RenderFog, WP6)

These follow coa-vfog renderer.cpp:1265-1525 (RenderPasses).

1. `CGxTex* depth = g_theGxDevicePtr->DeviceCopySceneDepth()` takes a snapshot of the scene depth after the liquid.
2. **March**: VFogMarch{1,2,3}[L][N] -> `march` at low res (vp/4 for Q1, vp/2 for Q2-3). Premultiplied
   (in-scatter, opacity).
3. **Temporal** (Temporal > 0): VFogTemporal(march, history[r], depth, historyDepth[r]) -> history[w],
   then VFogHistoryDepth -> historyDepth[w].
4. **Composite**: VFogComposite{1,2,3}[L][N] at full res over the back buffer. It uses the depth-aware
   upsample, with a full-res march fallback on silhouettes, scissored to the world viewport.

Every pass draws one full-screen quad through the fixed-function VS. The quad uses `FFX::SetupGeo_Quad` /
`FFX::s_quadBatch` conventions and passes **TEXCOORD0 = target uv**. The shader turns uv into a pixel
position instead of using VPOS, because the Vulkan emitter has no vPos.

Blend: with ColorSpace=1 the composite is coa's default LinearOverSceneCopy: `GxTexCopy` copies the world
viewport of the back buffer (every backend: D3D9 StretchRect, D3D11 resolved back buffer, Vulkan CopyFromScene)
to s3, and the composite draws opaque, blending in linear light with glow compensation (c98.w). If the copy fails
or the world is not on the back buffer, the fallback: the Gx API has no (ONE, INVSRCALPHA), so the composite
outputs `float4(fog.rgb / max(a, 1e-4), a)` and draws with `GxBlend_Alpha` (SRCALPHA, INVSRCALPHA). The stock-fog
fits follow the blend actually drawn (`CompositeOverSceneCopy()`). On D3D9 the INTZ depth-stencil is unbound
around the composite (`DeviceSceneDepthBound`). `GxRs_ColorWrite` is
RGB only, and depth test and write are off. Linear mode (ColorSpace=1) does the gamma math in the shader
as coa does. Use the blend-over-scene-copy path only if the premultiplied form shows banding.

## 2a. God rays (coa DrawGodRayMask / DrawGodRaysOverScene, renderer.cpp:1189-1260)

Drawn when `vfogGodRays * lightVisibility * sunScreenFade > 0.005` (coa :1344): lightVisibility is the model's
sun/moon elevation fade (moon scaled, interior-weighted), sunScreenFade = clamp(1.6 - max|ndc|, 0, 1) of the
DayNight light direction projected through the main projection. Needs the back buffer (no rays when the world
draws into a target) and the three shaders; without them the fog still draws. Under water: off with the fog.

1. In RenderFog, after the composite: **VFogRayMask** into rays[1] (world viewport / 4, bilinear, clamped):
   bright sky (depth > c99.y) around the sun, `pow(dot(view, toLight), 8)`, luma threshold 0.12 x 4 ramp. It
   reads the pre-fog scene copy (taken whenever rays draw, even if the fog blend does not use it), bilinear at
   the ray texel centre = coa's linear StretchRect into the quarter-res target.
2. **VFogRayBlur** twice: rays[1] -> rays[0] at step 0.075, rays[0] -> rays[1] at 0.075/8; 8 taps toward the
   sun uv, decay 0.9, normalised by the tap weight sum.
3. In OnWorldDone (both vfogTransparent modes, before FFX glow): **VFogRayComposite** over a fresh copy of
   the finished world viewport, opaque, RGB: `displayed += (1 - displayed) * (1 - exp(-rays * rayColor * strength))`
   with the client glow compensation (c98.w). rayColor = sun colour (moon: direct), or the authored halo hue.

Deviations from coa: coa folds the rays into the fog composite when that composite is the last pass
(TransparentFog=0); here they are always a separate pass after the world, which is coa's own TransparentFog=1
path. The difference is one 8-bit round trip of the fogged scene. coa draws the mask and blur at the end of
the world in its early mode; here they are drawn at composite time from the same inputs (pre-fog copy, depth,
sun), so the rays are the same. Registers: mask c9 (toLight view, falloff), c10 (0, threshold, 1/ray size);
blur c9 (sun uv, step, 1/norm); composite c96.y strength, c97 colour, c98.w glow; samplers mask s0 depth,
s1 scene; blur s0; composite s2 rays, s3 scene.

History invalidation follows coa renderer.cpp:1323-1332: settings, map, screen-effect slot or
projection/viewport change; forward dot < 0.707; a frame gap over 0.25 s; a move over 30 yd. Key it on the
view matrix, never on a frame counter, because water reflection re-runs world passes (wxl lesson).

## 2b. Water (coa water_renderer.cpp, water_data.cpp, water_spectrum.cpp, shaders/vw_*)

coa's modern water: lakes, rivers, the sea and WMO indoor pools shaded like the modern client (refraction of the
pre-water scene, Beer-Lambert transmittance, in-scattering, wave/shore/depth foam with the Forever masks, GGX sun
glint, EnvBRDF reflection with a screen-space march and a fogged sky fallback, shore fade), per-class Forever presets
from `VFog\waterdata.bin`, Forever FFT waves. Code: `VolumetricFogWater.cpp` (data, classification, CVars, passes,
hooks), `VolumetricFogWaterWaves.cpp` (waves), `VolumetricFogWater.h` (private header; it also declares the full-screen
pass helpers VolumetricFogRenderer.cpp now shares: SetPs, Bind, DrawQuad, LiveWorldViewport, Begin/EndPasses).

Draws only while vfog is active (above water, `WaterFrame()`), `vfogWater` is 1, waterdata.bin loaded, the R32F
render-target format is available and the five shaders are in the MPQ. Anything missing: the client's own water.
Under water both the fog and the water are off (coa Underwater=0: coa has no underwater fog by default, and
its water is skipped with the camera in liquid), so the underwater view keeps the stock fog and the stock water.

**Classification** (coa water_classify.h): the liquid type of the instance's `CMaterialSettings` (its index in
`CSettingsBank::s_settings`), LiquidType material 1 (water) and texture 0 under `\river\` or `\ocean\`: type 17 =
interior, sound bank 1 = ocean, type 9 = river, else lake. A class waterdata.bin has no preset for stays stock.
Magma (material 2) and slime (3) are never classified, so they keep the client's look.

**Stencil scheme (`vfogWaterStencil 1`, default, coa).** In `Liquid::CRenderer::Flush` (transparent pass; coa hooks the
same pass only): `WaterArm` sorts as in step 1 below; `Begin` (first water draw) takes the scene colour and depth copies
(step 2), clears the stencil (`GxBufferBit_Stencil`), pushes the render states and sets stencil ALWAYS / REPLACE
(fail and zfail KEEP); before each class's draws the ref is the class index (1..4), so the front surface's class wins
where the depth test passes. `WaterFlushDone` pops the states, takes one depth copy of the finished water
(VFogWaterDepth into the first mask target: positive view Z everywhere, so the shader's sign test always passes) and
shades every drawn class with stencil EQUAL its index (coa ShadeClasses). Cost: 1 colour copy + 2 depth copies + one
shading pass per class. Used when the scene depth has stencil bits; else, or with the CVar 0, the mask scheme:

**Mask scheme (no stencil).** coa tags each class into the stencil during the client's draws and shades after the
pass. Here:
1. `WaterArm`: after the stock qsort, a stable sort moves the water to the end grouped by class (lake, river,
   ocean, interior). The stock comparator orders by material first, so cross-material order was already arbitrary;
   inside a class the stock order holds.
2. Before the first water draw (`Begin`): `GxTexCopy` of the world viewport (pre-water scene colour, point
   sampled) and a `DeviceCopySceneDepth` linearised to R32F view Z (**VFogWaterDepth**, = coa's scene depth copy).
3. After each class's draws (`ShadeClass`): another depth copy, **VFogWaterMask** writes view Z where the depth
   moved closer since the previous copy (that class drew the front surface: water writes depth while vfog draws,
   `LiquidDepthWrite`), else minus it. The previous copy ping-pongs between two R32F targets. Then **VFogWater{Q}**
   shades that class over the back buffer with `GxBlend_Alpha`, RGB writes: alpha 1 on the class's pixels, 0
   elsewhere. A later class drawn in front simply overwrites; water behind an earlier class fails the depth test
   and leaves no mark.
Cost per frame with water: 1 colour copy + (1 + classes) depth copies and linearisations + one shading pass per class.

**Waves on the GPU (2026-10-18, `vfogWaterGpuWaves` 1, default; the CPU path below is the fallback).** With
`GxTex_Abgr16161616F` targets and render-to-mip (`GxRenderTargetSet(GxBuffers_Color, tex, level)`) coa's GPU FFT runs
as Gx passes (`VolumetricFogWaterWavesGpu.cpp`, shaders `VFogWave{Evolve,Fft,Surface,Mip,Pack}` + `vw_wave_common.hlsli`),
as a speed change: same maths as the CPU path, same output maps, so `vw_surface.hlsli` / `VFogWater*` are unchanged.
Deviation from coa (RGBA32F there, MRT): Gx has RGBA16F only and no MRT, so
- every 16F texel that must carry float precision holds one value split coarse + residual (coa SplitForHalfStorage,
  signed: 9 significant bits exact in half + the remainder, ~2^-20 relative); an FFT texel is one complex
  (re hi, im hi, re lo, im lo), so heightAndX and alongY run as two chains;
- each butterfly stage halves (never grows past h0, no overflow) and h0 is prescaled n^2/256^2 * 2^-e on upload
  (e >= 0 keeps |texel| <= 16384, 0..4 for the Forever tiles at wind 10 / waves 2), the surface pass multiplies by 2^e:
  together the CPU's 1/256^2 at 128 and 256;
- h0 = CPU `Configure` output (`ComputeWaveSpectrum`), uploaded once per configuration with `GxTexUpdate` as two RGBA16F
  textures (coarse, residual); h0.w = the CPU's integer loop frequency mod 2048 (exact in half), the shader adds the
  multiple of 2048 nearest its own sqrt estimate, so a GPU sqrt an ulp off cannot flip the CPU's floor(x + 0.5)
  (a few hundred modes per tile sit within 2e-3 of that tie); twiddles in the shader (sincos of [-pi, pi));
- the moments mip chain keeps the central variance (mean of the children's variance + their means' spread) instead of
  E[s^2], so half storage never cancels E[s^2] - |E[s]|^2.
Per tile per frame (n = 256 / 128): 2 x (evolve+fold into bit-reversed order + 2 log2 n radix-2 stages, ping-pong over
3 shared RGBA16F n^2 targets) -> surface (moments level 0) + foam step (per-tile RGBA16F ping-pong, split storage, dt
clamped 0.1 s, cleared on reconfigure) -> log2 n box-filter passes into per-level scratch RGBA16F targets -> per level
one pack into the tile's final slopes (RGBA16F, .xy) and one into its state (Argb8888, the CPU encoding: R sqrt(var/0.25),
G sqrt(wide^2), B foam, A oxygen at level 0, 0 above) by render-to-mip (wrap, trilinear). 62 draws at 256, 54 at 128.
The step runs where the CPU launch was (`WaterFlushDone`, after the last class), inside BeginPasses/EndPasses with the
world target restored, every frame (the CPU updates at its worker rate). Fallback to the CPU path, sticky: no
`m_rttAbgr16161616F` (caps 0x114) or Argb8888 target cap, a VFogWave shader missing from the MPQ, or a target/texture
that fails to create; `vfogWaterGpuWaves 0` forces the CPU (the GPU textures are dropped).
**Check** (`python3 Tools/CustomMpq/check_gpu_waves.py`, numpy): the CPU path (float radix-2 replica of Step) against
the shader maths with a float16 store at every target write, all 7 waterdata.bin tiles, 128 and 256, defaults (wind 2,
waves 1) and the extremes (wind 10, waves 2), 12 steps of 70 ms at t = 3611 s (late loop phase, foam built). Measured:
slopes max 9.8e-4 (1 half ulp at |slope| 1..2; 2.4e-4 at the defaults), RMS 2e-6..6e-5; state R/G max 1 unit (an
8-bit rounding flip), RMS <= 0.035 (defaults) / 0.10 (extremes); foam/oxygen max 1 unit (defaults), 4 at the extremes,
RMS <= 0.04. Deviation from coa: FoamStep integrates df/dt = a (1 - f) - b f exactly over dt (f_eq + (f - f_eq)
e^-(a+b)dt, shader and CPU) instead of coa's explicit Euler step, which fell into a 2-cycle at saturation once
dt (a + b) > 2 (oxygen 200/s at 70 ms: 1 <-> 0.951, crest texels shimmering); the check needs no exemption since
(2026-10-18 audit M9: foam/oxygen max 1 unit at every setting).
Without the coarse/residual split (plain 16F FFT texels) the same check gave slopes 1.7e-2 and oxygen 40 units off.
**Expected savings**: the CPU path costs ~10 ms of worker time per 256 step per tile (threads, so the waves update at
~50-100 Hz) plus, per update, `GxTexUpdate` of 2 x 256^2 texels x 4 B with mips (~0.7 MB) per tile on the main thread.
GPU: ~2.6 M pixels per 256 tile per frame (34 n^2 FFT passes, 2-4 point fetches of 8 B each), ~80 MB of traffic:
~0.2-0.3 ms per tile on a 300-500 GB/s card, so ~0.3-1 ms for the 1-4 drawn tiles, no CPU work besides 62 draws per
tile, no uploads after configuration, and the waves advance every frame. (Estimate, not measured in game.)

**Waves on the CPU (fallback; the original deviation).** The Gx API had no RGBA16F/32F targets and no render-to-mip, so coa's GPU FFT runs on
the CPU instead: coa's own CPU reference path (water_spectrum.cpp: WaterReferenceH0/Modes/FoldModes/InverseTransform/
AssembleDisplacement/Surface) in float, same spectra, Forever tile parameters, 1024 s loop clock, 1/256^2
normalisation, surface stencil, Jacobian foam/oxygen persistence (FoamStep, dt clamped to 0.1 s) and box-filtered
mips. Resolution as coa: 128 at vfogWaterQuality 1, 256 above. Each drawn tile is one thread; a step is launched
after the frame's last water class and collected at the next frame's first (never waited on: if a step is still
running the previous maps stay, so the waves update at the worker's rate, about 10 ms per 256 step here).
Per tile two textures, wrap, trilinear, every mip down to 1x1, uploaded with `GxTexUpdate`:
- slopes, `GxTex_Gr1616F`: mean slope (coa moments.xy);
- state, `GxTex_Argb8888`: R sqrt(variance / 0.25), G sqrt(wide slope^2 / 1), B foam, A oxygen.
The shader rebuilds coa's moments: z = variance + |filtered slope|^2, w = wide^2 (vw_surface SampleMoments); storing
the variance rather than E[s^2] keeps 8 bits from swamping a mirror-flat surface's zero variance. Foam and oxygen are
read at level 0 as coa's foam maps. Measured ranges (waves 2): variance < 0.06, wide^2 < 0.16.

**Registers / samplers** (vw_constants.hlsli = coa's): c0-c8 vf_common (c3.z = 65536, coa kWaterMaxViewDepth),
c9-c67 coa ShadingConstants (light, sun, ambient, isotropic, sky x5, stock fog (always off: vfog pushes it out),
absorption, scattering x3, foams x4, surface response, inverse tile sizes, wave control, scrolls, depth decode, mask
tints x10, reflection fog 4 layers x 5 + range, ripples c65-c67); the depth passes also read c99. Samplers: s0 scene
colour, s1 scene depth (R32F), s2 signed water depth (R32F), s3-s6 tile slopes, s7-s10 tile state, s11 packed wave
foam masks (R/G/B = high/mid/low), s12 ripple map (Gr1616F, linear, clamp), s14 shore mask, s15 depth mask (Argb8888, full mip chains from waterdata.bin).
Missing maps bind an 8x8 zero texture (coa m_flat).

**Shaders**: VFogWaterDepth, VFogWaterMask (MASK 0/1 of VFogWaterDepth.hlsl), VFogWater1/2/3 = coa
shade_low/mid/high (SSR_STEPS 0/16/32); variant = quality - 1, or 1 (sky only) when vfogWaterReflections is 0.
The Vulkan translator gained `texldd` (tex2Dgrad -> textureGrad).

**Deviations from coa**: 16F GPU waves with split storage, or CPU waves (above); scene copies clamp instead of mirror addressing (the Gx API has no
mirror); no packed-RGBA8 depth fallback (needs R32F targets); MSAA water is shaded from one depth sample per pixel (with the
stencil scheme the class tags and the shading test are per sample, as coa; with the mask scheme the mask is per pixel);
classes are shaded one after another inside the liquid pass instead of all after it (same pixels, same inputs:
every class reads the same pre-water copy, as coa); water is drawn after the other transparent liquids; the
scene copy is taken before the first water draw rather than before the whole liquid pass (it includes slime);
no water GPU timer/summary log.

**Ripples and wakes** (vfog7; coa water_ripples.cpp, water_contacts.cpp, engine_actors.cpp, client_ripple_sprites.cpp,
vw_ripple_step.hlsl; code `VolumetricFogWaterRipples.cpp`). Same model, constants and defaults as coa:
- *Contacts*: once per frame with water drawn (`Begin`), the visible object list (`ObjectMgr::m_visibleObjects`, the
  list coa walks) gives the units whose map entity found liquid crossing the body (`CMapStaticEntity::m_staticFlags`
  0x20 and 0x100, `m_liquidHeight` the surface; the flags the client's own `CGUnit_C` ripple emitter reads): position
  (the entity's when on a transport), `CMovement_C` collision radius/height, swimming flag 0x200000. The 32 nearest
  within 48 yd of the camera target, deeper than 2x height excluded. coa's guard bytes and SEH reads are not needed.
- *Tracker*: coa's verbatim (held footprints along the path, wake motion ramp, entry splash with re-arm and 2 s
  interval, 0.5 s stale tracks, 30 s departed-entry memory).
- *Simulation*: **GPU, as coa** (Gr1616F render targets exist on all three backends, so no CPU path): VFogRippleStep
  ping-pongs two Gr1616F maps (r height, g previous height), 0.125 yd texels, 512^2 (256^2 at vfogWaterQuality 1)
  centred on the camera target, 30 steps/s, at most 4 per frame (the rest dropped), up to 48 disturbances in c1-c96.
  Stops after 450 quiet steps (15 s, maps kept), restarts after a > 1 s gap, a map change or a clock going back.
  The shading interpolates current/previous by the step phase (c66.x), as coa.
- *Client sprites* (coa WaterClientSplashes=0): coa holds the `waterRipples` console value at 0; natively
  `CMap::CreateWaterRipple` skips new sprites while `VolumetricFog::WaterHidesClientRipples()` (vfogWater on,
  vfogWaterRipples > 0, vfogWaterClientSplashes 0, ripples supported, modern water drawn within the last second, coa's
  grace). Existing sprites expire as with coa, and `waterRipples` itself is never written.
- *Cost*: nothing without water in view (it runs from the water pass); with water, one walk of the visible objects
  (flag tests only); the GPU step (512^2, 5 taps + the used disturbances) only while a unit is in water or the ripples
  are still settling, 30 per second, roughly 0.05 ms each on a mid GPU (estimate, not measured); two 1 MB render
  targets, made on first contact, dropped when vfogWaterRipples is 0.
- *Deviations*: the step reads TEXCOORD0 instead of VPOS; clearing is a step drawn with c97.x = 0 (the Gx clear depends
  on the viewport per backend); the map is created linear and read by the step at texel centres only (coa point-samples
  it there); the clock is FrameTime (coa QPC); no ripple stats/summary log. The output is a float target, so vfogDither
  does not apply.


## 2c. Colour grading (coa grading_renderer.cpp, forever_look.cpp PrepareGrading, shaders/vp_grade.hlsl)

The modern client's per-light colour curve (32 entries, fogdata.bin's grading curves: brighter midtones, the
brightest highlights clipped), blended as coa: per light, the keys around the time of day (InterpolateGrading), the
lights by their weight, identity where no key carries a curve (GradingBlend::Result). Strength =
`vfogGradeStrength * clamp((coverage - 0.5) / 0.5, 0, 1)` (coa ColorGrading x ForeverLookWeight).

Placement as coa: decided in `OnWorldDone` (world viewport = the live one there), drawn in `OnWorldRendered`, i.e.
after god rays, the whole world and the FFX glow (`FFX::Finish`), before names, icons (`WorldTextRenderBatch`) and the
UI, so the interface is never graded. **VFogGrade** copies the world viewport of the back buffer (`GxTexCopy`, the
shared scene copy) and draws `lerp(scene, curve(scene), strength)` over it opaque, RGB only, scissored. Skipped
(as coa): vfogGrade 0, the ghost view (`FFX::s_effect == ffxEffectDeath`), the camera under water, no light with a
curve, coverage <= 0.5, the world not on the back buffer.

Deviations from coa: the curve is 8 pixel constants (c11-c18, c10.x strength, c0 viewport, c79 dither) instead of a
32x1 R32F texture; the shader's hat-weight sum is exactly coa's lerp between neighbouring entries and needs no float
texture on three backends. Grading needs vfog active (vfog 1, above water) because the camera inputs come from the fog
frame; coa grades with its fog off too. The output is dithered with vfogDither (coa has none).

## 3. Render targets

All are created with `TexCreate` and the renderTarget flag, like `FFX::CreateTex`, and recreated on a
viewport size change or device restore.

| Target | Size | Format | Notes |
|---|---|---|---|
| march | low | `GxTex_Abgr16161616F` (vfogHdrTargets, caps), else `GxTex_Argb8888` | as coa (A16B16G16R16F, Argb8888 fallback) |
| rays[2] | viewport / 4 | as march, bilinear | god rays (section 2a), created on first use |
| scene copy | world viewport | `GxTex_Argb8888`, bilinear | pre-fog copy (s3, ray mask), reused for the post-world ray composite |
| history[2] | low | as march | ping-pong |
| historyDepth[2] | low | `GxTex_Argb8888` | packed 16-bit viewZ + depth class (vf_history_depth) |
| scene depth | full | backend-owned | `DeviceCopySceneDepth()` |
| noise atlases | 272x136, 528x528 | `GxTex_Argb8888`, wrap | static, from WP1 |

The 32x1 RGBA32F point-light texture and the 1x25 probe are dropped. Lights go into pixel constants.

### Scene depth per backend (`CGxDevice::DeviceCopySceneDepth`, added in scaffolding)

- **Vulkan (WP3)**: add `TRANSFER_SRC` usage to depth images (Vulkan/Frame/Targets.cpp:185). Add
  `frame::CopyDepthFromScene` modelled on `CopyFromScene` (Readback.cpp:35): end the pass, transition,
  `vkCmdCopyImage` of the DEPTH aspect into a sampled depth image of the same format with a depth-only
  view, transition back, then expose it as a CGxTex that `ITexResolve` can bind. Never sample the live depth,
  because `DepthFor()` shares one depth image per size.
  The copy is in the frame's own depth format, so D16/D32 need nothing there.
  **MSAA (vfog11)**: `gxMultisample` 2/4/8 (clamped to `framebufferColorSampleCounts & framebufferDepthSampleCounts`)
  gives the *scene* a multisampled colour and depth (`s.msColour`/`s.msDepth`, made and dropped with the scene, so every
  format change and resize remakes them). Its passes are `vkCreateRenderPass2` passes that resolve at the end of every
  pass instance: colour into the scene image, depth (`VK_RESOLVE_MODE_SAMPLE_ZERO_BIT`, core 1.2
  depth_stencil_resolve) into the single-sample depth buffer of the scene's size. So `CopyDepthFromScene`,
  `CopyFromScene` (water, the composite's scene copy, glow), the screenshot read-back and the present pass are
  unchanged and read sample 0 / resolved colour, as D3D11's `blit_depth_ms_ps` and `IResolvedBackBuffer`. No shader
  changed. Texture render targets stay single-sample; full-screen passes over the scene (vfog composite, glow
  blend-back, UI) rasterise into the multisampled scene, every sample alike. Pipelines key on the pass's sample count
  (`Key::samples`, `frame::PassSamples()`). Needs a Vulkan 1.2 device; below it the scene stays single-sampled (logged).
  Occlusion queries in the scene count samples, as on D3D. Cost: 1920x1080 at 4x, 33 MB colour + 33 MB depth
  (D24S8), 8x double; plus one full-screen colour+depth resolve per scene pass break.
- **D3D11 (WP5, vfog9)**: single-sampled D24: `CopyResource(m_depthTexture -> a GxTex_D24X8 CGxTex)`, which gets an
  R24_UNORM_X8 SRV, point sampled. MSAA or D16/D32 (CopyResource cannot change the sample count or format):
  `IBlit` draws `m_depthSrv` into a `GxTex_R32F` render target with a Load shader (`blit_depth_ps`, or
  `blit_depth_ms_ps`, ps_4_1, `Texture2DMS` sample 0; /root/d3d11-port/ffshaders blit.hlsl, FfShaders.h). MSAA needs
  feature level 10_1 for the multisampled SRV; below it, nullptr.
- **D3D9 (WP5, vfog9)**: an INTZ texture at back-buffer size bound as the world depth-stencil, as coa d3d9_wrap.cpp:959
  and :1002-1022 do. Check INTZ with CheckDeviceFormat. ShadowCache's own D24X8 targets must keep working. This covers
  both `CGxDeviceD3d` and `CGxDeviceD3d9Ex`. MSAA (coa msaa_depth.cpp): the multisampled auto depth stays bound, INTZ
  is only a copy target, and `DeviceCopySceneDepth` fills it by the driver's RESZ resolve (`IResolveSceneDepthResz`,
  coa ResolveThroughResz: INTZ on stage 0, a dummy point, `D3DRS_POINTSIZE = 0x7FA05000`, every touched state read
  back and restored). RESZ is checked at device create and each Reset (`CheckDeviceFormat(RENDERTARGET, SURFACE, 'RESZ')`): AMD,
  Intel, DXVK. No RESZ: nullptr, stock fog.

**MSAA, what is the same as coa and what is not.** As coa, the passes read one depth sample per pixel; the scene copies
are resolves (D3D9 StretchRect, D3D11 IResolvedBackBuffer); the composite and the water draw over the multisampled
back buffer. **Sample split** (`vfogMsaaSplit 1`, coa DrawCompositeBySampleDepth / MarkSilhouetteSamples, vf_sample_split): with a
multisampled depth the composite is drawn in four quads, the live multisampled depth-stencil bound throughout (on D3D9
MSAA INTZ is only the RESZ copy target, so nothing sampled is bound): (1) stencil cleared, **VFogSilhouette** with
depth test LESS, colour off, stencil ALWAYS ref 1, pass INVERT, zfail REPLACE: its oDepth is the middle depth of a
silhouette's 3x3 neighbourhood (of the single-sample copy; non-planar or two depth classes), so per sample the far
samples become 0xFF and the near ones 1, everything else stays 0 (clipped); (2) the plain composite, everywhere when it
overwrites (over the scene copy) else on stencil 0; (3)/(4) **VFogCompositeSplit{Q}[L][N]** on stencil 1 / 0xFF, side in
c80 (coa c99, the horizon band here), fogging each side at its own depth (own depth, nearest or farthest). oDepth is used as coa does. **Vulkan (vfog14)**: the
translator writes oDepth as `gl_FragDepth` (glslang adds `DepthReplacing`; a local `oDepth` starts at `gl_FragCoord.z`,
written out clamped to [0, 1] after the alpha test), so VFogSilhouette translates and the split arms as on D3D11. Nothing
else changed: the composite passes already draw inside the scene's multisampled pass, whose depth-stencil is LOADed and
STOREd (CreatePassMs), the stencil clear is `vkCmdClearAttachments` on that attachment, and the stencil test runs per
sample, so no sample shading, `gl_SampleID` or `sampler2DMS` is needed (the shaders read the sample-0 depth copy, as
D3D11). The stencil state, colour mask and sample count are in the pipeline key already. Cost (as D3D): one stencil
clear and four full-screen quads in place of one (the silhouette pass is depth/stencil only), plus one pipeline per
split variant. Also off with no stencil bits, the world drawn into a target, or the CVar 0. It uses the low five
stencil bits (near 1, far 0x1F) so the fading-model alpha in 0xE0 survives it (section 1, alpha cross-fade).
Deviations: (1) the water is shaded from one depth sample per pixel (its class tags are per sample, section 2b). (2) No NVAPI (`NvAPI_D3D9_StretchRectEx`, coa's
path on native Windows NVIDIA D3D9, where there is no RESZ): there D3D9 MSAA keeps the stock fog; D3D11 works. (3) No
depth-copy self-test and no `Multisampling` setting: coa refuses MSAA unless its copy passes a test; here MSAA is never
touched, and a failed or missing copy only leaves vfog off.

### Stencil (Gx, commit ab0969f)

`GxRs_StencilTest/Func/Ref/ReadMask/WriteMask/Fail/ZFail/Pass` (EGxStencilCmp / EGxStencilOp = the D3D values, front
and back alike; defaults off, ALWAYS, 0, 0xFF, 0xFF, KEEP), `GxBufferBit_Stencil` in `GxSceneClear` (to 0; D3D11/Vulkan
the whole surface, D3D9 the viewport), `m_caps.m_stencilBits` (8 or 0). Implemented on Vulkan (pipeline key; D24S8 or
D32S8 depth), D3D11 and D3D9/9Ex. vfog's helpers: `StencilAvailable()`, `SetStencil(...)` (VolumetricFogWater.h);
`BeginPasses` turns the test off. Users: water tags (2b), fading-model alpha (1), MSAA sample split (above); each
clears the stencil itself, in order in a frame (water in the liquid pass, then fading, then the composite). Fading and
the split share it (bits: fading 0xE0, split 0x1F; the split skips its clear after a fading pass, section 1).

## 4. Depth decode (shader, vf_common)

coa used one model, `viewZ = c3.y / (d - c3.x)` from the GL projection and viewport MinZ/MaxZ. We keep
that model for the **main band** d <= c3.w (= 0.94) and add:
- **horizon band**: `c99 = {0.998046875, 0.9990234375, nearYd, farYd}` (FrameInputs::horizonBand).
  For `c99.x <= d <= c99.y`: `t = (d - c99.x)/(c99.y - c99.x)`, `viewZ = n*f/(f - t*(f-n))`, clamped to
  MaxFogDistance. This is wxl-vfog's InFarSlice retargeted to the decomp band.
- **sky**: d > c99.y (replaces 0.99903). 0.94 < d < 0.998 is empty in the decomp; treat it as horizon.

The horizon projection's xy differs from the main one (fov*0.6 vs SG fovy). For a horizon-band pixel, the
ray direction still comes from the main projection: the pixel is the same screen ray, and only its distance
changes.

## 5. Shaders (WP2) and register contract

The sources are `Tools/CustomMpq/ShaderSrc/VFog*.hlsl` plus `vf_*.hlsli` includes ported from coa. They build
to `Shaders/Pixel/ps_3_0/*.bls` (D3D9) and `ps_4_0` (D3D11, shader-xlate), and Vulkan SPIR-V comes through
`archive.py`. They are loaded with `ShaderCreate(&ps, GxSh_Pixel, "Shaders\\Pixel", "<Name>", 1)`.

| Name | Variants | coa source |
|---|---|---|
| VFogMarch{Q}{L?}{N?} | Q = 1,2,3 (STEPS 16/24/32), L = local lights, N = noise | vf_march.hlsl + vf_integrate.hlsli |
| VFogComposite{Q}{L?}{N?} | same | vf_composite.hlsl/.hlsli |
| VFogTemporal | 1 | vf_temporal.hlsl |
| VFogHistoryDepth | 1 | vf_history_depth.hlsl |
| VFogRayMask, VFogRayBlur, VFogRayComposite | 1 each | vf_ray_mask/blur/composite.hlsl (section 2a) |
| VFogWaterDepth, VFogWaterMask | 1 each | vw_linear_depth.hlsl (section 2b) |
| VFogGrade | 1 | vp_grade.hlsl (section 2c) |
| VFogWater{1,2,3} | SSR_STEPS 0/16/32 | vw_water.hlsl + vw_*.hlsli (section 2b) |
| VFogRippleStep | 1 | vw_ripple_step.hlsl (section 2b, ripples; c0-c97, s0) |

Names are built as `VFogMarch2`, `VFogMarch2L`, `VFogMarch2N` and `VFogMarch2LN`: 12 march + 12 composite +
2 = 26 shaders, plus the 3 god ray shaders. Unroll layer loops (`[unroll]` over the 4 layers), because vkd3d turns indexed constant
arrays into cmp chains. The step loop stays a `[loop]`.

Pixel registers are the same as coa (vf_common.hlsli, vf_integrate.hlsli, etc.) except where marked:

| Reg | Use |
|---|---|
| c0 | world viewport px (x, y, w, h) |
| c1 | (scale, frame mod 1024, 1/depthW, 1/depthH) |
| c2 | (proj00, proj11, proj20, proj21) |
| c3 | (depthAtInfinity, depthPerInvViewZ, maxFogDistance, deepestWorldDepth = 0.94) |
| c4-c7 | viewToWorld |
| c8 | low-res size / texel |
| c9 | toLight (view) + lightAboveHorizon |
| c9-c12 | temporal: cViewToPreviousClip; c13 cTemporal |
| c11 | (jitterOn, distanceCurveRange, horizonStart, farClip) |
| c12-c35 | 4 layers x 6 |
| c36-c47 | 3 noisy layers x 4 |
| c53 | local-light control |
| c78 | density variation |
| **c79** | **new**: vfogDither (x strength on 8-bit outputs, y frame, z strength on the fog stores); every VFog fog pass, VFogGrade, VFogWater |
| c96-c98 | composite, godray colour (w = 0, dither removed), sun |
| **c99** | **new**: horizon band (minZ, maxZ, near, far) |
| **c100-c131** | **new**: 8 local lights x 4 (pos view + cutoff, colour, attenuation c/l/q, spare). Replaces sampler s8 |

| Sampler | Use |
|---|---|
| s0 | scene depth (point) |
| s1-s4 | composite: fog/history, god rays (VFogRayComposite only), scene copy, current march. Temporal: current, history, depth, historyDepth |
| s9 | density noise atlas (bilinear, wrap) |
| s10 | authored noise atlas (bilinear, wrap) |

### Noise atlases

The Gx API has no volume textures, and adding them to three backends costs more than this. Each Z slice
goes into a 2D grid tile with a 1-texel wrap border, so bilinear filtering inside a tile is exact. The shader
does the slice lerp. The result equals trilinear filtering with wrap addressing.

- density: 32^3 in an 8x4 grid of 34x34 tiles, 272x136.
- authored: 64^3 in an 8x8 grid of 66x66 tiles, 528x528, with the value in every channel.

Shared helper `float4 SampleNoiseAtlas(sampler2D s, float3 p, float n /*32|64*/, float cols, float tile)`:
`p` is in volume texels and wraps on all axes. The CPU layout (WP1) and the shader helper (WP2) must
match this exactly.

### Vulkan translator (WP4)

What it needs: per-record pixel-constant rows `pconst` (stock records stay at 16, so their SPIR-V does not
change; ours need 132), `rep/endrep`, `loop/endloop` if vkd3d emits them, `break/breakc`, `texldl`, and any
other opcode the VFog shaders use. No vPos or volume samplers are needed, by design; oDepth only by
VFogSilhouette (MSAA split): `gl_FragDepth` since vfog14 (section 3).

## 6. Data and custom.MPQ

- `Tools/CustomMpq/VFog/fogdata.bin`: coa data/fogdata.bin, VFD1 v4, 755960 bytes, byte-identical to
  wxl-vfog's copy. It is packed as `VFog\fogdata.bin` (WP2 adds `folder_files('VFog', ...)`) and loaded by
  WP1 with `SFileOpenFile`/`SFileReadFile`. If it is missing or invalid, the derived layers are used, as coa does.
- `Tools/CustomMpq/VFog/waterdata.bin`: coa data/waterdata.bin (VWD1 v1, 8390050 bytes: 4 Forever presets, 7 FFT
  tiles, 6 foam masks 1024^2 with full mips), packed as `VFog\waterdata.bin`, read by VolumetricFogWater.cpp on the
  first frame with water. Missing or invalid: the client's own water.
- `Shaders\Pixel\ps_3_0|ps_4_0\VFog*.bls`: WP2. The outputs are committed, as FarBlend's were.

## 7. Per-frame parameters (shared with the stock fog)

`OnWorldUpdate` fills `FrameInputs` from `DayNight::g_dnInfo` **before** the push-out (fog-params notes):
- `fogColor/start/end` = `m_fogInterior`. Its colour already includes the interior blend, glow and eclipse.
- `zoneFogDistance` = `m_bands.m_fogEnd`. `farClip` = `World::s_farClip`.
- `ambient` = `m_bands +0`, `direct` = `+4`, `sun` = `m_sunCloudColors[0]`. Port these by offset, do not renumber bands.
- `dayFraction` = `m_dayProgression`, `stormBlend` = `m_stormBlend`, `screenEffectSlot` = `m_overrideLightParamsId`.
- the light direction: the visible sun or moon from `DayNight::g_planets`. `interiorBlend` = `m_interiorFogBlend`.
- `mapId`: CWorld's current map id (not `s_simpleMapID`).

`OnOpaqueDone` adds the camera-relative view and the GL projection (`CWorldScene::viewMatrix/projMatrix`),
the viewport (`GxXformViewport`), the horizon band and its near/far, and point lights from the M2 scene
light hash (coa engine_lights.cpp; scene + 0x24, 64x64 cells; real CM2Light fields).

The distance layer (fog_model.cpp:201-226) takes the unstretched stock end. There is no stretch any more, so
coa's `farLimit = clamp(fogEnd, 100, farClip)` holds as written. vfog reaches full density at farclip and
the horizon band beyond it is fogged solid, which matches the stock look. If far terrain ever comes back,
change `farLimit` to `horizonFar` in this one place.

### Where future light and colour settings hook in

- Zone and band colours: at the end of `DayNight::SetColors`, before `SetLightColors()`. Sky, lighting, stock
  fog and vfog all follow.
- Fog distance or tint for everything: `DayNight::SetOverrideFog` / `ClearOverrideFog`.
- vfog's own look (density, haze, scatter, ambient, exposure): `Config`, which comes from the `vfog*` CVars.
  Never write it into DNInfo.

## 8. CVars (WP7, registered from `CWorldParam::Initialize`)

`vfog` (1, master), `vfogQuality` (2), `vfogTemporal` (0.85), `vfogDensity` (1), `vfogHaze` (1),
`vfogGroundFog` (0.6), `vfogFarFog` (1), `vfogAmbient` (1.75), `vfogData` (1), `vfogLocalLights` (1),
`vfogTransparent` (1), `vfogFadeEarly` (1; 0 = fading M2s stay in pass 1 with the fitted fog), `vfogFadeStencil` (1, wxl-vfog
0.6.3 alpha cross-fade; 0 = they take their own fog at any alpha), `vfogMsaaSplit` (1, coa sample split with MSAA; 0 =
sample 0's fog), `vfogStartOffset` (10), `vfogMaxDistance` (5000), `vfogNoise` (0.15), `vfogDebug` (0),
`vfogGodRays` (1 = on, the user's choice; coa GodRays, a 0..4 strength, 0 = off, coa ships 0),
`vfogDither` (1 = +-1/255, 2 = +-2/255, 0 = the undithered path), `vfogHdrTargets` (1; 0 = 8-bit fog targets), grading: `vfogGrade` (0 = off, coa ColorGrading=0), `vfogGradeStrength`
(1, 0..1, coa ColorGrading's strength when on).
Water (coa CoAVolFog.ini Water* defaults and ranges): `vfogWater` (1), `vfogWaterQuality` (2, 1..3),
`vfogWaterWaves` (1, 0..2), `vfogWaterWind` (2, 0.5..10), `vfogWaterFoam` (1, 0..2), `vfogWaterReflections` (1, 0..2),
`vfogWaterSpecular` (1, 0..4), `vfogWaterClarity` (1, 0.25..4), `vfogWaterZoneColors` (0.5, 0..1), 
`vfogWaterRipples` (0.5, 0..2; 0 = off: no simulation and no reading of the units; coa WaterRipples),
`vfogWaterGpuWaves` (1 = the GPU waves, 0 = the CPU path; new), `vfogWaterStencil` (1 = coa's stencil class tags, 0 = the depth-difference mask; either way the mask without
stencil bits), `vfogWaterClientSplashes` (0 = the client's splash discs and wake trails hidden while the ripples run, 1 = kept; coa
WaterClientSplashes), `vfogWaterDebug` (0; 1 normal, 2 foam, 3 transmittance, 4 reflection, 5 class, 6 ripples).
The defaults are coa CoAVolFog.ini plus the user's wxl-vfog look: Ambient 1.75, ClassicExposure 0.5,
LocalLightIntensity 2.5, StartOffset 10. FarFog is 1, not wxl's 0, because no far-field plugin owns far fog
now. Dither: see section 1 (vfogDither; the old fps regression was never isolated, so it can be toggled).
vfog13, every remaining coa key (coa ranges): `vfogSunScatter` (1, 0..10), `vfogExposure` (1, 0..10),
`vfogClassicExposure` (0.5, 0..10), `vfogClassicPhase` (0), `vfogColorSpace` (1; 0 = gamma), `vfogLocalLightIntensity`
(2.5, 0..8), `vfogLocalLightPhase` (0.3, -0.9..0.9), `vfogInterior` (1, coa InteriorAware), `vfogInteriorDensity`
(0.15), `vfogInteriorFarFog` (1, coa PR #9: the distance layer x lerp(1, f, interior weight)), `vfogNoiseScale`
(0.025, 0.001..1), `vfogNoiseWind` (0.5, 0..10), `vfogClassicNoise` (1), `vfogGlowCompensation` (1), `vfogUnderwater`
(0), `vfogStockFog` (1), `vfogForeverGlow` (0), `vfogSunMarker` (0, debug), `vfogLiquidDepth` (1; modern water forces
the depth writes anyway, `WaterArmed`).

## 9. Perf budget (1440p, mid GPU)

| Part | Budget |
|---|---|
| depth copy | <= 0.1 ms |
| march, Q2 half-res 24 steps | <= 1.0 ms |
| temporal + history depth | <= 0.2 ms |
| composite | <= 0.3 ms |
| CPU model + fit | <= 0.2 ms |

Keep coa's and wxl's speedups: lit march scissored to the light spheres, per-ray layer terms hoisted
(LayerRayTerms), an early-out before the nearest layer start (wxl ec1c66b). If the composite is over budget,
check the full-res fallback rate first.

## 10. Work packages

All packages run in parallel, and no two own the same file. None of them builds, publishes or pushes.
`VolumetricFog.h` is frozen; anything private goes into a package's own header.

| WP | Title | Owns | Depends on |
|---|---|---|---|
| WP1 | CPU fog model, data, noise, fit | VolumetricFogModel/Data/Noise/Fit.cpp, VolumetricFogModel.h | - |
| WP2 | Shaders + MPQ pipeline | Tools/CustomMpq/ShaderSrc/**, Tools/CustomMpq/Shaders/**, build_custom_mpq.py, bls/archive.py | - |
| WP3 | Vulkan scene depth | Vulkan device + Frame files | - |
| WP4 | Vulkan shader translator | bls/emitter.py, translate.py, manifest, Pipelines.hpp, Regs.cpp | - |
| WP5 | D3D11 + D3D9 scene depth | CGxDeviceD3d11.*, CGxDeviceD3d.*, CGxDeviceD3d9Ex.* | - |
| WP6 | GPU renderer | VolumetricFogRenderer.cpp (+ private .h) | WP2 names/registers (contract above) |
| WP7 | Driver + client integration | VolumetricFog.cpp, WorldFrame.cpp, World.cpp, WorldParam_CVar.cpp, MaterialWater.cpp, CM2SceneRender.cpp | contract |

Merge order: WP3/4/5 -> WP2 -> WP1 -> WP6 -> WP7. Then build once, and the user tests in game.

## 2026-10-18: ground spell decals (benilla port, branch `decals`)

benilla (`benilla-world/src/ground_fx.rs`, `decal.rs`; detector `GroundQuad::ground_quad_hover` in
`benilla-formats/src/models/types.rs`) redraws flat spell ground quads (Consecration, the paladin aura rings,
Blizzard/Flamestrike floors) as decals clipped onto terrain and WMO faces, instead of the flat M2 planes that
hills bury and dips leave floating.

The 3.3.5a client already has that projector: an M2 batch with flag 4 becomes element type 1, and
`CM2SceneRender::DrawBatchProj` (0x00820720) builds a world box and eye-linear tex matrix from the quad
(`CM2Model::GetRegionBounds`), then `World::ProjectTextureCallback` → `ProjectTex2dDrawShaderEffect` redraws
the terrain/WMO triangles in the box (`World::GetTris`, flags 0x200122, the same set as benilla's ring) with
`Projected_ModMod`/`Projected_ModAdd`. So no new shader and nothing per backend: D3D9, D3D11 and Vulkan all
run stock effects that already ship.

- `CM2Scene` (M2Scene.cpp `IsGroundDecalBatch`): a non-flag-4 batch is sent down that path when
  `groundDecals` is 1 and it is: a `Spells\` model; world-planted, or on the Base (19) or root attachment
  (benilla's ground anchor; chest-anchored quads such as ProtectionFrom* stay geometry); one texture, not
  env-mapped; 4 vertices on one non-billboarded bone at full weight; a flat axis-aligned XY rectangle at model
  z in [-0.01, 1.0] (`M2GroundDecal.h`, benilla's thresholds).
- vfog20: most ground spell quads already carry flag 4 (Consecration's disc and 23-yard floor, Flamestrike,
  Blizzard_Spawn, DeathAndDecay_Area_Runes), so the `!projected` gate never saw them and they kept the stock
  ±2-yard slab: hills clipped them and `groundDecals` changed nothing. `DrawBatchProj` now runs
  `CM2Scene::IsGroundDecalBatch` on flag-4 batches too.
- `DrawBatchProj`, for those batches only: draws whatever `projectedTextures` says (other flag-4 batches
  still obey it), multiplies the batch's texture animation into the tex matrix, and pads the box in z by the
  quad's width (benilla `fit_frame`: ±2·max half-extent) instead of ±2, so slopes to 45° stay inside.
- CVar `groundDecals` (default 1; 0 = stock flat planes).
- Cost: per decal, one `World::GetTris` box query plus one draw per receiving terrain chunk/WMO group batch,
  instead of one 2-triangle draw. CPU only; no extra passes or targets.
- Check: `clang++ -std=c++17 -I. Tools/GroundDecalCheck/check.cpp -o /tmp/gdc && /tmp/gdc`.
- Not done: doodad receivers (benilla has none either); a multi-texture ground batch keeps the flat plane.

## 2026-10-18: vfog22, client audit fixes

The 2026-10-18 audit's findings, fixed on four branches and merged together (published as `2026.10.18-vfog22`):

- **Vulkan** (`fix-vk`): hashed pipeline table that grows (stencil ref is dynamic state); frame flushed before a
  rebind; same-frame query reuse reset in order; recreated colour targets re-set; pending clears applied before
  present; scene copies refused inside an active query; D32 bias step 2^24; translated alpha test on 8-bit alpha.
  The translator changed, so every SPIR-V module was regenerated (1633 records + 2 companions, spirv-val clean).
- **D3D9 / 9Ex / D3D11** (`fix-d3d`): RESZ resolve re-sends state from Gx's caches instead of `Get*` (PUREDEVICE);
  shared INTZ/RESZ code in `D3d9SceneDepth.h`; scene-depth copy reports transient failures; D3D11 binds nothing for
  a viewless colour target, keeps stencil on depth clears, sizes b0 to the shader, caches MSAA resolves and per-mip
  RTVs; textures split into `CGxDeviceD3d11Texture.cpp`.
- **VFog** (`fix-vfog`): fade depth survives target rebuilds (C1 crash); CPU wave batches no longer dropped; exact
  foam integration (no 2-cycle); camera surfacing re-decides the frame; texture/shader failures retry; register
  restore bounded; unlit march as bands around the lit rect; GPU waves capped at 60 steps/s per tile.
- **Misc** (`fix-misc`): `OnWorldDone` runs before WorldFrame restores the outer viewport; ground-decal slab pad
  halved, rerouted decals drape M2 doodads and fall back to the flat batch over water (the project-texture
  callback returns the hit); occlusion queries drain before reuse; particle/query scratch vectors static;
  `s_mipBits` shrinks back after small loads; dead `NDEBUG` returns removed from DebugLog.

Checks: `check_gpu_waves.py` PASS (no foam exemption), GroundDecalCheck ok.

## 2026-10-18: vfog23, ghost-image fix

Published as `2026.10.18-vfog23`:

- **Vulkan** (`Readback.cpp`): a pending clear is applied before a scene copy or read-back, so copies no longer
  pick up the previous frame's contents (the ghost image).
- **FFX** (`FFX::Finish`): the glow is skipped when `GxTexCopy` fails instead of compositing a stale copy.

No shader or custom.MPQ changes.

## 2026-10-18: vfog24, audit leftovers and sun shadows

Published as `2026.10.18-vfog24`:

- **Audit leftovers** (`fix-leftovers`): scene depth is-a-copy and depth-fresh flags (P9, P13: the water's and the
  fog's depth copies share one snapshot); Vulkan refills of sampled images renamed into the pre-pass, inline fills
  deferred past queries (P4, L2); VFog model helpers shared (O4) and `RenderFog` split into pass functions (O3);
  vertex register generation skips the slice memcmp (P3, partial). Merged with vfog23's `OutsideAfterClears()`: the
  depth-fresh skip also requires no pending depth clear, so a skipped copy never returns pre-clear depth.
- **Shadows S1** (`shadow-s1`): D3D11 comparison sampler (hardware PCF) for D24X8 shadow targets, with the new SM4
  pack from shader-xlate 6f64417 in custom.MPQ (exe and pack ship together).
- **Shadows S2** (`shadow-s2`): sun shadows follow the real sun/moon direction, shadow maps snap to whole texels,
  cascade anchor holds through jumps; CVars `gfxSunShadows`, `gfxShadowSnap`, `gfxShadowAnchor` (default 1), new
  Video Options > Advanced > Shadows page.

custom.MPQ: 5121 files, vfog22's SPIR-V set unchanged (spirv-val clean); only the SM4 pack and
`VideoOptionsAdvanced.lua` differ. Checks: `check_gpu_waves.py` PASS, GroundDecalCheck ok.

## 2026-10-18: vfog25, shadowed sun shafts (shadow-s3)

Published as `2026.10.18-vfog25` (SHADOW_PORT_PLAN S3). The fog's sun in-scatter is now tested against the engine's
own cascaded sun shadow maps, so beams fall from the sun through the fog past trees, buildings and their M2/WMO
casters: lit where the sun reaches, dark in shadow. The screen-space god rays (VFogRayMask/Blur/Composite,
`vfogGodRays`) are unchanged and stay a separate option.

- **Accessor** (`ShadowCache.cpp/.h`, appended): `CShadowCache::GetPublished(ShadowView&)` gives each band's front
  `CGxTex*`, its receiver texgen rows (`s_shadowMatrices`, view space), the Gx view `PostUpdate` built them in
  (`s_publishedView`, new), map size, pcf flag and a PostUpdate count. 0 while the bands do not exist
  (`extShadowQuality` <= 2), are being recreated, or the shadow query never ran.
- **vfog** (`VolumetricFogRenderer.cpp` `ShadowConstants`/`BindShadows`): bands 1 and 2 (160 / 640 yd) bound at
  s5/s6, their rows composed with the fog's view (fog view -> camera-relative world -> PostUpdate's view) at
  c56-c61, control at c62 (on, hardware compare, canopy, size). Set for the march and for the composite, whose
  full-res edge re-march must agree. Unbound after each.
- **Shader** (`vf_shadow.hlsli`, new; `vf_integrate.hlsli`): one visibility per march step at the step's jittered
  sample point (IGN + frame, already the march's jitter), in band 1 where it covers the point (seam dithered by the
  jitter), else band 2, else lit with a 10 % edge fade. Mapping as the stock receivers (Terrain2[_pcf]):
  uv = q.xy * 0.5 + 0.5, lit when q.z <= stored. Visibility scales each layer's sun term (diffuse x phase) and, by
  `vfogCanopy`, its emissive haze glow (1 - canopy x shadow), which darkens the fog under trees.
- **Per backend**:
  - Vulkan (R32F maps, the user's path): four point taps per step, comparisons interpolated bilinearly
    (northlight `fogShadow`: interpolate comparisons, never depths). The SPIR-V is plain samples.
  - D3D11 (hwPCF, D24X8): one `SampleCmpLevelZero` tap (bilinear PCF through the S1 comparison sampler).
    `build_shaders.py` now translates the SM4 pack in-process with shader-xlate's `compile_one` and marks s5/s6 as
    compare slots for VFogMarch*/VFogComposite*. With `hwPCF 0` (R32F) the shafts are off on D3D11 (the SM4 shader
    would need a comparison sampler).
  - D3D9 / 9Ex: hwPCF uses the hardware compare in `texldl` (one tap), R32F the four-tap path. Every variant fits
    ps_3_0 (32 temps): the manual taps are accumulated one at a time and the visibility is one float through the
    layer loop.
- **CVars**: `vfogShadowedSun` (default 1), `vfogCanopy` (0..1, default 0.5); on Video Options > Advanced >
  Light & God Rays. Shadows off or low: the sun is unshadowed, as before.
- Not done: band 0 (40 yd) is not read (one sampler less, the near 40 yd are a small part of a fog ray); no
  receiver-plane bias beyond the engine's own (the march points are in the air, not on surfaces).
- **Also in vfog25** (merged, no shaders):
  - `fix-texdecay` (`CGxDeviceVulkanTexture.cpp`): Vulkan remakes a recycled texture's image when its data format or
    mip count differs (garbled letters / white textures after relogs).
  - `fix-lowwater` (`VolumetricFogWater.cpp`): stock splash rings and wake trails are no longer hidden at sky-only
    water quality or with reflections off.
  - `fix-fogedge` (`VolumetricFogRenderer.cpp` `CompositePass`): the noise atlases s9/s10 are bound for the
    composite's full-res fallback march (fog-coloured outlines on silhouettes near the fog start).

custom.MPQ: 5121 files; the SPIR-V set regenerated (1633 records + 2 companions, spirv-val clean; only the 72 VFogMarch/
VFogComposite* records changed), the SM4 pack from shadow-s1, our ps_3_0/ps_4_0 fog shaders rebuilt.

## 2026-10-18: vfog28, temporal filter and the shadowed sun (fix-temporal)

What the user saw on vfog25 (Vulkan, Wow64): with `vfogTemporal` on, turning the camera left a ghosted image of the
area in the fog that flickered and settled; with `vfogTemporal 0` a warped, fogged copy of the background stayed fixed
on screen.

- Checked and ruled out: the temporal-off composite reads the current march (`TemporalPass` returns
  `s_targets.march`, s1 = s4 = march); the history ping-pong (`history[2]`, `historyDepth[2]`) flips once per filtered
  frame and nowhere else; `StoreHistory` stores the view/projection once per `RenderFog`, after the passes, as
  pre-O3 (bb9d49a^) and coa-vfog `renderer.cpp:1510-1522` do; validity (first frame, resize, rotation > 45 degrees,
  settings, temporal off -> on) as coa `:1320`. Vulkan: no target is sampled while bound (`ITexResolve` binds white
  for the current target), render passes carry the external colour -> fragment-read dependency, descriptor sets are
  keyed by never-reused image ids.
- Cause 1 (`vf_integrate.hlsli` `StepJitter`): with `vfogTemporal 0` the march has no jitter (coa's fixed mid-step
  sample). Since vfog25 every step also does an on/off shadow compare (`SunVisibility`), so at fixed fractions of each
  ray's length the shafts are sampled as shells round the camera, scaled by the pixel's depth: the shadow pattern
  turned into a warped copy of the scene behind, fixed to the screen. Now, with temporal off and the shadowed sun on,
  the step is dithered in space alone (IGN of the low-res texel, no frame term; c11.x = 0.5 selects it, 1 = animated, 0 = none); without `vfogShadowedSun` it stays 0.5.
- Cause 2 (`VolumetricFogRenderer.cpp` `TemporalPass`): the shafts are in the air, but the history is reprojected by
  the surface depth, so when the orbit camera moves as it turns, the old shafts land in the wrong place. Their
  jittered four-tap compares make the 3x3 clamp range too wide to catch them, so they blended in as a ghost until the
  camera stopped. The shadowed sun now counts as changing light (`adaptive`, `cTemporal.z`): the existing lighting
  reject (`LightingHistoryWeight`) drops history that differs from the current frame by more than 20-40 %.
- `SameSettings` now also compares `shadowedSun` and `canopy`, so toggling them resets the history.

Shaders: VFogMarch* / VFogComposite* / VFogCompositeSplit* rebuilt (ps_3_0, ps_4_0), SPIR-V regenerated (spirv-val
clean), custom.MPQ rebuilt with shadow-s1's SM4 pack.
