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

## Cheap non-visual fixes F1-F5 (2026-10-18)

Branch `bl-fixes`, one commit per item. No shaders.

- **F1 tile stack fix (crash fix, no CVar).** `CMap::PreUpdateAreas` (Map.cpp, 0x007B5950) alloca'd 1024
  `CMapAreaEntry` with no bound check; Northrend has 1131 ADTs, so a large farclip (about 4500 yd and up)
  overran the stack. Now 4096 entries (the 64x64 maximum the clamped `gbChunkRect` can cover, 32 KB) plus
  `entryCount < kMaxEntries` checks on both fill loops. Source: wraithhost-lite `offsets/game/ADT.hpp`
  (patch of the `mov eax, 2000h` at 0x007B597F). A pure crash fix, so no CVar.
- **F2 liquid far cap (no CVar).** `CWorldScene::AddToDistanceBucket` (World.cpp, 0x00792DF0) returned for
  bucket >= 64, dropping liquid past 2133 yd; it now clamps to bucket 63. Source: wxl-renderdistance
  `LiquidCap.hpp` (its `far_liquid` toggle defaulted on; not carried over, nobody turned it off).
- **F3 terrain pool cap.** `CMapRenderChunk::UpdatePools` (MapRenderChunk.cpp, 0x007BA600) sized the
  terrain VB/IB pools from farclip^2 (about 0.9 GB at 10000 yd). The radius is now capped to what fits the
  budget (2 x (145 x stride + 768 x 2) bytes per radius^2); chunks that find no free block are skipped by
  `AllocBuf` as in stock (far chunks drop, no crash). CVar `terrainPoolMaxMB`: 128 on Wow32 (wraithhost-lite
  `terrain_pool_max_mb` default, about 3600 yd of pools), 0 = uncapped on Wow64 (no address-space pressure,
  so stock look). Changing it marks the pools dirty (`SetDirty`), rebuilt next frame. Control: Advanced >
  Loading & Gameplay > "Terrain memory budget (MB)" (0..1024, step 32).
- **F4 spell queue.** ClientExtensions.dll does NOT provide it to the decomp exes: tswow byte-patches stock
  Wow.exe to `LoadLibraryA("ClientExtensions.dll")` (ClientPatches.ts), the decomp has no such load, CE's
  hook writes a `call` at the stock address 0x00739773, and Wow64 cannot load the 32-bit DLL. Ported CE
  `Gameplay/SpellQueue.cpp` (= wxl-spellqueue): `Spell_C_GetSpellCooldownQueued` (Spell_C.cpp) answers
  "ready" when a player cooldown ends within the window (on-hold and pet cooldowns never pass), used by
  `CGUnit_C::ValidateSpellCast` (Unit_C.cpp, 0x00739650) in place of `Spell_C_GetSpellCooldown`. The server's
  SpellQueue (core `SpellQueue.Window` 150) holds the early cast; no packets involved. CVar
  `spellQueueWindow` (ms, default 150 = CE default, 0 = stock); CE's separate `enabled` folded into 0.
  Control: Advanced > Loading & Gameplay > "Spell queue window (ms)" (0..400, step 10).
