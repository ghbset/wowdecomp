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
- **Under water: off**, with stock fog (coa `Underwater=0`). The under-water branch has no early composite.
- **God rays** (vfog4): see section 2a. On by default (`vfogGodRays 1`, coa's "on" strength); coa ships `GodRays=0`.
- **Water** (vfog5): see section 2b. On by default, as coa (`Water=1`). Ripples and wakes (vfog7): section 2b, ripples.
- **Colour grading** (vfog6): see section 2c. Off by default, as coa (`ColorGrading=0`).
- **Dither** (vfog6): `vfogDither 1` puts a +-0.5/255 triangular dither (interleaved gradient noise from the pixel
  position, ALU only, no fetch; vf_common `Dithered`) on what the march, temporal and composite store into 8-bit
  targets and on the grade, against fog banding. Each pass has its own offset; the frame index animates it so the
  temporal blend averages it instead of accumulating a fixed bias (h = c + d/(1-w)). The composite dithers the colour it
  writes, i.e. after the conversion back to display space in LinearOverSceneCopy. coa's own dither was removed earlier
  as an fps regression that was **never isolated** (wxl-vfog/wxl-light composites, 2026-10-10), so it is a CVar:
  `vfogDither 0` skips it on a uniform branch (c79.x = 0), the old path.
- **Not ported in v1:** the MSAA
  split/silhouette, the GPU density probe, the overlay, ForeverGlow, and wxl's fading-doodad early draw and TopUp.
  The fading-doodad draw is a v2 item if fading props pop; see wxl-vfog hooks.cpp DrawFadingModelsBeforeFog
  and /root/RE/render-path/doodad-fade-*.md.

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

**Mask scheme (no stencil).** coa tags each class into the stencil during the client's draws and shades after the
pass. Here, in `Liquid::CRenderer::Flush` (transparent pass; coa hooks the same pass only):
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

**Waves (deviation: CPU).** The Gx API has no RGBA16F/32F targets and no render-to-mip, so coa's GPU FFT runs on
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

**Deviations from coa**: CPU waves (above); scene copies clamp instead of mirror addressing (the Gx API has no
mirror); no packed-RGBA8 depth fallback (needs R32F targets); no MSAA (DeviceCopySceneDepth is no-MSAA, as the fog);
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

**Left**: MSAA.

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
| march | low | `GxTex_Argb8888` | coa's own fallback from A16B16G16R16F. Add an RGBA16F EGxTexFormat only if banding shows |
| rays[2] | viewport / 4 | `GxTex_Argb8888`, bilinear | god rays (section 2a), created on first use |
| scene copy | world viewport | `GxTex_Argb8888`, bilinear | pre-fog copy (s3, ray mask), reused for the post-world ray composite |
| history[2] | low | `GxTex_Argb8888` | ping-pong |
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
- **D3D11 (WP5)**: `CopyResource(m_depthTexture -> a GxTex_D24X8 CGxTex)`. That texture already gets an
  R24_UNORM_X8 SRV (CGxDeviceD3d11.cpp:3081, :3223-3242), with point sampling. No-MSAA only; return nullptr
  under MSAA.
- **D3D9 (WP5)**: an INTZ texture at back-buffer size bound as the world depth-stencil, as coa d3d9_wrap.cpp:959
  and :1002-1022 do. Check INTZ with CheckDeviceFormat. No-MSAA only; return nullptr otherwise. ShadowCache's
  own D24X8 targets must keep working. This covers both `CGxDeviceD3d` and `CGxDeviceD3d9Ex`.

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
| **c79** | **new**: vfogDither (x on, y frame); every VFog fog pass and VFogGrade |
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
other opcode the VFog shaders use. No vPos, oDepth or volume samplers are needed, by design.

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
`vfogTransparent` (1), `vfogStartOffset` (10), `vfogMaxDistance` (5000), `vfogNoise` (0.15), `vfogDebug` (0),
`vfogGodRays` (1 = on, the user's choice; coa GodRays, a 0..4 strength, 0 = off, coa ships 0),
`vfogDither` (1; 0 = the undithered path), grading: `vfogGrade` (0 = off, coa ColorGrading=0), `vfogGradeStrength`
(1, 0..1, coa ColorGrading's strength when on).
Water (coa CoAVolFog.ini Water* defaults and ranges): `vfogWater` (1), `vfogWaterQuality` (2, 1..3),
`vfogWaterWaves` (1, 0..2), `vfogWaterWind` (2, 0.5..10), `vfogWaterFoam` (1, 0..2), `vfogWaterReflections` (1, 0..2),
`vfogWaterSpecular` (1, 0..4), `vfogWaterClarity` (1, 0.25..4), `vfogWaterZoneColors` (0.5, 0..1), 
`vfogWaterRipples` (0.5, 0..2; 0 = off: no simulation and no reading of the units; coa WaterRipples),
`vfogWaterClientSplashes` (0 = the client's splash discs and wake trails hidden while the ripples run, 1 = kept; coa
WaterClientSplashes), `vfogWaterDebug` (0; 1 normal, 2 foam, 3 transmittance, 4 reflection, 5 class, 6 ripples).
The defaults are coa CoAVolFog.ini plus the user's wxl-vfog look: Ambient 1.75, ClassicExposure 0.5,
LocalLightIntensity 2.5, StartOffset 10. FarFog is 1, not wxl's 0, because no far-field plugin owns far fog
now. Dither: see section 1 (vfogDither; the old fps regression was never isolated, so it can be toggled). The remaining Config fields are constants in code
until someone needs a knob for them.

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
