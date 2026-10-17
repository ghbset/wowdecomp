# Far-field fog (WDL horizon and later far passes)

Not stock. CVars: `wdlTextured` (1), `horizonFog` (1), `horizonFarclipScale` (stock, 4.0).
`horizonFog 0` or `wdlTextured 0` gives back 12340's fog everywhere.
Line numbers are for the commit that adds this file.

## The idea

Stock 3.3.5a fog ends at or before farclip (`DayNight::UpdateFog` caps the end at
`DNInfo::m_farClip`). The WDL horizon is only drawn past `farZ - 50`
(`CWorldScene::BuildHorizonProjection`), so stock draws it completely fogged: a fog-coloured
silhouette. A textured horizon either looks the same as stock or needs a fog of its own, and a fog of
its own cannot meet the terrain's, because the terrain is already fully fogged at its edge. Before
this change the textured WDL used a linear fog stretched only for itself (`413f7c4`), and it came out
clear right behind terrain that had faded completely into fog.

The fix gives everything one fog. The world's fog end is stretched by `horizonFarclipScale`, so the
fog reaches full density at the horizon's far plane. The curve keeps its zone shape: the start
fraction and the WotLK fog rate are unchanged. The terrain is then no longer fully fogged at
farclip. Every far pass is drawn with the terrain's own fog constants, so it continues the same
curve. This is what wxl-farfield's `game_fog = 1` did for the stock exe
(`/root/wxl-farbush-wt/wxl-farfield/src/FarField.cpp` `GameFog::Apply`).

## Where the parameters come from (per frame)

| Step | Where |
|---|---|
| Light.dbc bands -> fog end, start fraction, rate. On the WotLK model (map >= 530) the end is pinned to farclip and the rate comes from `CalcFogRate` | `WoW/Source/WorldClient/DayNight.cpp:426` (CalcFogRate), `:447` (CalcColors) |
| Final fog: `end = min(bandEnd, farClip) * FarField::FogStretch()`, `start = end * startMul`, colour = sky colour 5, density = rate | `DayNight.cpp:2120` (UpdateFog), stretch at `:2123`, `:2130`, `:2140` |
| Stretch factor: `horizonFarclipScale` when `wdlTextured` and `horizonFog` are both on, else 1 | `WoW/Source/WorldClient/MapLowDetail.cpp:362` |
| `CWorld::farFog` (object fog cull) clamped to farclip, so culling keeps stock's bound | `WoW/Source/WorldClient/World.cpp:1561` |
| Terrain vertex constants built once a frame from `DNInfo::m_fog` and the map light (sun) | `WoW/Source/WorldClient/MapRenderChunk.cpp:1961` (InitializeVertexShaderConstants), fog `:2032`, sun `:2003`; called from `World.cpp:3730` (CWorldScene::RenderChunks) |

All consumers of `DNInfo::m_fog` (terrain, WMOs, M2 `CM2Lighting::SetFog`, liquids, detail
doodads, the FF path) see the same stretched values, so the near scene and the far passes agree.

## How it reaches the GPU

The far pass uses the **stock terrain shaders** (`Terrain` vs permutation 0, `Terrain1` ps
permutation 0). That means no new shader, nothing to change in custom.MPQ, and the same registers on
every backend.

Vertex constants (`CMapRenderChunk::SVertexShaderConstants`, `MapRenderChunk.h:178`):

| Register | Field | Use |
|---|---|---|
| c0-c3 | m_worldView | world = translate(-camera) |
| c4-c7 | m_projection | the horizon projection (bound by RenderHorizon) |
| c12 | m_fog | x = -fogMul/(end-start), y = end/(end-start), z = rate, w = 0 |
| c13 / c14 | m_texOffset[0] / [1] | layer / alpha uv offset |
| c18 / c19 | m_texScale[1] / [2] | layer / alpha uv scale (uv = (pos.yx - c23.yx) * scale.yx + offset.yx) |
| c23 | m_position | geometry origin for uv |
| c24 / c25 / c26 | m_sunDir (view space) / m_sunAmbient / m_sunDiffuse | lighting |

Pixel constant c2 holds the fog colour. On caps with `unk_0134` the colour goes to `GxRs_FogColor`
instead (`FarField::BindFogColor`, `MapLowDetail.cpp:392`).

- **Vulkan:** `ShaderConstantsSet` -> `IShaderConstantsFlush` (`CGxDeviceVulkan/CGxDeviceVulkanShader.cpp:230`,
  `:263`) -> one uniform-ring slice, set 1 binding 0: `p[16]` at byte 0, `misc` at 256, `v[i]` at
  272 + 16 i (`Tools/VulkanShaders/bls/emitter.py:619`). The translated Terrain shaders read
  `regs.v[12]` and `regs.p[2]`. Only the fixed-function program uses pixel rows 14/15, for its
  linear fog (`CGxDeviceVulkanDraw.cpp:266-281`, `Shaders/Ff.frag`).
- **D3D11:** the SM4 builds of the same stock shaders take the register file as constant buffer b0
  (`CGxDeviceD3d11/CGxDeviceD3d11.cpp:2583-2592`): the same c12 / c2.
- **D3D9:** the stock shaders run natively, with the same registers.

## Shader formula (identical to the ADT terrain)

From the `Terrain` vs_3_0 bytecode:

```
fog   = min(pow(max(viewZ * c12.x + c12.y, 0), c12.z), 1)      // 1 = clear, 0 = fog colour
light = min(c25 + c26 * saturate(dot(c24, N_view)), 1)          // ps doubles the 0.5-scaled vertex colour
out   = lerp(fogColour(c2), tex0 * (0.7 + 0.3 * tex1.a) * light, fog)
```

So the fog is linear between start and end, raised to the zone's rate (1 on old-continent maps,
1.5-7 on WotLK maps). Because the WDL uses the terrain's own c12/c2 for the same frame, it matches
the ADT terrain exactly at every distance. The only difference is the stretch, and that applies to
both.

## Draw order and state

`CWorldScene::Render` (`CWorldSceneRender.cpp`): ApplyFogState -> RenderChunks (ADT terrain;
constants built here) -> RenderMapObjDefGroups -> occlusion queries -> **RenderHorizon**
(`CWorldSceneRender.cpp:204`, `World.cpp:4929`) -> portals -> **sky** (`CWorldSceneRender.cpp:222`)
-> entities -> liquids.

RenderHorizon: horizon projection (near `farZ - 50`, far `farclip * horizonFarclipScale`),
viewport depth band `0.998046875..0.9990234375` (`World.cpp:478`, `:481`, `:4941`), so anything near
wins the depth test. WDL tiles (`CMap::RenderLowDetail`, `MapLowDetail.cpp:406`, `World.cpp:4951`):
DepthTest 3, opaque blend, culling off/on for holes, GxRs_Fog 1. Then the horizon WMO silhouettes
(`World.cpp:4989`): fixed function, fog 0..1, so they stay fog-coloured. The sky is drawn after
and fills only far-depth pixels.

wxl-light, vfog and the other wxl plugins do not load in Wow32/Wow64 (they hook stock 12340
addresses; see `/root/d3d11-port/notes/vulkan-plugin-parity.md`). When they come back as an
in-tree WorldPass, a fog/lighting composite must run after RenderHorizon and treat the
horizon depth band as far geometry.

## Adding a far pass (tree/grass billboards, WMOs on the WDL)

1. Draw inside `CWorldScene::RenderHorizon`, between the WDL loop and the WMO silhouettes, so it
   uses the horizon projection and depth band. Use `RsPush`/`RsPop`.
2. Call `FarField::TerrainConstants(consts)` (`MapLowDetail.h:47`). It returns this frame's c0-c7,
   c12, c24-c26 with the bound projection and no point lights. Then call `FarField::BindFogColor()`.
3. If the pass can use the stock terrain shaders, set its own `m_position` / `m_texOffset` /
   `m_texScale` and upload `consts` to vertex c0. Otherwise (billboards and WMOs need their own
   shaders), give the new shader **the same registers**: fog in vertex c12 with the formula above,
   fog colour in pixel c2, sun in c24-c26. Then the one upload above serves every far pass. Add the
   shader to custom.MPQ for Vulkan (SPIR-V) and D3D11 (SM4) (`Tools/CustomMpq/build_custom_mpq.py`).
4. Gate the pass on `FarField::FogStretch() != 1.0f`. With the stock fog, everything past farclip is
   fully fogged and the pass would be invisible.
5. Do not compute fog from `GxRs_FogStart/End`. That is the fixed-function fallback only (linear, no
   rate).

## Where future light settings hook in

- **Fog distance, colour or tint for everything (near and far):** `DayNight::UpdateFog`
  (`DayNight.cpp:2120`), after the stretch. Every consumer reads `DNInfo::m_fog`.
- **Sun, ambient or diffuse for terrain and every far pass:** `CMapRenderChunk::InitializeVertexShaderConstants`
  (`MapRenderChunk.cpp:1961`, sun at `:2003`). The far passes copy these values.
- **Far-only adjustments** (e.g. a horizon tint, or a fog bias for far geometry):
  `FarField::TerrainConstants` / `FarField::BindFogColor` (`MapLowDetail.cpp:370`, `:392`). This is
  the one place every far pass reads from.
- **Per-zone light:** comes in through Light.dbc in `DayNight::CalcColors` (`DayNight.cpp:447`) and
  is already reflected in both of the above.

## Files

- `WoW/Source/WorldClient/MapLowDetail.cpp`: CVars `:344`, `:350`; FarField `:362-404`; PN grid
  with normals `:295`; RenderLowDetail `:406` (shader path from `:425`, the old FF textured path and
  the stock silhouette remain as fallbacks).
- `WoW/Source/WorldClient/MapLowDetail.h:47`: `namespace FarField`.
- `WoW/Source/WorldClient/DayNight.cpp:2123-2140`: the stretch.
- `WoW/Source/WorldClient/World.cpp:1561`: farFog clamp.

## Known limits

- The minimap tiles (256 px per ADT, about 2 yd/px, highest source available) have daylight shading
  baked in. Real sun lighting on top darkens slopes facing away from the sun a little.
- WDL heights are 33 yd apart, so the shape is coarse compared with the ADT terrain in the 50 yd
  overlap. Normals at a tile's edge are one-sided.
- The horizon WMO silhouettes are still fog-coloured (stock). With the stretched fog they read as
  flat shapes until "WMOs on the WDL" replaces them.
- Without terrain vertex shaders, the WDL falls back to the fixed-function textured path: linear fog
  with no rate, using the same stretched start/end.