- **F5 fast load + random loading screens.** Source wxl-loading `Loading.cpp`. `OsSleep` (Os/Win32 and Win64
  TimeManager.cpp, 0x0086B280) returns at once while `OsSetFastSleep(true)` is in force, set by
  `LoadingScreenEnable` (0x0040AB70) and cleared by `LoadingScreenDisable` (0x00409550), with the 60 s
  safety cap. CVar `fastLoad` (default 1, as wxl-loading). Random screens: CVar `loadingScreens`
  (default empty) = `<mapID>:<LoadingScreens id>,<id>;<mapID>:...`; `LoadSimpleBackgroundTexture`
  (0x00409ED0) picks one at random per load (falls back to the map's own screen if the id has no record).
  The plugin wrote the id into the live Map.dbc row; here it is picked locally, as that function is the
  row's only reader. No control for the list (string); "Fast loading" check on Advanced > Loading & Gameplay.

All CVars are registered in `CWorldParam::Initialize` (WorldParam_CVar.cpp); their C++ defaults hold before
registration. Unverified: nothing run in game. F3's skipped chunks at a small budget, F4 against the live
server, F5's busy-spin on async readers (same as the plugin) are untested in the decomp.

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

### Cata water pieces B and C: ripples, thickness fog, underwater tint (2026-10-18)

**Ripples (piece B).** No Cata simulation is ported: vfog's ripple sim (`VolumetricFogWaterRipples.cpp`, Gr1616F
r = height / g = previous height, 0.125 yd texels, 512^2 around the camera target, the same format as Cata's level 0)
is run by `CataWater::Arm` through `VolumetricFog::RippleFrame` (strength 1 only gates it; quality 2 = 512^2), with a
`FrameInputs` holding only what it reads (camera target, map id), so it runs with the fog off. waterStyle 0 is
untouched: the vfog water still calls `RippleFrame` from its own `Begin`, and Cata's `Arm` runs first only at style 1.
The PS adds wxl-water's `RippleSlope` (W1 8.2, one level, no wide ship level) to N2 in both Above and Below:
s11 = the map, c29 = (window origin - camera xy, 1 / extent), c30 = (1 / texels, step fraction, P15; 0 when the sim
has no live map, s11 then gets the colour copy as a dummy). While the Cata ripples run, the client's splash / wake
sprites are hidden as for the modern water (`WaterHidesClientRipples` asks `CataWater::DrawsRipples` first).
**CVar** `waterCataRipples` (default **1**: the sim only runs while units are in water near the camera and is shared
code already shipping with vfog water; the design ships A+B together), Advanced > Water "Cataclysm ripples".

**Thickness fog (piece C).** `Arm` resolves Cata's fog A once per pass (wxl-water `ResolveThicknessFog`,
water-stage2-fog-research.md 3.1): above water, the underwater LightParams of the base area light plus every area
light whose falloff holds the light reference point, far to near, each by its falloff (`DayNight::BlendAreaLightColors`
/ `BlendAreaLight` with underwater = 1, so storm blend and the game's own fog-mode rewrite come with it); under water,
the game's current bands (already the underwater fog, liquid light included). Then Cata FUN_00648190: end = min(fog
end, far clip), start = max(multiplier, 0) x end, exponent = fog rate x 2; c24.xyz = (-1/(end-start), end/(end-start),
exponent), c25.rgb = the fog colour (LightIntBand 7), c23.y = 1. Zone lights, the override light and WMO fog are left
out, as in wxl-water.

**Underwater tint (piece C).** Cata FFXUnderWaterLow as wxl-water draws it: with style 1 and the camera in a water-
material liquid, `CataWater::OnWorldDone` (called in `CGWorldFrame::OnWorldRender` before the chat bubbles and vfog's
`OnWorldDone`) copies the world viewport and draws `CataWaterTint` over it: frame x (1 + (fog colour / its brightest
channel - 1) x 0.3) x (1 - 0.45 x smoothstep(0.2, 2, |ndc|^2)). No caustics (separate backlog item). No CVar: it is
part of Cata's under-water look, and costs one copy + one quad only while submerged.

**Files.** `WorldClient/CataWater.{h,cpp}`, `WorldClient/VolumetricFogWater.cpp` (`WaterHidesClientRipples`),
`Ui/WorldFrame.cpp` (tint call); `ShaderSrc/CataWaterPs.hlsl` (ripples), new `ShaderSrc/CataWaterTint.hlsl`,
`build_shaders.py` (s11 on Above / Below, `CataWaterTint`), rebuilt `.bls` (ps_3_0 + ps_4_0); the checkbox in
`VideoOptionsAdvanced.lua`. Vulkan: whole set re-translated and spirv-val'd (see the commit).

**Unverified.** Nothing ran in game. The ripple slope sign against the normal map's (wxl-water's/Cata's convention,
used as is on vfog's map; vfog's own shader negates its slope into N), the ripple height scale (vfog's footprints vs
Cata's perturbTex splats: P15 1..2.5 here vs vfog's 3 x 0.5), the fog-rate exponent when the game's fog mode is not
the rewriting one (then the exponent is 2), and whether the world viewport is still current where the tint draws.
## Soft particles (2026-10-18)

Backlog #4, ported from wxl-light `src/Soft.cpp` (a35) and its shader rewrite (`tools/rxrewrite.py`
`rewrite_soft_vertex` / `rewrite_soft_pixel`). Blended M2 particles and ribbons fade where they come within a fade
distance in front of the scene behind them: by alpha for the alpha and additive blends, by colour towards the
blend's neutral for the others (NoAlphaAdd to 0, Mod to 1, Mod2x to 0.5). Opaque and alpha-keyed emitters stay hard.

