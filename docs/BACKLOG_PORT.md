# Backlog ports (FEATURE_BACKLOG.md items in the decomp)

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
