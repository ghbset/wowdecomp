# Legion feature port (survey items from /root/d3d11-port/LEGION_SURVEY.md)

## 2026-10-18: Legion stream

Survey 2.11 (Legion `worldLoadSort` / `componentTexLoadLimit`). CPU and async-loader only, every API.

- **`worldLoadSort`** (0/1, default 1). Each `CAsyncObject` carries the world x/y of the thing it loads
  (`AsyncLoadOrigin` scope in `Engine/Source/Services/AsyncFile.h`; tagged in `CMap::CreateDoodadDef` (ADT and WMO
  doodads), `CMap::CreateMapObjDef` (ADT WMOs) and the WMO group reads in `CMap` def-group prep). Loads issued from a
  postload callback (skins, textures, WMO groups after the root) inherit the parent's position. When a disk reader
  pops the queue (`AsyncPickNearest`, under `g_queueLock`) it takes the entry nearest the camera
  (`World::PrepareAreaOfInterest` → `AsyncFileReadSetViewPos`) among the first 64 sharing the head's priority, so the
  stock priority bands (WMO root 124, skins 125, models/groups 126, textures 129-131) still decide first.
  Untagged loads, the object `AsyncFileReadWait` is blocking on, and anything queued over 120 frames keep stock
  head-first order, so nothing starves. ADT tiles were already queued nearest-first by stock (`qsort` in CMap).
  The streaming (net) queues are untouched.
- **`texUploadLimit`** (count per frame, default 24, 0 = unlimited). The per-frame poll (`EVENT_ID_POLL`) runs at most
  this many `AsyncTextureSuccessCallback`s (BLP parse + `TextureAllocGxTex` + `GxTexUpdate`); further finished
  textures move to a defer list and run first on the next frames. Everything else in the post list (models, WMOs,
  ADTs) still runs that frame. `AsyncFileReadWait` and `AsyncFileReadWaitAll` (loading screens) poll uncapped, so
  waits and loading always drain; `AsyncFileReadIsReading` / unload see the defer list.
  Why 24: the texture read budget is 4 MB in flight (`s_asyncLoadBufferUsed`), roughly 50-100 typical world BLPs, so a
  full burst spreads over 2-4 frames while sustained streaming (well under 24 x 60 = 1440 textures/s) is never held
  back. Deferred textures keep their read buffers, which throttles new texture reads a little: natural back-pressure.
- Options: Advanced page, "Load nearest first" (check) and "Textures per frame" (8 / 16 / 24 / 48 / Unlimited).
- Not done: Legion `worldPreloadSort` / preload distances (no preload pass in 3.3.5a beyond the area-of-interest),
  character `componentTexLoadLimit` (component textures are composited on the CPU, not uploads). Untested in game.
