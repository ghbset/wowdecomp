# Legion 7.3.5 feature ports into the 3.3.5a decomp client

## 2026-10-18: Legion objcull

Screen-size small-object culling (LEGION_SURVEY 2.5, Legion test 0x00C6EE2D). CPU only, every backend.

**Where.** `CWorldScene::AddDoodadDefModelToModelScene` (World.cpp), the one point every doodad path goes through
(exterior sort-table doodads, chunk doodads, interior/portal doodads, and the water reflection re-run). It runs after
the stock distance fade and multiplies into the same `m_fadeAlpha`, so objects cross-fade instead of popping.
`CWorldParam::s_lodObjectPixScale` = window height / tan(fovy / 2) is set once per frame in `CWorldScene::Render`.

**Formula.** d' = |object centre - camera| - lodObjectCullDist, r = bounding-sphere radius.
Projected height in pixels h = r * pixScale / d'. Threshold P = lodObjectCullSize / 5 pixels; fade band
B = P * lodObjectFadeScale / 100. alpha *= clamp((h - P) / B, 0, 1); below 0.01 the doodad is not drawn.
Legion's form `(3r)^2 < (d' * k)^2` is the same test at h = P, with k = 3 * P / pixScale = 3 * P * tan(fovy/2) / H.
Legion's `d - r > maxDist` half is left out: the stock doodad fade distance already does it.

**Defaults.** lodObjectCullSize 15 -> P = 3 px, fadeScale 100 -> fades between 6 px and 3 px tall. At 1080p and the
stock 90 degree fov (pixScale 1080), a 1 yd tall bush (r 0.5) starts fading at ~120 yd and is gone at ~210 yd; a
4 yd crate at ~390 / ~750 yd. Only objects a few pixels tall go, so the master `lodObjectCull` defaults to 1.

**Exemptions.** Within lodObjectCullDist (30 yd); radius >= lodObjectMinSize / 10 yd (100 -> 10 yd: big trees, rocks);
doodads flagged 0x800 (never distance-fade in stock either).

**WMOs: not culled.** MapObj groups have no fade alpha path, so culling them would pop, and the 10 yd radius exemption
would spare nearly all of them anyway.

**Shadows.** The sun shadow maps gather doodads themselves (ShadowQuery.cpp, by fade distance and band size), not from
the scene list, so a faded-out doodad keeps casting its shadow: no shadows vanish with the model.
**Reflections.** The reflection pass re-culls with the mirrored camera, whose distance to the object equals the
reflected path length, so the reflection fades the same doodads at the same apparent size.

| CVar | Range (clamped) | Default |
|---|---|---|
| lodObjectCull | 0/1 | 1 |
| lodObjectCullSize | 1-100 (5 = 1 px) | 15 |
| lodObjectCullDist | 1-200 yd | 30 |
| lodObjectMinSize | 0-1000 (radius in 1/10 yd) | 100 |
| lodObjectFadeScale | 50-300 % | 100 |

Options: Advanced page, "Fade tiny far objects" (lodObjectCull) and "Tiny object size" slider (lodObjectCullSize 5-50).
lodObjectMinSize is our reading of the Legion CVar (its semantics are not verified).

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
