# Backlog ports (features ported from our plugins into the decomp)

## Cata water (2026-10-18)

Piece A of the Cata-style water design (`waterStyle`): Cataclysm 4.3.4's max-quality water surface as a second water
look beside the volumetric fog's modern (coa) water.

**Ported from** wxl-water (`/root/wxl-water`, branch `caustics` @ 17cf9af): `src/Shaders.hpp` `kVertex` / `kPixel`
(itself a port of Cata's ProcWater VS and ProcWaterAbove / ProcWaterBelow PS, decoded in
`/root/wxl-wowforever/water-research-W1-cata.md` sections 7, 8 and 10) and `src/Water.cpp` `kCataRows` (Cata's
LiquidType P3..P15 per 3.3.5a row id, `/root/wow434/water-assets/README.md`). Textures `XTextures\ocean\newoceanbump.blp`
and `XTextures\foam\waterchop7bw.blp` from `/root/wxl-water/textures` (Cata data).

**CVar** `waterStyle` (int, default **0**): 0 = modern (today's behaviour: the vfog water, still gated by `vfogWater`
and the fog being active), 1 = Cataclysm. Default 0 because it is a different look, not an upgrade (the design's
verdict), and it is unverified in game. Video Options > Advanced > Water: "Water style" dropdown (Modern /
Cataclysm). The coa sliders on that page are not greyed out at 1 (the page has no dependency mechanism); they simply
do nothing then.

**How it draws.** `Liquid::CRenderer::Flush` (transparent pass) calls `CataWater::Arm` before the first draw: with
style 1 and a water-material instance (LiquidType MaterialID 1) in the pass, it copies the world viewport's colour
(`GxTexCopy`) and the raw scene depth (`DeviceCopySceneDepth` -> R32F through `VFogDepthCopy`), and vfog's
`WaterArm` is skipped. `CMaterialWater::Render` and `CMaterialWaterNoSpec::Render` (no highlight) call
`CataWater::Bind` after `SetShaderConstants`, in place of the stock shader binds: our VS + PS, the normal map (s9),
foam (s10), colour copy (s12), depth copy (s13), VS c50..c54 and PS c16..c28 (register map in the shader headers).
Bind returns false (stock look) for rows that are not the water material and while a texture is still loading.
Stock fog: when vfog has pushed it out (`StockFogReplaced`), VS c4 is set to no fog (vfog fogs the water after the
liquid pass). Depth write is unchanged (vfog's `LiquidDepthWrite` already forces it while vfog is active).
Under water (`World::SceneCamLiquidStatus`), the surface takes `CataWaterBelow`.

**Shaders** (`Tools/CustomMpq/ShaderSrc`): `CataWater.hlsl` -> `CataWater` (vs_3_0 / vs_4_0, the first custom vertex
shader), `CataWaterPs.hlsl` -> `CataWaterAbove`, and with `BELOW` `CataWaterBelow` (ps_3_0 / ps_4_0). Differences from
wxl-water: no far sea, caustics, sun / land shadow lookups, debug views, strength / reflection-fade knobs, or
last-frame depth reprojection (the copy is this frame's, taken before the first water draw, so the stale-texel
handling is reduced to "something in front of the water = open water"); no VPOS (the Vulkan translator refuses
misc-type registers): the VS passes clip xy and the PS divides; the light direction, diffuse and specular colours are
PS constants instead of varyings. Reflection = Cata p = 2: one-tap screen-space reflection 128 yd along the
flattened grazing ray in the colour copy, faded at steep rays and the screen edge, the fog colour where it misses
(W1 8.4; Cata reads a 512^2 blurred copy, here the full-res copy).

**Vulkan translator.** The first custom VS (`CataWater`) translates unchanged. `CataWaterAbove` uses `fwidth` (dsx / dsy),
which the translator refused: `Tools/VulkanShaders/bls/emitter.py` gained `r_dsx` / `r_dsy` (dFdx / dFdy; the VS
negates gl_Position.y, so the window orientation is D3D's). The whole set re-translated: 1636 / 1636 records,
1638 modules spirv-val clean.

**Hooks left for the follow-up pieces.** Piece B (ripples): the `piece B` comment in `CataWaterPs.hlsl` where the slope
is added to N2, sampler s11 free, Cata's P15 is in the row table. Piece C (thickness fog): the shader already folds
Cata's AboveFog into the refraction; `CataWater::Bind` uploads strength 0 (c23.y) and an empty fog A (c24.xyz, c25),
fill them from the zone's underwater fog. Piece D (planar reflection): the `piece D` comment where `fallback` is
chosen, sampler s14 free.

**Files.** `WoW/Source/WorldClient/CataWater.{h,cpp}` (new), seams in `LiquidSystem/LiquidRenderer.cpp`,
`LiquidSystem/MaterialWater.cpp`, CVar registration in `WorldClient/VolumetricFog.cpp`, shutdown in
`WorldClient/VolumetricFogRenderer.cpp`; `Tools/CustomMpq/ShaderSrc/{CataWater,CataWaterPs}.hlsl`,
`build_shaders.py` entries, the built `.bls`, `Tools/CustomMpq/XTextures/**` and the `XTextures` folder in
`build_custom_mpq.py`; the dropdown in `Interface/FrameXML/VideoOptionsAdvanced.lua`.

**Unverified.** Nothing has run in game. In particular: the view-depth decode (vfog's formula on `m_projection`
during the liquid pass), the screen uv from the clip position on each backend (D3D9 half-pixel), the normal map's
channel layout (wxl-water reads `.xy` as 3.3.5a decodes the BLP), the light direction sign, and how it looks
next to vfog's fog composite.