- **Depth.** `SoftParticles::BeginPass` / `EndPass` (new `WoW/Source/WorldClient/SoftParticles.h/.cpp`) wrap both
  `scene->Draw(M2PASS_1)` and `scene->Draw(M2PASS_2)` in `CGWorldFrame::OnWorldRender`, in both branches (above and
  under water): the scene depth as drawn so far, like the plugin's `MidWorld` copy at each blended pass. It is
  `DeviceCopySceneDepth`, so no second copy: Vulkan (`frame::DepthWritten`) and D3D11 (`m_sceneDepthFresh`) hand
  back the snapshot they already hold when nothing wrote depth since. Above water the pass-2 copy is the one
  shadowMask takes in `OnSolidDone` and the pass-1 copy is vfog/water's after the liquid (`OnLiquidDone`), when
  those are on. It registers `CGxDevice::DepthUser_SoftParticles` (bit 7) every pass from the CVar.
- **Draw.** `g_softParticleDraw` (Engine, ParticleSystem2.h) is armed for the pass and called right before/after
  the particle draw in `CParticleEmitter2::RenderParticleVertices` (only when `CParticleEmitter2::s_soft`, set by
  `CM2SceneRender::DrawParticle` for M2 blend >= 2) and the ribbon draw in `CRibbonEmitter::Render`. It keys the
  fade by the draw's Gx blend, swaps in our pixel shader, binds the depth on s12 and puts c30 / s12 back after.
  The `particleGpuQuads` path is covered: the Vulkan compute pass only expands quads into vertices, the draw then
  goes through the same `RenderParticleVertices` with the stock VS and our PS.
- **Shaders.** Only the pixel shader is ours: `Tools/CustomMpq/ShaderSrc/SoftParticle.hlsl`, built as
  `SoftParticle0` (= `Combiners_Mod:0`, the particle/ribbon PS without shadow) and `SoftParticle1` (= `Combiners_Mod:8`,
  texkill on `c2.w`) for ps_3_0 and ps_4_0 (`build_shaders.py`). The plugin rewrote the stock vertex shaders to pass
  view depth in FOG.y; the Vulkan translation carries FOG as one float, so instead the vertex fog row c30 is set to
  `(-1/16384, 1, 1)` for the draw (FOG = 1 - z / 16384, every CDiffuse_T1 / Color_T1 permutation computes FOG the
  same way) and the PS redoes the stock fog from the real c30 (c43). Screen position is `VPOS`: the Vulkan emitter
  (`Tools/VulkanShaders/bls/emitter.py`) now maps `dcl vPos` to `gl_FragCoord.xy - 0.5` (shader-xlate already did).
  Constants: c40 (1 / fade yd, rgb fade, alpha fade), c41 (neutral rgb, 16384), c42 (1/w, 1/h, depth decode as
  vfog's c3), c43 (real c30.xyz, the world viewport's maxZ: deeper is horizon/sky, never fades).
