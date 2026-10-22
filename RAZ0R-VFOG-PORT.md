# Raz0r1337/coa-vfog fork vs our vfog port (planning, 2026-10-22)

Upstream clone: /root/wotlk-renderers/coa-vfog, HEAD 3908909. Our port is based on 4e31ddf (3 commits behind HEAD:
CI, docs and the interior far-fog control, which we already have as vfogInteriorFarFog).
Fork: remote `raz0r`. `st0ny-main` = 27 commits past HEAD; `st0ny-dev` (= FETCH_HEAD) = those 27 + 12 more
(the Outland/Northrend work). CI, README, release and AGENTS-only commits are left out below
(0ee3cfc, aaac5f3, 0843fc9, 017f567, bb217f6, 1a3ea4e, 7a9ee20, f1ca82c, a270caf, 5df5ec3, 82f1174, merges).

Read-only survey; nothing edited, built or published.

## Recommended port order

| # | What | Fork commits | Applies | Effort |
|---|------|--------------|---------|--------|
| 1 | Storm params without fog keys keep the clear layers | d08a6a3 | yes | 15 min, ~8 lines, VolumetricFogData.cpp only |
| 2 | Temporal lighting rejection on the 3x3 neighbourhood mean | 789fdc6 | yes | 30 min, ~20 lines, VFogTemporal.hlsl + shader rebuild |
| 3 | Void below the horizon fogged like the sky above (Netherstorm white wall) | 1667464 | yes | 5 min, 1 line, vf_integrate.hlsli:340 |
| 4 | Storm-noise interpolation of the filtered fog between taps | 0d6a2f6 | yes | 30-45 min, ~35 lines, vf_composite.hlsli |
| 5 | Lightstay: keep lights of models not drawn this frame | 2fcb056 (part) | partly | 30 min + a check: source change, not a byte patch |
| 6 | Outland/Northrend fog data + per-region density | 2a64f31, bfe7663, 55923a2, a8ae5c7, f519263, aa4ca03, d44f4e1, 00b8678, ef983f1 | partly | 2-4 h |
| 7 | Local light stabiliser (pool, hysteresis, 1 s smoothstep fades, frustum test) | 2fcb056, c2e640e, 881523f, 3adef89, 59220bf | partly | 1-2 days; touches CaptureLocalLights in VolumetricFog.cpp (wait for the doodad agent) |
| - | Fog after glare, before world text | 205ade5, 5057b96 | no (already so) | 0 |

## Per commit

