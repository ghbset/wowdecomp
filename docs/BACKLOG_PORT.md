# Backlog ports (decomp vfog branch)

## WMO normal repair (2026-10-18)

Backlog #3. Port of wxl-light `src/Normals.cpp` (which hooked `Wmo.GroupWalk`, 0x007D7F50, and edited the
group's file buffer before the walk). Now `CMapObjGroup::RepairNormals()` in `MapObjRead.cpp`, called from
`CMapObjGroup::CreateDataPointers` once MOPY/MOVI/MOVT/MONR pointers are set, editing MONR in place before the
vertex buffers are built.

Heuristic, unchanged from the plugin: per vertex, accumulate the area-weighted face normals of the rendered
triangles round it (faces with `m_materialId == 0xFF`, collision only, are skipped). Rebuild the vertex normal
to that sum, normalised, only when all of: the largest face round it is >= 20 sq yd; the faces agree
(|sum| >= 0.8 x sum of areas, i.e. near one plane); the stored normal is more than 25 degrees off (cos < 0.906).
No smoothing across creases: a vertex whose faces disagree is left alone, as are small detail and NaN normals.
Bail-outs as the plugin: normal count != vertex count, fewer polys than triangles, any out-of-range index.

- CVar `wmoNormalRepair` (default 1: CPU-only, once per group load, cheap; the plugin shipped it on).
  Applies at load, so a change shows after a relog or once the buildings reload.
- Advanced options > World Effects: "Repair building lighting".
- Files: `WoW/Source/WorldClient/MapObjRead.cpp`, `MapObjGroup.h`, `WorldParam_CVar.cpp`,
  `Tools/CustomMpq/Interface/FrameXML/VideoOptionsAdvanced.lua`.
- Unverified: not run in game (dock wedge at Ratchet under a low sun is the test). The plugin's per-group log
  line was dropped (diagnostics only).

## Tree sway (2026-10-18)

Ported from wxl-treewind `src/TreeWind.cpp` (branch `leaves`, the shipped build), sway only.

