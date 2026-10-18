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