| Commit | Change | Our code path | Applies | Effort / notes |
|--------|--------|---------------|---------|----------------|
| d08a6a3 | `KeepClearLayersWhereStormHasNoFogKeys`: when the storm-slot LightParams has no fog layers, copy the clear layers into it before `BlendConditions`. 31 Classic lights (EPL light 42 etc.) point their storm slot at fogless params, so fog faded to ~1/23 as rain set in. | `FogData::LightConditionFog`, VolumetricFogData.cpp:697-710, identical to pre-fork code | **yes** | trivial. Same data file, so the bug is live for us now. |
| 789fdc6 | `LightingHistoryWeight` compares the 3x3 neighbourhood **mean** with history instead of the raw centre texel; `CurrentNeighbourhood` returns lowest/highest/mean. Stops step-jitter from reading as a light change (grain at Low/Medium with noise or point lights). | VFogTemporal.hlsl:27, 41, 158-160, identical to pre-fork | **yes** | small, shader only. |
| 1667464 | `upward = skyMask > 0 ? abs(directionWorld.z) : max(directionWorld.z, 0)` | vf_integrate.hlsli:340, identical to pre-fork | **yes** | one line. Visible only over voids (Netherstorm edges, Outland). |
| 0d6a2f6 | When a layer carries noise and the composite marches local lights without sampling noise (`kMarchesLocalLights && !kSamplesAuthoredNoise && AnyLayerNoise()`), a pixel matching no tap takes the bilinear **filtered** fog of the 4 taps when on their plane (`PixelOnTapPlane`), instead of re-marching at the noise mean. Fixes bright lines one low-res texel apart on grazing water in storm fog near point lights. | vf_composite.hlsli:153-251 has `PixelOnTapPlane`, `RayMeetsLocalLight`, `TapsReproduceMarch`, `FogWithoutMatchingTap`; vf_authored_noise.hlsli has `AnyLayerNoise`, `kSamplesAuthoredNoise` | **yes** | small; check ps_3_0 temp-register budget in the LOCAL_LIGHTS variants after the change (fork notes this was the constraint). |
| 2fcb056 (lightstay) | Patches the client's je at 0x81E548 to jmp so a light whose model was not drawn this frame is not disabled via 0x8356F0. | Our source: `CM2Scene` light gather, Engine/Source/Model2/M2Scene.cpp:971-976 (`light->m_stamp != m_frameStamp` -> `light->SetVisible(0)`). Our `CaptureLocalLights` reads `m_visible` from the native grid, so it has the same flicker. | **partly** | one-line source change (skip or CVar-gate the `SetVisible(0)`), but confirm in Ghidra that 0x81E548 is this site before doing it, and note it also changes stock M2 lighting (stale lights keep lighting models; that is what the exe patcher's No. 70 byte does anyway). |
| 2fcb056 (walk) | Tolerant table walk: skip disabled/non-point nodes instead of rejecting the capture, cap at 8192 nodes; interior inputs on partial captures. | Not needed: our walk is the native `m_lightGrid` with typed `CM2Light`, already skips non-point/invisible. | no | 0 |
| 2fcb056, c2e640e, 881523f, 3adef89, 59220bf (stabiliser) | `LocalLightStabiliser` (local_light_stabiliser.cpp, 332 lines): 24-candidate pool, 8 slots, 1.5x priority margin over 3 captures, 0.3 s priority smoothing, hold 8 failed captures, identity by 2 yd, edge-of-view 5 yd tolerance, 1 s smoothstep fade-in/out (3adef89), evicted slot inherited by its challenger, overlapping swaps, edge-entering lights not re-faded (59220bf). c2e640e drops the camera-distance gate (reach + 200 yd, last 50 yd faded) for a view-frustum test before ranking. 881523f ranks equal-irradiance lights by distance, not position. | `SelectLight`/`Precedes`/`CaptureLocalLights`, VolumetricFog.cpp:380-470: per-frame top-8 re-rank from scratch, distance gate `cutoff + kMaxLocalPointLightRadius`, ties broken by position. | **partly** (stabiliser + frustum + tie-break yes; table-walk parts no) | 1-2 days: port the class into a new VolumetricFogLights.cpp (fits our `LocalLight` with an added weight that scales colour), feed it a frustum from the camera, call it after `CaptureLocalLights`. Needs time step and map-change reset. Port the harness checks as an fncheck-style unit test. Do after the doodad agent is out of VolumetricFog.cpp. 881523f's tie-break alone is a 5-line change to `Precedes`. |
| 85018a6 | Settings text "lights in view" | no settings panel | no | 0 |
| 85a1b46 + 0b72d47 | Water foam mip chain, then reverted | net zero | no | 0 |
| 205ade5, 5057b96 | TransparentFog=0: draw the fog right after the glare pass, before world-name text (was: after the whole frame, so names in front of the sky took the sky's fog). transparent_fog_checks.h gained only `CheckFogDrawnBeforeWorldText`. | Already our order: WorldFrame.cpp:1811 `RenderGlares` (late path, `GlareDrawn()` false) -> 1823 `OnWorldDone` (late composite) -> RenderWorld:2136 grade -> `WorldTextRenderBatch`. | **no (already done)** | 0. Only difference: our late composite also runs after `PlayerNameUpdateLate` and `CGChatBubbleFrame::OnWorldRender`, which only place frames, not draw. |
| 2f6757f, e54dd93 | Outland/Northrend borrow Kalimdor (191) / EK (1) global light | superseded by 55923a2 (code removed) | no | 0 |
| 2a64f31, d44f4e1 | Per-region density: `RegionOfMap` (530 Outland, 571 Northrend, else Azeroth); `ConfigForMap` multiplies density by Azeroth/Outland/NorthrendDensity (1.0 / 0.15 / 0.15); Outland/NorthrendClassicFog=0 switches that region to derived layers. | `BuildFogParams(s_in, s_cfg)` VolumetricFog.cpp:1045 and `ResolveFogData` call sites; no region concept | **yes** | ~1 h: 5 CVars (vfogDensityOutland, vfogDensityNorthrend, vfogDensityAzeroth, vfogDataOutland, vfogDataNorthrend) + a `ConfigForMap` in VolumetricFogModel.cpp. |
| bfe7663 | Per-region SkyColor share: mix layer emissive toward the client's fog hue at the layer's brightness (`TakeClientFogHue`) | `BuildFogParams` | yes, optional | 30 min; defaults 0 so no visible change unless set. |
| 55923a2, a8ae5c7, f519263, aa4ca03 | tools/convert_client_lights.py converts the client's own Light/LightParams for Outland, Northrend and every map Classic never had into the same fogdata.bin (VFD1 format 4, header-compatible); final aa4ca03 keeps Classic data for the old world. File grows 755960 -> 2310020 bytes. | Tools/CustomMpq/VFog/fogdata.bin (755960 = upstream) | **yes** | drop-in file swap (same format 4), or rerun the script against OUR client DBCs (better: our Light.dbc may carry custom maps/lights; the fork's file is built from stock 3.3.5a). Custom tswow maps will start resolving converted data. |
| 00b8678, ef983f1 | Haze / FarFog / GroundFog now also scale the Classic haze (layer 0), far wall (layer 1) and ground layers, relative to their defaults (1, 1, 0.6); finer slider. | `AuthoredLayers`, VolumetricFogModel.cpp (our vfogHaze/FarFog/GroundFog scale derived layers only, VolumetricFogModel.cpp:111/125/211) | yes, **semantics change** | 20 min. Caution: with this, FarFog=0 (the preset) would delete the Classic far wall; without it FarFog only affects maps with no data. |
| 1667464 | see above | | | |

