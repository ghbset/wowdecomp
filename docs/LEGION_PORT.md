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

## 2026-10-18: Legion post (survey items 10, 5, 9: depth users, FXAA, colour blindness)

### Depth-consumer registry (item 10, Legion `SetDepthUser` 0x006C4343, count at 0x013CCDDC)

`CGxDevice::s_depthUsers` is a bitmask of the scene-depth consumers this frame (CGxDevice.h). While it is 0, every
backend's `DeviceCopySceneDepth` (Vulkan, D3D11, both D3D9 through `d3d9scenedepth::Copy`) returns nullptr with
`*transient = true`, so no copy, blit or RESZ resolve runs and callers retry next frame. A bit instead of Legion's
counter: setting it twice is harmless, so a consumer can set it from its state every frame.

Registered now:

| bit | set where | on while |
|---|---|---|
| `DepthUser_Fog` | `VolumetricFog::SetDepthUsers` (DecideFrame, OnViewerLiquid) | vfog draws this frame (`s_active`) |
| `DepthUser_FadeStencil` | same | vfog draws and `vfogFadeEarly` and `vfogFadeStencil` |
| `DepthUser_Water` | `WaterArm` (cleared at its top, set once armed) | modern water draws in this liquid pass |
| `DepthUser_ShadowMask` | `VolumetricFog::OnOpaqueDone` | `shadowMask` wanted (`TerrainSunShadow::MaskWanted`) and the renderer ready |

The fog, fade and water users are inside vfog, and the copy was already demand-driven (only callers pay, and the Vulkan/D3D11/RESZ
"fresh" flags share one snapshot per frame), so this saves nothing measurable yet. What it adds is the contract: a
consumer that does not register gets no depth.

**Registering a new consumer** (screen-space shadow mask, SSAO, refraction, light buffer, soft particles):
1. add a bit to the enum in `Engine/Source/Gx/CGxDevice/CGxDevice.h` (`DepthUser_ShadowMask = 1u << 3`, ...);
2. once per frame, before its `DeviceCopySceneDepth` call, `CGxDevice::SetDepthUser(CGxDevice::DepthUser_X, on)`
   with `on` from its CVar state (and whatever else gates its pass). Nothing else.
The shadow mask is the first consumer outside vfog's `s_active` (it runs with vfog off), registered this way.

### FXAA (item 5) and colour blindness (item 9): one post pass

