# Feature backlog ports (FEATURE_BACKLOG.md items)

## Lava/slime ProcMagma (2026-10-18)

Backlog #10. Cata 4.3.4's procedural magma (`ProcMagma`, waterDetail >= 1) for lava, slime and green lava,
in place of the stock `lava.%d` flipbook. Ported from wxl-water (`caustics` branch, the newest: Water.cpp
`kLavaRows` / `SubstituteLava` / `UploadLavaConstants`, Shaders.hpp `kLavaPixel`), checked against
wowdecomp15595 `Liquid::CMaterialMagma::SetupRender` (LiquidMaterial.cpp; 4.3.4 merged MaterialMagma.cpp
into it).

- `CMaterialMagma::Render` (0x008A6090, MaterialMagma.cpp): `BindProcMagma` runs after the stock state is set.
  It finds the LiquidType id by scanning `CSettingsBank::s_settings` (the settings carry no id), takes that
  row's Cata Float[2..11]/Int[2] from the hard-coded `kProcMagmaRows` (the 3.3.5a DBC has none of them; ids 3,
  4, 7, 8, 11, 12, 15, 19, 20, 21, 121, 141; an unknown id = the ground magma values, slime set for SoundBank 3),
  binds the noise on s0 and the three layers on s1..s3, uploads PS c6..c15, sets the tex transform to
  identity and swaps in the pixel shader `LiquidProcMagma`. The stock `vsLiquidMagma` stays (no custom VS on
  any backend): Cata's six VS texture matrices and the noise A/B lerp field are evaluated per pixel instead
  (linear, so the same; the lerp field is smooth at 0.05 scale). If the shader, the noise or any layer is not
  there (yet), the stock draw runs. The FFP material (0x008A6350) is untouched.
- Matches Cata where wxl-water approximated: noise scales carry `WorldCoordScale()` (1.0 in 3.3.5a), scrolls
  use the client's own `GetTexScrollMtx`; wxl-water's `lavauv`/`lavauvwmo`/`lavanoise` calibration knobs and
  debug views were left out (their defaults are what this does). P10 is dead in Cata too.
- Noise volume: Cata `XTextures\FX\noise.blob` (128x128x16 A8R8G8B8, from wxl-water/textures). The Gx API
  has no volume textures, so it is packed at load into a 520x520 atlas of 4x4 slices with 1-texel wrapped
  borders (the VolumetricFog atlas layout); the shader lerps slices, which equals Cata's trilinear wrap.
  Depth sweeps once every 600 s as in Cata.
- CVar `liquidProcMagma` (default 1: one pixel-shader swap, 7 texture fetches per lava pixel, no passes or targets;
  the stock path whenever an asset is missing). Advanced options > Water: "Cataclysm lava and slime".
- Files: WoW/Source/LiquidSystem/MaterialMagma.cpp, Liquid.h, WoW/Source/WorldClient/WorldParam_CVar.cpp,
  Tools/CustomMpq/ShaderSrc/LiquidProcMagma.hlsl (+ build_shaders.py entry, committed ps_3_0/ps_4_0 .bls),
  Tools/CustomMpq/Textures/ProcMagma/ (Magma0..2, Slime0..2, LavaGreen0..2 BLP2 DXT1 from Cata art.MPQ,
  noise.blob), VideoOptionsAdvanced.lua.
- custom.MPQ must carry `Shaders\Pixel\ps_3_0\LiquidProcMagma.bls`, `Shaders\Pixel\ps_4_0\LiquidProcMagma.bls`,
  the translated SPIR-V record of it (regenerate VulkanShaders), and `Textures\ProcMagma\*` (picked up by
  build_custom_mpq.py's Textures folder).
- Unverified: never run in game. The uv scale (raw stored uv, as wxl-water's default `lavauv = 1`; Cata's
  own uv sources disagree by up to 4x, lava-research-L1 section 9), WMO lava scale, and the noise channel
  order (wxl-water's default) are taken from the plugin, which ran in game.