## Themes asked about
- Light stabiliser / lightstay / 1 s fade: rows 5 and 7. Lightstay is cheap in source; the stabiliser is the big item.
- Outland/Northrend + per-region density: row 6; data is a drop-in, code is ~1-2 h.
- Storms without fog keys: row 1, trivial, live bug for us.
- After glare, before world text: already our order.
- Temporal rejection on the mean: row 2.
- Storm-noise interpolation: row 4.
- transparent_fog_checks.h: only the new world-text order test; no change to the transparent (TransparentFog=1) fit or batch fog.

## Grey doodads with vfogTransparent=1
No fork change explains or fixes it. The fork touches nothing in the TransparentFog=1 path (stock-fog fit, M2 batch
fog, fade-early); its transparent_fog_checks.h change only tests the TransparentFog=0 draw order. 0d6a2f6 changes the
no-matching-tap fallback only when a noisy layer and local lights are both active, not for thin doodads in general.
VFOG-DOODAD-STATUS.md's lead (instanced pass-0 doodads, DrawBatchDoodad) is still the best one.
One thing worth checking against that lead: with vfogTransparent=1, `BatchFog` (VolumetricFog.cpp:1123) applies to any
M2 batch drawn while `s_armed` is set. If instanced `DrawBatchDoodad` draws go through the batch-fog hook and run after
`OnLiquidDone` armed it (for example a second pass, or reflection pass re-entry), they would get the fit fog on top of
the composite = grey, over-fogged, and clear once they leave fit range. A frame capture with vfogTransparent 1 that
logs `BatchFog` calls per model name would confirm or rule it out.

## Preset (fork's recommended CoAVolFog.ini)

Every coa key in the preset has a vfog CVar in our port (vfog13 added SunScatter, ClassicExposure, ClassicPhase,
LocalLightIntensity/Phase, InteriorDensity/FarFog, ColorSpace, GlowCompensation). NoiseAmount is `vfogNoise`;
ColorGrading is `vfogGrade` + `vfogGradeStrength`. The preset is from st0ny-main (no region keys).

