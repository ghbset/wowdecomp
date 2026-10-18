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
