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