- **Reflection pass.** Nothing to skip: the decomp's water reflections are screen-space, no second model pass.
- **CVars** (WorldParam_CVar.cpp): `softParticles` (default 1: the plugin's default; the copy is usually one vfog
  or shadowMask already made, and the cost is one point fetch per particle pixel), `softParticlesDistance` (yards,
  0.05..20, default 1 = the plugin's `soft_distance`). The plugin's `soft_debug` view was not ported.
  Advanced > World Effects: "Soft particles" check and "Soft particle fade (yards)" slider (0.1..5).

Not covered, by design or for now: shadowed particle permutations (`Combiners_Mod` 1..3, `s_shadowValue != 0`) keep
the stock shader and stay hard; D3D9 without MSAA (live INTZ depth, no copy) has no soft particles; a world drawn
to a target of another size than the depth (the plugin's size check) stays hard.
Unverified: nothing run in game; the view-depth sign/units match between the fog row and the vfog depth decode by
reading, not by test.

### Cata water piece D: planar reflection (2026-10-18)

**What.** Cata's p = 1 reflection (W1 section 4, wxl-water `caustics` 17cf9af): with `waterStyle` 1 the water mirrors
the sky, the terrain and the WMO groups through the chosen water plane. Native: no bucket snapshots, the stock draws are
re-run. `CataWater::RenderReflection` is called in `CWorldScene::Render` after the culling and `ApplyFogState`, before
`RenderChunks`: it draws into `CataWaterReflection` (Argb8888, world viewport x `waterCataReflScale`; every backend
pairs a colour target with a scratch depth of its size), viewport 0..1, cleared to the fog colour, with
view = R x view (R: z -> 2h - z, in the camera-relative space the view takes) and the GL-form projection's x column
negated, so the mirrored view keeps the main view's winding and no cull state is touched (the shader reads the target
flipped in x, as wxl-water). Order: `DayNight::RenderSky` (plain mirrored projection), depth clear, then the native
projection (`m_projNative`, D3D form) gets an oblique near plane on the water (Lengyel, wxl-water `BuildOblique` minus
its z flip) and `RenderChunks` (mode >= 2) and `RenderMapObjDefGroups` (mode >= 3) run. The oblique is written into
`m_projNative` directly, because every backend's `XformSetProjection` rebuilds z from c2 / d2 alone; `CShaderEffect::
UpdateProjMatrix` is re-run after each change. wxl-water's oblique self-test runs once on first use (off on a mismatch).
While `CataWater::Reflecting()`, `RenderChunksSinglePass / Solid / ZoneDebug` and `RenderMapObjDefGroups` leave the
sort-table lists (and the groups' portal frustums) in place for the main view.

**Plane.** wxl-water `PlanePick.hpp` ported into CataWater.cpp: each terrain-water instance of the Cata pass (WMO
liquids are skipped: their sphere is the group's, not the water level) adds its sphere weighted r^2 / d^2 into 2 yd
height buckets; the heaviest body wins, the current one holds unless another is 1.5x heavier. Picked in `Arm` (frame N),
used by `RenderReflection` (frame N+1). Skipped: no plane, camera less than 0.1 yd above it (or under water), camera
inside a WMO or no exterior in view.

**Shader.** `CataWaterAboveP` = CataWaterPs with `PLANAR`: s14 = the target, c31 = (plane z - camera z, 0.5); the
lookup is wxl-water's (1 - u - bend.x, v + bend.y), bend = N.xy x 0.06 x saturate(tr + 0.25), and it replaces the SSR /
fog-colour reflection where the water is within 2 yd of the plane, fading out to 4 yd. Other water keeps the SSR.

**Pass hooks.** The reflection only runs the stock chunk / WMO / sky draws. The vfog fog, sun shadows (shadowMask),
AO, depth-user copies (`OnOpaqueDone / OnSolidDone / OnLiquidDone / OnWorldDone`, Cata `Arm`), the falling leaves
(`RenderLiquidPass`), tree sway and grass wind (M2 scene / detail doodads, not drawn), and the occlusion queries
(`OcclusionCull::IssueQueries`, after `RenderMapObjDefGroups` in `Render`) are not reached, so none sees the mirrored
camera. No doodads, M2s, particles or detail doodads (Cata's reflectionMode draws none of them either).

**CVars.** `waterCataReflection` (default **3** = sky + terrain + WMO, Cata's max reflectionMode: it only costs while
`waterStyle` 1 is chosen, which is off by default, and the mirror is what makes that style look like Cata; 0 = SSR only,
1 = sky, 2 = + terrain), `waterCataReflScale` (default 0.6, wxl-water's, clamped 0.25-1). Advanced > Water
"Cataclysm reflections" (dropdown) and "Reflection resolution" (slider).

**Files.** `WorldClient/CataWater.{h,cpp}`, `WorldClient/CWorldSceneRender.cpp` (the call), `WorldClient/World.cpp`
(list-keeping guards), `ShaderSrc/CataWaterPs.hlsl` (`PLANAR`), `build_shaders.py` (`CataWaterAboveP`), new
`Shaders/Pixel/ps_3_0` + `ps_4_0/CataWaterAboveP.bls`, `VideoOptionsAdvanced.lua`.

**Unverified / limits.** Nothing ran in game. The reflection reuses the main view's culled set: terrain chunks and WMO
groups culled or occluded for the main view (tall things above the top of the screen) are missing from it, and the WMO
groups' portal frustums are the main view's. The far horizon (`RenderHorizon`, its own projection) is not drawn, so
distant land past the far clip mirrors as sky. Under vfog (`StockFogReplaced`) the mirrored terrain may come out
unfogged. The oblique far plane is tilted, so terrain near the far clip can be cut in the mirror. RT orientation is
assumed to match the colour copy's on all three backends (the vfog/portrait RTs do). Cost unmeasured (Cata: 1.5-2.7 ms).