`VolumetricFog::PreparePost` (in `OnWorldDone`, which sees the world viewport) reads the CVars;
`RenderPost` (VolumetricFogRenderer.cpp, beside `RenderGrade`) draws in `OnWorldRendered`, after the colour grade and
the glow, before names (`WorldTextRenderBatch`) and the UI, so neither is anti-aliased or colour-shifted. It copies
the world viewport (the grade's bilinear `CopySceneBeforeFog`) and draws one quad, opaque, RGB, scissored to the world.
Runs whether or not vfog is on; skipped entirely when both features are off.

Shaders (ours, `Tools/CustomMpq/ShaderSrc/LegionPost.hlsl`, FXAA 3.11 quality after Lottes' public algorithm, flat
code with no dynamic branches): `LegionPost0` colour shift only, `LegionPost1` FXAA low (preset 12: 5 search steps,
edge threshold 0.166/0.0833) + shift, `LegionPost2` FXAA high (preset 39: 12 steps, 0.125/0.0625) + shift; subpix
0.75. Constants: c0 world viewport px, c10 FXAA (subpix, threshold, threshold min), c11-c13 the colour matrix rows.
The matrix is applied in linear light (gamma 2.2 decode/encode) and is the identity when colour blindness is off.

| CVar | range | default | |
|---|---|---|---|
| `ffxAntiAliasingMode` | 0-2 | 0 | 0 off, 1 FXAA low, 2 FXAA high (Legion's 3 = CMAA not ported) |
| `colorblindSimulator` | 0-3 | 0 | 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia |
| `daltonize` | 0/1 | 0 | 0 simulate the type, 1 correct for it |
| `colorblindWeaknessFactor` | 0-1 | 0.5 | severity: sim = I + f (M - I) |

Matrices: Machado, Oliveira and Fernandes 2009 at severity 1.0 (linear RGB); daltonize is Fidaner et al. 2005's error
shift, `I + E (I - sim)` with E = [[0,0,0],[0.7,1,0],[0.7,0,1]], folded into one 3x3 on the CPU
(`ColourShiftMatrix`). Legion's 11 types are not mirrored (its table is not used); `colorblindMode` (the stock UI text
labels CVar) is untouched and independent.

Options: Video Options > Advanced > Post Processing ("Anti-Aliasing & Colour").

## 2026-10-18: Legion lights (survey item 3, 2.8; FEATURE_BACKLOG #7 lamp lighting)

Legion 7.3.5 draws light volumes into a light buffer that every material reads (light pre-pass). v1 here is the
screen-space half of that: the frame's lamps resolved once from the scene depth and added onto the frame.

**Pass** (`LegionLights.cpp`, shader `LegionLights.hlsl`). The lights are the CM2Scene light grid's visible point
lights, the list vfog's fog scattering walks (`CaptureLocalLights`), without its 8-light cap: every M2 lamp, torch,
brazier and candle. CPU: camera distance cull (`maxLightDist` + radius), behind-camera cull, nearest `maxLightCount`
kept (`nth_element`), positions into view space. Radius = where the stock M2 attenuation (CM2Light: 0 / 0.7 / 0.03,
the same for every M2 light) leaves 1/32 of the light's peak colour, capped at 40 yd (about 23 yd for a colour-1
lamp). GPU: one full-screen quad per 16 lights (constants c10 control, c11-c58 three registers a light; literal
indices, no relative addressing for the translators), position from the depth, normal rebuilt from the depth
(VFogShadowMask's nearer-neighbour cross), per light `colour * min(stock attenuation, 1.5) * window^2 * N.L` with
`window = saturate(1 - d^2 / r^2)` (northlight local_light_effects.hlsl's smooth window, MIT; modern-wow-renderer
read for ideas only). Sky and the WDL horizon (depth past the world viewport's maxZ) are skipped; the light fades to
0 over the last quarter of `maxLightDist`; pixels with less than 1/512 of light are discarded.
Blend `GxBlend_ModAdd` (dst * src + dst): `colour += colour * light`, so the frame's own colour is the albedo proxy,
black stays black, and a surface at most doubles. The light is divided by the stock light level (luma of DNInfo
ambient + half the diffuse, at least 0.2), so a lamp adds about albedo x lamp as Legion's buffer does: strong at
night, faint at noon. Depth through `DepthUser_Lights` (bit 4), set only while the pass draws.

**Where, and the double-lighting question.** Stock 3.3.5a lights M2s (models, characters) and grass with up to 4
of these point lights each, but never terrain or WMO (WMO interiors have baked vertex colours, MOCV).
- `lightMode 1` runs right after `CWorld::Render` (terrain, WMO groups, horizon, sky) and before
  `scene->Draw(M2PASS_0)`: the depth then holds only terrain and WMO, and every model drawn after covers its pixels
  with its own stock-lit colour. So models are never double lit, and the lamps light exactly what stock leaves dark.
- `lightMode 2` runs after the solid world (after `VolumetricFog::OnSolidDone`, so after the shadow mask): models
  and grass take the lamps too, on top of their stock point lights (brighter near lamps). For a look closer to
  Legion's "everything lit"; not the default choice.
Both run before liquids, transparents and the fog composite, so the fog fogs the lit surfaces.

**CVars** (LegionLights.cpp, Video Options > Advanced > Lights, "Lamps & Torches"):
| CVar | Range | Default | |
|---|---|---|---|
| `lightMode` | 0/1/2 | 0 | 0 off, 1 terrain + WMO before the models, 2 the whole solid world (Legion "Quality of lighting" 0-2) |
| `maxLightCount` | 1..128 | 32 | nearest lights kept (Legion 128); 16 a pass, so 32 = 2 passes |
| `maxLightDist` | 20..2048 | 200 | yd; lights farther than this plus their radius are dropped, the light fades over the last quarter (Legion 2048) |
| `lightIntensity` | 0..4 | 1 | scale on the added light |

Default off: cost not measured (no rig by the rules), and it changes the look of every lit town at night. Estimate:
per pass 5 depth taps and ~16 x 14 ALU a pixel over the world viewport, about 0.2-0.4 ms a pass at 1440p on a mid GPU
(discarded pixels still pay the loop). With the shader cost confirmed, `lightMode 1` is the candidate default.

**Backends.** SPIR-V through Tools/VulkanShaders (Vulkan), ps_4_0 through build_shaders.py (D3D11), ps_3_0 (D3D9)
compiled by fxc.

**Known limits (v1).**
- Lights that are not M2 lights (WMO MOLT lights, spell lights) are not in the grid: not lit. Map lights are area
  lighting (DNInfo), not points.
- A lamp lights through walls (no shadowing); the N.L from rebuilt normals is per pixel and noisy on bumpy ground.
- WMO interiors already carry baked lamp light in MOCV: the lamp adds on top (lower `lightIntensity` indoors if it
  is too bright; no interior weight yet).
- The stock fog is in the colour when vfog is off (`vfogStockFog 0` or vfog off): a fogged pixel within reach is
  brightened with its fog. With vfog on the stock fog is pushed out, so the lit colour is unfogged here.
- Every pass covers the whole world viewport. ponytail in LegionLights.cpp: scissor each pass to its lights' screen
  rect if the cost shows.

**Next phase: a light pre-pass the receivers read (Legion's shape).** Draw each light as a volume (sphere or screen
rect) into an RGBA16F light buffer before the opaque world (needs a depth + normal pre-pass, or the normals rebuilt
from the depth as here) and have the terrain, WMO and M2 pixel shaders add `albedo * lightBuffer` in their own
lighting. That adds: true albedo (no division by the stock light level, so dark-textured and fogged surfaces are
right), lamps lighting under the stock fog, specular, per-material response, no 2x ceiling, models lit by more than 4
lights (their 4 stock point lights could be dropped), and spot lights (`lightbuffer_spotlight`). The cost is every
receiver shader (terrain 4-layer, WMO, M2 families) getting one more tap: 3-4 pieces, the survey estimate.
