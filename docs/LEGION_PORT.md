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