- **What:** every map-placed living tree and bush tilts rigidly about its origin (the trunk base) by a
  fraction of a degree, driven by wxl-treewind's travelling two-wave field (primary + cross swell, slow
  gust, per-tree phase/amplitude hash, constant downwind lean). Trees and bushes keep their own wind rows
  (the plugin's .ini defaults, `kTreeWind` / `kBushWind`).
- **Filter** (`M2TreeSway::Classify`, the plugin's word lists verbatim): only models with `m_flags & 0x20`
  (map doodads); dead/bare/burnt/stump/log words reject (`blight` except Dragonblight); a tree word without a
  not-tree word (streetlamp, spine, grass, rock, ...) and no own lights is a tree; bush words win over the
  tree row; trees taller than 200 world yards stay rigid.
- **Where:** `M2TreeSway::World(model)` replaces `m_worldTransform` in the world*view products of
  `CM2Model::AnimateMTSimple` (0x0082E140), `AnimateMT` (0x0082F0F0) and `AnimateSM` (0x00831990, the
  shadow-map caster animate). So the drawn palette and the sun shadow casters (S1–S3, which gather through
  `GatherModelBatchesShadowMap` → `AnimateSM` or reuse this frame's palette) carry the same tilt.
  `m_worldTransform` itself is never written, so the plugin's pristine-placement cache and its eviction are
  gone. Hit tests on un-animated models (`SphereTestModels`) and culling bounds stay untilted (sub-degree).
- **CVars:** `treeSway` (1; cheap: a memoized name test plus a few sin/cos per swaying doodad per animate,
  no GPU cost; the plugin ships on) and `treeSwayStrength` (1, 0..3; scales the amplitude, cross amplitude
  and lean, 1 = the plugin's defaults). Video Options > Advanced > World Effects: "Trees sway in the wind",
  "Tree sway amount".
- **Not ported:** the overlay panel and .ini (CVars instead), the knobs whose defaults are off
  (tall-thin gate, max distance, distance fade), separate bush on/off. Falling leaves is a separate item:
  it can reuse `M2TreeSway::KindOf` and `Evaluate` (the per-tree `State`: tilt, seed, wind direction).
- **Files:** `Engine/Source/Model2/M2TreeSway.h/.cpp` (new), `M2Model.cpp` (3 lines),
  `WoW/Source/WorldClient/WorldParam_CVar.cpp`, `Tools/CustomMpq/Interface/FrameXML/VideoOptionsAdvanced.lua`.
- **Check:** `clang++ -std=c++17 -I. Tools/TreeSwayCheck/check.cpp -o /tmp/tsc && /tmp/tsc` (classifier
  cases, tilt bounds, rotation orthonormal).
- **Unverified:** not run in game; the look (direction, amount) is untested. The draw paths read the
  tilt through `m_matrixF4` / `m_boneMatrices` (checked in M2Scene / CM2SceneRender), the same products the
  plugin's placement edit fed.

## Falling leaves (2026-10-18)

Ported from wxl-treewind `src/Leaves.cpp` (branch `leaves`, the shipped build; user: "looks pretty good"), on top
of Tree sway.

- **What:** each of the 32 nearest swaying trees (within 70 + 20 yards, taller than 3 yards) owns up to 48 leaf
  slots. A leaf starts in the crown, falls to the base at 1.3 yd/s, flutters, spins, turns edge-on now and then
  and drifts downwind; a tree sheds more while its gust is up. There is no per-leaf state: TreeLeavesVS works out
  each leaf from (tree constants, slot randoms, time) and an idle slot collapses outside the clip volume. One
  non-indexed draw of a static 9216-vertex buffer per frame, 4 vertex constants per tree (c10..c137).
- **Colour:** the plugin's name guess (palms/mushrooms/cacti shed nothing, conifers and snowy trees a few, autumn,
  red, yellow and night elf violet tints), then the average opaque texel of the tree's leaf texture (the one named
  leaf/branch/needle/..., else the most colourful, bark/trunk penalised). The plugin locked the D3D9 texture; here
  the same small mip (levels - 3) is read from the BLP file with `CBLPFile` on the main thread, once per tree, so
  it works on every backend. Lit by ambient + 0.7 x diffuse (`g_dnInfo.m_bands`).
- **Fog / order:** drawn at the entry of `CWorldScene::RenderLiquidPass` (0x00790A80, the plugin's hook), in both
  the above-water and in-water orders, opaque with depth writes and a `clip` at alpha 0.5 (the plugin's ALPHAREF
  128 test), colour write rgb only. So the water and the vfog early composite (`OnLiquidDone`, which copies the
  depth afterwards) treat a leaf as a solid model, exactly as the plugin did with wxl-vfog. The vertex shader
  also applies the stock fog from `g_dnInfo.m_fog` as the plugin did; with vfog on that fog is pushed out of
  range, so vfog alone fogs them. Not drawn in any reflection pass (the decomp water reflections are SSR: no
  scene re-render).
- **Feed:** `M2TreeSway::World` calls `TreeLeaves::NoteTree` for `Tree`-kind models with the unswayed transform,
  the sway seed, `kTreeWind` gust/speed and the primary wind direction (the plugin's call after the tilt in
  ApplySway). It runs on the animate workers, so the 32-row table is behind a mutex; the texture names are copied
  when a tree takes a row. A tree keeps shedding for 60 s after it was last animated, then fades over 1.5 s.
- **CVars:** `treeLeaves` (1; one draw of ~3k triangles, mostly collapsed, and a 32-row table, the plugin ships
  on; needs `treeSway`) and `treeLeavesDensity` (18, 1..48: the plugin's PerTree). The plugin's other .ini keys
  are constants. Advanced > World Effects: "Falling leaves", "Falling leaf amount".
- **Shaders:** `Tools/CustomMpq/ShaderSrc/TreeLeavesVS.hlsl` (vs_3_0 / vs_4_0) and `TreeLeavesPS.hlsl` (ps_3_0 /
  ps_4_0), built by `build_shaders.py TreeLeavesVS TreeLeavesPS` into `Tools/CustomMpq/Shaders/Vertex/vs_3_0`,
  `Vertex/vs_4_0`, `Pixel/ps_3_0`, `Pixel/ps_4_0`. The plugin's three float4 vertex inputs are repacked into the
  stock GxVBF_PNCT2 format (position, normal, colour, two texcoords), so no new vertex format on any backend.
  The z remap the plugin needed for the engine's OpenGL-form projection is gone: the VS uses
  `GxXformViewProjNativeTranspose` (camera-relative view x native projection), so positions go in camera relative.
- **Files:** `WoW/Source/WorldClient/TreeLeaves.h/.cpp` (new), `Engine/Source/Model2/M2TreeSway.cpp`,
  `WoW/Source/WorldClient/World.cpp` (1 call), `WorldParam_CVar.cpp`, `VideoOptionsAdvanced.lua`,
  `Tools/CustomMpq/ShaderSrc/build_shaders.py` + the two .hlsl.
- **Unverified:** not run in game on any backend. The native projection path (Sand's) and the camera-relative
  view are read from the code, not seen on screen; the leaf colour read from BLP files is untested.

## Grass wind (2026-10-18)

Ported from wxl-grasswind (`/root/wxl-grasswind`, main, src/GrassWind.cpp). The plugin swapped the 6 stock
`Shaders\Vertex\vs_3_0\DetailDoodad.bls` blobs at create time for copies with an injected motion block and wrote
its constants into the detail-doodad constant block. Natively the whole shader is ours:

- `Tools/CustomMpq/ShaderSrc/DetailDoodad.hlsl`: the stock grass VS, all 6 permutations (3 * point lights +
  shadow level, `CDetailDoodad::SetupVertexShader`), rewritten in HLSL, plus the plugin's motion block
  (`kMotionInject`) as HLSL: two travelling sine waves and a downwind lean bend each blade by `1 - uv.y`
  (anchored at 0.3, squared, per-blade variance and phase jitter, view-distance fade), and blades inside 2 yd of
  the player lean away from them (height cone above the player's feet). Same constants, same layout, moved from
  the plugin's c14-c21 (the point-light registers of permutations 3-5) to **c35-c42** (after the shadow matrices
  c23-c34).
- `build_shaders.py` gained `MULTI` (several permutations in one container, stock record masks a/b) and builds
  `Shaders\Vertex\vs_3_0\DetailDoodad.bls` (fxc, D3D9 and the Vulkan translation's source) and
  `Shaders\Vertex\vs_4_0\DetailDoodad.bls` (shader-xlate, D3D11). Both are committed; build_custom_mpq.py packs
  them over patch-R's SM4 copy, and translate_shaders.py picks up the vs_3_0 one from Tools/CustomMpq/Shaders.
- `DetailDoodad.cpp`: `UpdateGrassWind` (the plugin's `WriteFrameConstants` with its default settings) runs at the
  end of `InitializeShaderConstants`, once per detail-doodad pass; phases advance by frequency * dt from
  `FrameTime::s_curTimeMs`. The eye comes from the pass's own view matrix and `s_activeWorldView`, so the water
  reflection pass is correct too. `SetupVertexShader` uploads c35-c42 with every batch after the stock block.
- CVars (WorldParam_CVar.cpp): `grassWind` (default 1), `grassWindStrength` (0..4, default 1: scales both wave
  amplitudes and the lean), `grassParting` (default 1). Default on: a few dozen VS instructions on grass only, and
  off means zero constants, the stock blade positions. Advanced options: page "Grass" (before Debug).
- Not ported: the plugin's runtime disassemble/assemble fallback and blob hashing (not needed with our own
  source), its Settings.ini, the vs_2_0 profile (the plugin skipped it too; vs_2_0 clients keep still grass).

Unverified: nothing has run in game. The HLSL is a rewrite of the stock asm, checked by reading the disassembly,
not by an A/B render; with `grassWind 0` it should be pixel-identical to stock. Player position is
`CGPlayer_C::GetPosition` (world); on a transport it may not match the grass frame.

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
