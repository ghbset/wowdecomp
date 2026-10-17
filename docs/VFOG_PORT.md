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
| world done | end of OnWorldRender (before `FFX::Finish`) | `OnWorldDone()` restores DNInfo fog | 0x4F9281 restore |

The decisions:
- **Push-out with no lag.** coa-vfog guesses from last frame, and wxl needed an 8-frame hold. Natively,
  `OnWorldUpdate` knows this frame's answer before any fog constant is built. `Active()` is true when: CVar
  `vfog` is set, `RendererReady()`, the camera is not in liquid (`World::SceneCamLiquidStatus`), and FFX/fog
  master enable allow it. The push happens after `farFog` is read, so object fog culling stays stock.
- **Early composite only, no late composite.** coa's default draws at FFX::Finish. We use its TransparentFog
  mode, which wxl-vfog shipped as its default. With stock fog pushed out, anything drawn after the composite
  would otherwise be bare. Transparents drawn after it (weather, M2PASS_1, spell visuals, particles, GxuCat_1)
  get the fitted linear fog. The fit uses the screen-wide fit, or the per-model fit when the model is past the
  fit range (wxl-vfog 0.4/0.6.13).
- **Under water: off**, with stock fog (coa `Underwater=0`). The under-water branch has no early composite.
- **Not ported in v1:** water (vw_*), god rays (default off in coa; FFX glow covers the look), the MSAA
  split/silhouette, the GPU density probe, grading, the overlay, and wxl's fading-doodad early draw and TopUp.
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

Blend: the Gx API has no (ONE, INVSRCALPHA). So the composite outputs `float4(fog.rgb / max(a, 1e-4), a)`
and draws with `GxBlend_Alpha` (SRCALPHA, INVSRCALPHA), which gives the same result. `GxRs_ColorWrite` is
RGB only, and depth test and write are off. Linear mode (ColorSpace=1) does the gamma math in the shader
as coa does. Use the blend-over-scene-copy path only if the premultiplied form shows banding.

History invalidation follows coa renderer.cpp:1323-1332: settings, map, screen-effect slot or
projection/viewport change; forward dot < 0.707; a frame gap over 0.25 s; a move over 30 yd. Key it on the
view matrix, never on a frame counter, because water reflection re-runs world passes (wxl lesson).

## 3. Render targets

All are created with `TexCreate` and the renderTarget flag, like `FFX::CreateTex`, and recreated on a
viewport size change or device restore.

| Target | Size | Format | Notes |
|---|---|---|---|
| march | low | `GxTex_Argb8888` | coa's own fallback from A16B16G16R16F. Add an RGBA16F EGxTexFormat only if banding shows |
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

Names are built as `VFogMarch2`, `VFogMarch2L`, `VFogMarch2N` and `VFogMarch2LN`: 12 march + 12 composite +
2 = 26 shaders. Unroll layer loops (`[unroll]` over the 4 layers), because vkd3d turns indexed constant
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
| c96-c98 | composite, godray colour (w = 0, dither removed), sun |
| **c99** | **new**: horizon band (minZ, maxZ, near, far) |
| **c100-c131** | **new**: 8 local lights x 4 (pos view + cutoff, colour, attenuation c/l/q, spare). Replaces sampler s8 |

| Sampler | Use |
|---|---|
| s0 | scene depth (point) |
| s1-s4 | composite: fog/history, unused god rays, unused scene copy, current march. Temporal: current, history, depth, historyDepth |
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
`vfogTransparent` (1), `vfogStartOffset` (10), `vfogMaxDistance` (5000), `vfogNoise` (0.15), `vfogDebug` (0).
The defaults are coa CoAVolFog.ini plus the user's wxl-vfog look: Ambient 1.75, ClassicExposure 0.5,
LocalLightIntensity 2.5, StartOffset 10. FarFog is 1, not wxl's 0, because no far-field plugin owns far fog
now. **No dither**: it is a known fps regression. The remaining Config fields are constants in code
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