| coa key | preset | our CVar | our default | change? | notes |
|---------|--------|----------|-------------|---------|-------|
| Enable | 1 | vfog | 1 | - | |
| EngineHooks, Overlay, OverlayKey, LogLevel, DebugView, WaterDebugView | - | - (vfogDebug, vfogWaterDebug) | 0 | - | dll-only / debug |
| LightStay | 1 | none | - | port | row 5; no runtime equivalent yet |
| Quality | 1 | vfogQuality | **2** | **1** | same meaning: 1 = quarter res 16 steps |
| Density | 0.8 | vfogDensity | 1 | **0.8** | |
| Haze | 1.0 | vfogHaze | 1 | - | |
| GroundFog | 0.6 | vfogGroundFog | 0.6 | - | |
| FarFog | 0 | vfogFarFog | **1** | **0** | Same meaning as ours today: scales only the derived distance layer, used where there is no fogdata (Outland, Northrend, custom maps) or with vfogData 0. 0 means no distance fog there at all, with the stock fog still replaced (vfogStockFog 1). VFOG_PORT.md picked 1 deliberately (no far-field plugin owns far fog). If we port 00b8678, FarFog 0 would also delete the Classic far wall, so do not take this value and 00b8678 together. |
| NoiseAmount | 0 | vfogNoise | **0.15** | **0** | turns off the density variation; Classic banks stay (ClassicNoise) |
| NoiseScale | 0.025 | vfogNoiseScale | 0.025 | - | |
| NoiseWindSpeed | 4.34 | vfogNoiseWind | **0.5** | **4.34** | moot while vfogNoise 0 |
| ClassicNoise | 1 | vfogClassicNoise | 1 | - | |
| StockFog | 1 | vfogStockFog | 1 | - | |
| TransparentFog | 0 | vfogTransparent | 0 | - | |
| DataMode | 1 | vfogData | 1 | - | |
| ColorSpace | 1 | vfogColorSpace | 1 | - | |
| SunScatter | 0.2 | vfogSunScatter | **1** | **0.2** | biggest look change: much less sun-lit fog |
| Ambient | 0.8 | vfogAmbient | **1.75** | **0.8** | 1.75 was the user's wxl-vfog look (VFOG_PORT.md:573) |
| Exposure | 1.0 | vfogExposure | 1 | - | |
| ClassicExposure | 0.85 | vfogClassicExposure | **0.5** | **0.85** | 0.5 was the user's wxl-vfog look |
| ClassicPhase | 0 | vfogClassicPhase | 0 | - | |
| LocalLights | 1 | vfogLocalLights | 1 | - | |
| LocalLightIntensity | 1.2 | vfogLocalLightIntensity | **2.5** | **1.2** | 2.5 was the user's wxl-vfog look |
| LocalLightPhase | 0.4 | vfogLocalLightPhase | **0.3** | **0.4** | |
| InteriorAware | 1 | vfogInterior | 1 | - | |
| InteriorDensity | 0.15 | vfogInteriorDensity | 0.15 | - | |
| InteriorFarFog | 1.0 | vfogInteriorFarFog | 1 | - | |
| GodRays | 1 | vfogGodRays | 1 | - | |
| GlowCompensation | 1 | vfogGlowCompensation | 1 | - | |
| ForeverGlow | 0 | vfogForeverGlow | 0 | - | |
| ColorGrading | 0.5 | vfogGrade + vfogGradeStrength | **0** + 1 | **1 + 0.5** | coa uses one 0..1 strength (0 = off); ours gates with vfogGrade and scales with vfogGradeStrength |
| FarClipMax | 1583 | none (client `farclip`) | - | - | coa raises the client's farclip cap; our decomp owns the cap directly, so just set `farclip` |
| MaxDistance | 5000 | vfogMaxDistance | 5000 | - | |
| Temporal | 0.85 | vfogTemporal | 0.85 | - | |
| Underwater | 0 | vfogUnderwater | 0 | - | |
| LiquidDepth | 1 | vfogLiquidDepth | 1 | - | |
| Multisampling | 1 | vfogMsaaSplit (closest) | 1 | - | coa key keeps MSAA working under the fog; ours is always on, vfogMsaaSplit is the per-sample split |
| SunMarker | 0 | vfogSunMarker | 0 | - | |
| Water | 1 | vfogWater | 1 | - | |
| WaterQuality | 2 | vfogWaterQuality | 2 | - | |
| WaterWaves | 1.0 | vfogWaterWaves | 1 | - | |
| WaterWind | 2.0 | vfogWaterWind | 2 | - | |
| WaterFoam | 1.0 | vfogWaterFoam | 1 | - | |
| WaterReflections | 0.5 | vfogWaterReflections | **1** | **0.5** | |
| WaterSpecular | 1.5 | vfogWaterSpecular | **1** | **1.5** | |
| WaterClarity | 1.4 | vfogWaterClarity | **1** | **1.4** | |
| WaterZoneColors | 0.5 | vfogWaterZoneColors | 0.5 | - | |
| WaterRipples | 0.5 | vfogWaterRipples | 0.5 | - | |
| WaterClientSplashes | 0 | vfogWaterClientSplashes | 0 | - | |

Ours only (no preset key, keep as is): vfogStartOffset 10, vfogFadeEarly, vfogFadeStencil, vfogDither, vfogHdrTargets,
vfogShadowedSun, vfogCanopy, vfogWaterStencil, vfogWaterGpuWaves.

To make the preset our default: change the `CVar::Register` default strings in VolumetricFog.cpp (vfogQuality,
vfogDensity, vfogFarFog, vfogNoise, vfogAmbient, vfogGrade, vfogGradeStrength, and the vfog13 `reg` lines for
SunScatter, ClassicExposure, LocalLightIntensity, LocalLightPhase, NoiseWind) and VolumetricFogWater.cpp:1016-1018
(Reflections, Specular, Clarity). Saved CVars in the user's Config.wtf override defaults, so the user should also run
the line below once.

Try live (one chat line, 254 chars, under the 255 limit; NoiseWind left out since it is moot while vfogNoise is 0):

```
/run for k,v in pairs{Quality=1,Density=.8,FarFog=0,Noise=0,Ambient=.8,SunScatter=.2,ClassicExposure=.85,LocalLightIntensity=1.2,LocalLightPhase=.4,Grade=1,GradeStrength=.5,WaterReflections=.5,WaterSpecular=1.5,WaterClarity=1.4}do SetCVar("vfog"..k,v)end
```
