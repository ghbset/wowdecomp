# Legion port (LEGION_SURVEY.md items, one dated section per item)

## 2026-10-18: Legion shadowmask (survey item 6, 2.7)

Mountains (near ADT and far WDL) now cast the sun's shadow onto the world: terrain, WMOs, M2s, detail doodads and the
far horizon land, plus the modern water's sun glint, foam and in-scatter sun.

**Pass.** `VFogShadowMask` (ours; Legion's `shadowmask` ps was read for its role only) is one full-res quad over the
world viewport, drawn by `VolumetricFog::RenderShadowMask` from `VolumetricFog::OnSolidDone()` (WorldFrame.cpp, after
`M2PASS_0` and `RenderDetailDoodads`, before liquids, the fog composite and the transparents). It reads the scene depth
(`DeviceCopySceneDepth`), rebuilds the view position, tests it against gfxTerrainShadow's lit-height texture
(`TerrainSunShadow::Update`, the same s7/c63 row as the fog, plus a 4 yd surface bias for the ADT between WDL vertices)
and multiplies the colour (`GxBlend_Mod`, RGB) by

    (ambient + diffuse * ndl * min(T, C)) / (ambient + diffuse * ndl * C)

with T the land's visibility, C the engine cascades' visibility (vf_shadow.hlsli `CascadeSunVisibility`, bands 1-2),
ambient and diffuse the DNInfo light colours, and ndl from a normal rebuilt from the depth (the smaller-step neighbour
per axis, so silhouettes keep their own side). So:
- a sun-lit surface under a mountain goes to its ambient; a surface facing away from the sun (ndl 0) is unchanged;
- a pixel a stock receiver already shadowed with the cascades (C = 0) is unchanged: **no double darkening**, and the
  ratio is never above 1, so nothing is brightened;
- sky pixels and fully lit pixels return after one depth and one land tap; the fog is applied afterwards, so it lies
  over the shadow. With the volumetric fog off (or vfogStockFog 0) the stock fog is already in the colour, so the
  darkening fades by the stock fog's linear factor. It fades out by DNInfo `m_interiorFogBlend` in WMO interiors.
- The frame inputs come from `OnOpaqueDone`, which now also captures (with its DNInfo half) when only the mask wants
  them; local lights are captured only for the fog.

**Water.** VFogWater{1,2,3} read the same texture (s13, the free slot; row c68) and multiply `SunVisibility()` per
pixel (`LandSunVisibility(w.position)`).

**Reflection pass.** The native client has no second world pass for water reflections (VFogWater's are screen space),
so nothing to skip; `OnWorldRender` is the only caller.

**CVars** (TerrainSunShadow.cpp, Video Options > Advanced > Shadows):
| CVar | Range | Default | |
|---|---|---|---|
| `shadowMask` | 0/1 | 1 | the pass and the water tap; needs `gfxTerrainShadow` 1 (the land texture) |
| `shadowSoft` | 0..4 | 1 | the penumbra: yards under the lit height to full dark, 16 x this (min 2). Low sun = long soft edge |

**Cost** (estimate, not measured: no rig run by the rules): one full-res pass; lit and sky pixels 2 taps and ~15
instructions, shadowed pixels 6 depth taps, 1-2 cascade taps, 1 land tap, ~60 instructions. About the cost of the
god-ray composite: well under 0.5 ms at 1440p on a mid GPU, so default on. The land texture is shared with the fog
(rebuilt only when the light turns or the camera moves 533 yd).

**Backends.** SPIR-V through Tools/VulkanShaders (Vulkan), ps_4_0 through build_shaders.py (D3D11; s5/s6 compare slots
as the fog), ps_3_0 (D3D9) compiled by fxc.

**Ceiling (`ponytail:` in VolumetricFogRenderer.cpp).** The mask is applied after the receivers, not read by them.
- The stock receivers keep their own cascade sampling, so the cascades are not softened or widened here (Legion's
  shadowSoft PCF). That needs the receivers to read the mask before they shade: a depth pre-pass (or two-phase opaque)
  plus a one-tap mask read in the terrain/WMO/M2 shaders. That replaces SHADOW_PORT_PLAN S4a/S4b (no per-receiver
  cascade rewrites needed) and is the next step.
- The ratio assumes stock lighting = ambient + diffuse * ndl (gamma space); specular and emissive parts are darkened
  with the rest. A stock receiver's own cascade shadow that does not go fully to ambient stays at its own strength in
  a mountain's shadow.
- WDL is 33 yd per vertex: mountain-scale shadows only. A cave interior under a mountain seen from outside is
  darkened by the sun's share (from inside, the interior fade removes it).
- Underwater frames take the mask too (it draws before the liquid in both branches).

## 2026-10-18: Legion post (survey items 10, 5, 9: depth users, FXAA, colour blindness)

### Depth-consumer registry (item 10, Legion `SetDepthUser` 0x006C4343, count at 0x013CCDDC)

`CGxDevice::s_depthUsers` is a bitmask of the scene-depth consumers this frame (CGxDevice.h). While it is 0, every
backend's `DeviceCopySceneDepth` (Vulkan, D3D11, both D3D9 through `d3d9scenedepth::Copy`) returns nullptr with
`*transient = true`, so no copy, blit or RESZ resolve runs and callers retry next frame. A bit instead of Legion's
counter: setting it twice is harmless, so a consumer can set it from its state every frame.

Registered now:

| bit | set where | on while |
|---|---|---|
| `DepthUser_Fog` | `VolumetricFog::SetDepthUsers` (DecideFrame, OnViewerLiquid) | vfog draws this frame (`s_active`) |
| `DepthUser_FadeStencil` | same | vfog draws and `vfogFadeEarly` and `vfogFadeStencil` |
| `DepthUser_Water` | `WaterArm` (cleared at its top, set once armed) | modern water draws in this liquid pass |
| `DepthUser_ShadowMask` | `VolumetricFog::OnOpaqueDone` | `shadowMask` wanted (`TerrainSunShadow::MaskWanted`) and the renderer ready |

The fog, fade and water users are inside vfog, and the copy was already demand-driven (only callers pay, and the Vulkan/D3D11/RESZ
"fresh" flags share one snapshot per frame), so this saves nothing measurable yet. What it adds is the contract: a
consumer that does not register gets no depth.

**Registering a new consumer** (screen-space shadow mask, SSAO, refraction, light buffer, soft particles):
1. add a bit to the enum in `Engine/Source/Gx/CGxDevice/CGxDevice.h` (`DepthUser_ShadowMask = 1u << 3`, ...);
2. once per frame, before its `DeviceCopySceneDepth` call, `CGxDevice::SetDepthUser(CGxDevice::DepthUser_X, on)`
   with `on` from its CVar state (and whatever else gates its pass). Nothing else.
The shadow mask is the first consumer outside vfog's `s_active` (it runs with vfog off), registered this way.

### FXAA (item 5) and colour blindness (item 9): one post pass

`VolumetricFog::PreparePost` (in `OnWorldDone`, which sees the world viewport) reads the CVars;
`RenderPost` (VolumetricFogRenderer.cpp, beside `RenderGrade`) draws in `OnWorldRendered`, after the colour grade and
the glow, before names (`WorldTextRenderBatch`) and the UI, so neither is anti-aliased or colour-shifted. It copies
the world viewport (the grade's bilinear `CopySceneBeforeFog`) and draws one quad, opaque, RGB, scissored to the world.
Runs whether or not vfog is on; skipped entirely when both features are off.

Shaders (ours, `Tools/CustomMpq/ShaderSrc/LegionPost.hlsl`, FXAA 3.11 quality after Lottes' public algorithm, flat
code with no dynamic branches): `LegionPost0` colour shift only, `LegionPost1` FXAA low (preset 12: 5 search steps,
edge threshold 0.166/0.0833) + shift, `LegionPost2` FXAA high (preset 39: 12 steps, 0.125/0.0625) + shift; subpix
0.75. Constants: c0 world viewport px, c10 FXAA (subpix, threshold, threshold min), c11-c13 the colour matrix rows.
The matrix is applied in linear light (gamma 2.2 decode/encode) and is the identity when colour blindness is off.

| CVar | range | default | |
|---|---|---|---|
| `ffxAntiAliasingMode` | 0-2 | 0 | 0 off, 1 FXAA low, 2 FXAA high (Legion's 3 = CMAA not ported) |
| `colorblindSimulator` | 0-3 | 0 | 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia |
| `daltonize` | 0/1 | 0 | 0 simulate the type, 1 correct for it |
| `colorblindWeaknessFactor` | 0-1 | 0.5 | severity: sim = I + f (M - I) |

Matrices: Machado, Oliveira and Fernandes 2009 at severity 1.0 (linear RGB); daltonize is Fidaner et al. 2005's error
shift, `I + E (I - sim)` with E = [[0,0,0],[0.7,1,0],[0.7,0,1]], folded into one 3x3 on the CPU
(`ColourShiftMatrix`). Legion's 11 types are not mirrored (its table is not used); `colorblindMode` (the stock UI text
labels CVar) is untouched and independent.

Options: Video Options > Advanced > Post Processing ("Anti-Aliasing & Colour").

## 2026-10-18: Legion refraction (survey item 7, 2.6: heat haze)

Legion's `refraction` writes UV offsets from `Particle_Refraction` particles into a distortion target and
`refractionapply` offsets a scene copy by them. 3.3.5a M2 particles have no refraction blend, so we tag emitters and
draw the haze ourselves (cheapest workable design, one pass, no distortion target):

- **Tagging** (`HotEmitter`, VolumetricFog.cpp): a blended particle element (type 4 in M2 passes 1 and 2, blend not
  opaque/modulate) whose model path or particle texture contains `fire flame lava magma brazier torch burn exhaust
  immolat inferno`, and neither contains `smoke steam mist fog cloud firefl firework candle frost` (the texture's
  "cool" word wins, so a fire model's smoke emitter is left out). Same idea as `groundDecals`' name test.
- **Sources** (`CollectRefraction`, in `OnOpaqueDone` after Animate built the pass lists): each emitter's
  `WorldBounds()` (world space, from its last draw, so one frame old), skipped when inverted (not drawn yet) or over
  50 yd; widened 15% and raised by 0.75 x its height; the nearest 12 within 80 yd (`refraction 1`) or 32 within 160 yd
  (`2`). `DepthUser_Refraction` (bit 5) is set only while there is at least one source, so no fire = no cost.
- **Draw** (`RenderRefraction`, VolumetricFogRenderer.cpp; `VolumetricFog::OnHeatHaze` right after `OnLiquidDone` in
  WorldFrame): above water only, after the liquids and the early fog composite, before the blended M2 pass, weather and
  barriers. One copy of the world viewport (`CopySceneBeforeFog`) and the scene depth; each box is projected on the
  CPU to a screen rect and drawn as one `LegionRefraction` quad scissored to it, opaque RGB. The shader reads the copy
  at an offset: rising two-axis sine noise in the source's yards (a second octave at `2`), an elliptical fade to the
  rect's edge, zero on pixels in front of the source (scene depth < its near depth, 1 yd ramp) and zero where the tap
  lands in front of it (no foreground smear). Amplitude ~2 cm per yard of flame height (0.5-4 yd) x
  `refractionStrength`, in pixels at the source's distance, capped at 12 px x strength.
- Pass-2 particles (the common case above water) are drawn before the haze, so the flames themselves shimmer a little
  with what is behind them; split-lighting (liquid plane) ones in pass 1 draw after it, sharp.

| CVar | range | default | |
|---|---|---|---|
| `refraction` | 0-2 | 1 | 0 off, 1 on (12 sources, 80 yd), 2 high (32, 160 yd, second noise octave) |
| `refractionStrength` | 0-4 | 1 | haze amplitude scale |

Options: Video Options > Advanced > Post Processing ("Heat haze", "Heat haze strength").
Shader: `Tools/CustomMpq/ShaderSrc/LegionRefraction.hlsl` (ours; s0 depth, s1 scene copy; c0-c3/c99 vf_common,
c10 rect px, c11 near yd / offset px / seconds / high, c12 rect size yd).

Known limits (ponytail): no lava liquid surfaces (only lava/magma particle emitters); none under water (that branch has
no `OnLiquidDone`); overlapping sources do not add up (each reads the undistorted copy, last drawn wins); a box the near
plane cuts (camera inside the fire) is skipped; names are tested per element per frame (cache on CM2Shared if it ever
shows in a profile); the tag word list is a guess, untested in game: check torches/braziers/spell auras and trim it.
