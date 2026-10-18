# Backlog ports (FEATURE_BACKLOG.md items, native in the decomp)

## Caustics (2026-10-18)

Backlog #6, ported from /root/wxl-water branch `caustics` (17cf9af): `Water.cpp` CausticsUnder + the "3a caustics"
term of the surface shader, `Shaders.hpp` Caustic / SunVis / kCausticUnder (the `caustics` and `uwtint` knobs).

**CVars** (VolumetricFogCaustics.cpp, registered from `RegisterCVars`; read every frame, live):
- `waterCaustics` 0..3, default 1 (wxl-water `caustics`): sunlight rippling on the ground under water, from above and below.
- `waterUnderwaterTint` 0..2, default 1 (wxl-water `uwtint`): Cata FFXUnderWaterLow, the underwater fog hue
  (brightness kept, x0.3 per unit) and darker corners (vignette 0.45 per unit, cap 0.9) while the camera is submerged.
Both default on as in the plugin (the user: "looks good"): each is one full-screen pass that clips untouched pixels,
and the caustic is a branch that runs only within 90 yd and 7 yd (above) / 25 yd (below) of water.
Advanced options > Water: "Caustics" and "Underwater tint" sliders.

**Passes** (both blended `GxBlend_Mod2x`, 2 src dest, so 0.5 = unchanged; RGB only; `clip` where nothing changes):
- **VFogCausticsAbove**, from `WaterFlushDone` after the modern water's classes are shaded: s0 = the water's
  pre-water view-Z copy (the bed, `s_targets.sceneDepth`), s1 = the copy with the water (stencil scheme: the one
  after the pass; mask scheme: the last class's signed copy, |r|). No new depth copy. Thickness = bed Z - water Z along
  the view, weights as wxl-water (0.5 yd ramp, gone by 7 yd thick, 90 yd away, and fading with the sun lower than z 0.4). With the stencil
  scheme the pass tests `stencil & 3 != 0` (lake/river/ocean; the interior class 4 and dry pixels are skipped).
  Only while the modern water draws (vfog active, `vfogWater 1`, above water).
- **VFogCausticsUnder**, from `OnWorldDone` with the camera in liquid (before the late vfog composite): the scene
  depth (`DeviceCopySceneDepth`, decoded through c3/c99), surface height from `World::SceneCamLiquidStatus`, plus the
  tint. vfog is off under water by default, so `OnOpaqueDone` then captures the view/projection/viewport
  (`CaptureView`, split out of OnOpaqueDone) and the DNInfo inputs into a second FrameInputs; with `vfogUnderwater 1`
  the fog's own inputs are used.
- No caustic at night (moon), with the sun under the horizon, or with the camera in an interior WMO group
  (`interiorBlend > 0`); the tint still runs. Sun colour = DNInfo `m_bands.m_diffuse`; tint colour = the fog colour
  DNInfo holds under water.
- Sun shadow: the engine's bands 1 and 2 through vf_shadow.hlsli (`SunShadowRegisters`, split out of the renderer's
  ShadowConstants), as the shadowed fog; unshadowed without the bands (shadows Low) and on D3D11 with R32F maps.
- Water reflection pass: this client has none (the modern water reflects in screen space), nothing to skip.

**Files**: WoW/Source/WorldClient/VolumetricFogCaustics.cpp (new), VolumetricFogWater.h (declarations),
VolumetricFogWater.cpp (`CommonRows` split out of BuildCommon; two `CausticsAbove` calls in WaterFlushDone),
VolumetricFogRenderer.cpp (`SunShadowRegisters`), VolumetricFog.cpp (CVars, CaptureView, the underwater hook),
Tools/CustomMpq/ShaderSrc/VFogCaustics.hlsl (new; `UNDER` 0/1) + build_shaders.py (records, s5/s6 compare slots),
VideoOptionsAdvanced.lua.

**Shaders** (custom.MPQ must carry them): `Shaders\Pixel\ps_3_0\VFogCausticsAbove.bls`, `VFogCausticsUnder.bls`
and their `ps_4_0` twins (committed), and the two Vulkan modules the translator makes from the ps_3_0 ones.

**Deviations**: the plugin lit the refraction inside its water shader; here the pass scales the shaded water pixel,
with the reflected share (Schlick, flat surface) taken out. The underwater face normal comes from the next pixels'
depth instead of ddx/ddy (the Vulkan translator has no dsx rule). wxl-farfield's land-shadow height map is not
ported (no such map natively; the sun shadow bands cover the near field). Pixels past the bands are unshadowed.

**Unverified**: nothing has run in game or on the rig. The pattern's brightness against the vfog water's own
transmittance, the stencil read inside the liquid flush on all three backends, and the Mod2x 0.5 round trip
(clip covers the untouched pixels) are the things to look at first.
