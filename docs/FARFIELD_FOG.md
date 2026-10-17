# Far-field fog (WDL horizon and later far passes)

Not stock. CVars: `wdlTextured` (1), `horizonFog` (1), `horizonBatched` (1), `horizonFarclipScale`
(stock, 4.0); the far ground texture's `farTexQuality`, `farTexPixels`, `farTexMemory`, `farTexBand`,
`farTexBandWidth` (see "The ground texture" and "The blend band"). `horizonFog 0` or `wdlTextured 0` gives back 12340's fog everywhere. `horizonBatched 0`
draws the WDL one tile at a time again (same look, for comparing cost).
Line numbers are for the commit that adds the batched pass (2026.10.17-wdlperf).

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
| Stretch factor: `horizonFarclipScale` when `wdlTextured` and `horizonFog` are both on, else 1 | `WoW/Source/WorldClient/MapLowDetail.cpp:375` |
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
| c13 / c14 | m_texOffset[0] / [1] | layer / alpha uv offset (0 for the WDL) |
| c18 / c19 | m_texScale[1] / [2] | layer / alpha uv scale (uv = (pos.yx - c23.yx) * scale.yx + offset.yx); WDL: -1 / (64 x 533.33) |
| c23 | m_position | geometry origin for uv; WDL: the map corner (17066.67, 17066.67), so one value serves every tile |
| c24 / c25 / c26 | m_sunDir (view space) / m_sunAmbient / m_sunDiffuse | lighting |

Pixel constant c2 holds the fog colour. On caps with `unk_0134` the colour goes to `GxRs_FogColor`
instead (`FarField::BindFogColor`, `MapLowDetail.cpp:405`).

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
wins the depth test. WDL tiles: `FarField::RenderTerrain` (`World.cpp:4949`, `MapLowDetail.cpp:1379`),
or, when it returns false, 12340's per-tile `CMap::RenderLowDetail` (`MapLowDetail.cpp:1502`).
DepthTest 3, opaque blend, culling off (culling on for a tile's hole cells), GxRs_Fog 1. Then the
horizon WMO silhouettes (fixed function, fog 0..1, so they stay fog-coloured). The sky is drawn after
and fills only far-depth pixels.

## The WDL pass (FarField::RenderTerrain)

One `RsPush`, shaders, the whole vertex constant block, the fog colour and the atlas (stages 0 and 1)
set once. Then:

- **Geometry**: static vertex + index buffer per 8 x 8 tiles (`BuildBlock`, `MapLowDetail.cpp:1199`),
  built the first time a block is visible and refilled only when the device drops the buffer
  (`m_valid`). Vertices are PN in world space (`WriteGridNormals`, `:272`), so no per-tile constants.
  Indices per tile in block order: first every tile's culled-off cells, then every tile's hole cells,
  so adjacent visible tiles form one index run (`DrawRuns`). 12340's `s_vbbList` (24 blocks, rebuilt
  per frame through BufStream past the 24th tile) is only used by the per-tile path now.
- **Tiles**: from `m_horizonChunkList` (`CMapLowDetail::CullHorizonAreas`, frustum + occlusion), minus
  tiles wholly nearer than the near plane `farZ - 50` (every pixel of them is clipped). Blocks sorted
  front to back.
- **Two sets**: baked tiles textured; tiles not baked yet (or with no ADT) in a second pass with
  vertex c12 = (0, 0, 1, 0), so fog = 0 and the shader writes exactly the fog colour: 12340's silhouette.
- Cost per frame: about 2 draws per visible block (one per run) plus a few for broken runs, 1 texture,
  1 constant upload (+1 register), no buffer writes. The per-tile path did 1-3 draws, an RsPush/Pop,
  a full constant upload, two texture binds and (past 24 tiles) a vertex rebuild per tile.
- No coarser WDL LOD: the tile triangle count (1024) is not the cost, and a coarser index buffer past
  2x farclip would open T-junction cracks the sky shows through. The finer levels near the view distance
  are the next section's.

## Detailed levels (farTerrainLod)

CVars: `farTerrainLod` (1; 0 = WDL only, as before), `farTerrainLodPixels` (1.5, the largest height
error allowed on screen, px), `farTerrainLodMemory` (160 MB for heights and level buffers),
`farTerrainLodStats` (0; 1 prints tiles, levels, draws and MB to the console every 600 frames).
Code: `WoW/Source/WorldClient/FarTerrainLod.{h,cpp}`, hooked into `MapLowDetail.cpp` at five places
(grep `FarLod::`).

The WDL grid is 33 yd, so a hill between its samples came out as a cone. Port of wxl-farfield's
detailed level (`/root/wxl-farbush-wt/wxl-farfield/src/FarTerrain.cpp`, "detail: the baked level":
65 x 65 heights a tile from wxl-wraithhost's `<map>.farlod`, 25 yd skirts, ADT holes left out, those
tiles masked out of the WDL pass). wraithhost does not load in Wow32/Wow64, so the heights are read here
from the same ADTs the ground bake reads, and there are three grid levels instead of one:

| Level | Grid a tile | Spacing | Vertex buffer (with skirts) | Index buffer (shared unless holes) |
|---|---|---|---|---|
| 0 full | 129 x 129 (every ADT corner) | 4.17 yd | 412 KB | 203 KB |
| 1 half | 65 x 65 (farfield's) | 8.3 yd | 108 KB | 52 KB |
| 2 quarter | 33 x 33 | 16.7 yd | 29 KB | 14 KB |
| 3 WDL | 17 x 17 + 16 x 16 | 33.3 yd | the batched pass above | |

- **Heights** (`FarLod::AdtRead`, `FarTerrainLod.cpp:548`; `Extract` `:197`): MCVT outer vertices + MCNK
  posZ, MCNR normals as the real terrain's vertices take them (x 1/127), MCNK holes. About 117 KB a tile
  on the CPU, at most half of `farTerrainLodMemory`. `PumpBake` reads a tile's ADT once for the bake and
  the heights together; a tile whose heights were dropped (it left the horizon, or the cap) is read again
  for heights only (`FarLod::WantsAdt`, `:533`). Same queue, nearest first, one finished read a frame.
- **Error** (`MeasureErrors`, `:261`): per tile and level, the largest height difference between that
  level's surface and the real terrain at every MCVT vertex (outer and inner). Level 3 measures the WDL
  mesh itself (12340's four-triangle fan), so a tile whose WDL is already right stays on the cheap batched
  pass.
- **Pick** (`FarLod::BeginFrame`, for every tile with heights, in view or not; `FarLod::Claim` only reads
  the result for the visible ones): px per yard
  = 0.5 x viewport height x projection y-scale / distance to the tile's nearest point (at least the
  horizon near plane). The coarsest level with `err x px/yd <= farTerrainLodPixels` wins; level 3
  means the tile stays in the WDL runs. Tiles the real terrain reaches into (nearest point within
  farclip + 34 yd) always take level 0: those vertices and normals are the real terrain's own and its
  chunk edges lie on level 0's quad edges, so there is no step where it stops (`:640`).
- **Joins**: farfield's skirts. Every detailed tile's four edges hang down `max(25, 2 x WDL error)`
  yd (`:456`), over the cracks to a neighbour at another level or on the WDL; their normals and atlas
  colour are the edge's, so a crack shows ground, not sky. No stitched indices, so no shared-edge
  bookkeeping between tiles.
- **Holes**: levels 0 and 1 drop exactly the ADT's hole quads (a bit per 2 x 2 corners, as
  `CMapChunk::CreateIndices`); level 2 drops a quad only when all four bits under it are holes
  (`AllHoles`).
- **Buffers and cache** (`Build`, `:416`): static PN vertex buffer per tile and level, built once and
  refilled only when the device drops it; index buffers per level shared by every tile without holes.
  Missing levels queue nearest first, whether in view or not, and at most 2 build a frame
  (`FarLod::EndFrame`); until then the nearest built level draws, else the WDL. Over the cap, only levels
  their tile's distance no longer asks for go, farthest tile first: looking away never drops a level
  (2026.10.17-farlod rebuilt off-screen tiles on the LRU and drew them on the WDL meanwhile). Heights for tiles 2.5 tiles past the horizon's reach go every 60 frames.
- **Draw** (`FarLod::Draw`, `:674`): inside the WDL pass's `RsPush`, after each set's WDL runs, with
  the same shaders, constants, atlas and fog (set 1 fog-coloured for tiles not baked yet). One draw a
  detailed tile, culling off, front to back.
- **Cost**: the WDL pass did about 2 draws per visible 8 x 8 block. Each detailed tile adds one draw
  (and can split a WDL run, about +1). With farclip 1000 and horizonFarclipScale 4, expect roughly 10-20
  level-0 tiles round the view distance, a few dozen at levels 1-2 where hills need them, the rest on
  the WDL: 30-60 extra draws, 15-30 MB. `farTerrainLodStats 1` prints the real numbers.

## The ground texture (no minimap)

The WDL shows the ground that is really on each tile, baked in the client the way wxl-wraithhost bakes
wxl-farfield's `.faralb` (the map's layer textures composited by their alpha maps; farfield lays it on its
detailed tiles at 2.083 yd a texel, `/root/wxl-farbush-wt/wxl-farfield/src/FarTerrain.cpp` header and
`albedo`). Baked here because wraithhost does not load in Wow32/Wow64; it needs nothing but the ADTs.

CVars: `farTexQuality` (2; 0 coarse only, 1 + mid, 2 + near), `farTexPixels` (1.5: the largest far texel
allowed on screen, px), `farTexMemory` (72 MB: each atlas costs 21.3 MB, 10.7 on the GPU and its 10.7 MB
CPU copy for the texture callback; coarse always, then mid, then near while they fit).

| Atlas | A tile | A texel | Holds | Texture |
|---|---|---|---|---|
| near | 512 px | 1.04 yd | the 7 x 7 tiles round the camera's (8 x 8 slots) | 4096 px DXT1, 11 levels |
| mid | 256 px | 2.08 yd (farfield's) | 15 x 15 tiles (16 x 16 slots) | 4096 px DXT1 |
| coarse | 64 px | 8.3 yd | the whole 64 x 64 map | 4096 px DXT1 |

- **Window atlases** (farfield's `albedo` layout): a tile lives in the slot `(col mod n, row mod n)`, so the
  uv stays linear in the position: `uv = (pos.yx - c23.yx) * c18.yx` with c23 the map corner and
  `c18 = -1 / (n x 533.33)`, wrap addressing. One constant pair and one texture per atlas; a bilinear tap or
  a mip that crosses a slot edge reaches the tile that really is next door (except at the window's edge,
  which only the farthest window tiles touch). The coarse atlas is the same code with n = 64.
- **Which atlas a tile draws with** (`PickAtlas`): the coarsest one that holds it whose texel stays under
  `farTexPixels` at the tile's nearest point (px per yd = 0.5 x viewport height x projection y scale /
  distance, as farTerrainLod), else the finest that holds it, else fog-coloured. Distance only.
- **Bake** (`PumpBake`, `StepJob`): nearest tile first within the horizon's reach, two ADT reads in flight
  on the async file queue; a tile is read when its coarse bake is missing, it is inside a window atlas that
  does not hold it yet, or farTerrainLod wants its heights. A finished read is baked in slices, about 2 ms a
  frame (one slice always runs): heights (FarLod), 16 chunk rows composited at the finest resolution any
  wanted atlas needs, then each wanted atlas's levels box-filtered down and DXT1-encoded 16 block rows a
  slice; the tile's slot rectangle is uploaded once at the end (`GxTexUpdate`). Levels where a texel spans
  tiles come from the slots' tile averages. The ADT buffer is freed after the composite.
- **Composite** (`CompositeRow`): per atlas texel, each MCLY layer's texture at the texel's place in its
  repeat (8 repeats a chunk, u along the columns, v along the rows, as the Terrain vertex shader maps
  them; `LayerTexOf` reads the texture's smallest mip of 4 x 4 or more, reduced to 4 x 4, 2 x 2, 1 x 1:
  at 1.04 yd a texel a repeat is 4 texels, so this is the mip the real terrain samples there), blended by
  the alpha maps averaged over the texel (big alpha: weighted sum, perm 14; else layered lerp, perm 6),
  times MCCV (x 2 / 255, bilinear) and MCSH (the shader's 0.7 + 0.3 lit). MCAL is sized by the MCNK's
  `sizeAlpha`, never the MCAL header (Northrend's headers say 512). Liquid cells take their LiquidType's
  colour (wraithhost's). No saturation boost any more: the old 1.4 opened up 8.3 yd averages; these are
  the texels the real terrain itself shows at that density.
- **Cost**: `farTerrainLodStats 1` prints tiles held per atlas, bakes and the average bake time a tile
  (all slices summed). Expect a few ms of work a tile spread over several frames; each frame spends at most
  about 2 ms plus one slice. Memory: 64 MB at the defaults (three atlases), plus 7 MB static scratch.
- No eviction pass: a window atlas's slot is overwritten only when a tile nearer the camera claims it, so
  nothing is lost by looking away.

## The blend band (farTexBand)

CVars: `farTexBand` (1), `farTexBandWidth` (150 yd). Port of farfield's `blend` (kBlendPs): the last
stretch of real terrain fades into the far land's colour at the same ground point, so by the cut-off the
two are one picture and there is no line.

- `CWorldScene::RenderChunks` notes every real chunk it draws (`FarField::NoteChunk`, both chunk loops in
  `World.cpp`); after them `FarField::RenderBlend` redraws the chunks reaching into the band from their own
  buffers with the stock `Terrain` vertex shader (permutation 0: no vertex colour, the bake holds MCCV) and
  our pixel shader **FarBlend** (`Tools/CustomMpq/ShaderSrc/FarBlend.hlsl`). The far land's colour, lit and
  fogged as the far pass paints it (`atlas x light x 2`, `lerp(c2, colour, fog)` with the terrain's own fog
  factor), at alpha `smoothstep` over the band (pixel c3 = start, 1 / width, strength). c23 = the camera, so
  texcoord1 (c14 / c19 = 1) is the ground offset in yards; texcoord0 is the atlas uv (c13 / c18 per atlas).
- Each chunk paints from the finest atlas holding its tile (near, almost always). Alpha blend, no depth
  write; the projection's z is pulled 4e-7 toward the camera so the paint passes the depth test over its
  own triangles on every backend.
- Farfield works from the depth buffer as a full-screen post pass with a height test for objects; drawing
  the terrain's own triangles needs neither: trees, buildings and water are never painted.
- The band ends 4% short of the view distance and is measured in the ground plane (the shader only gets x/y):
  terrain far below the camera reaches the view-depth cut at a shorter ground distance.
- Not ported: farfield's live colour match (`match`, a per-channel gain measured on the GPU against the near
  ground). The bake now uses the real texels with the terrain's own blend, MCCV, shadow, light and fog, so
  the colours should already agree; the band hides what is left.
- Shader pipeline: `python3 -I Tools/CustomMpq/ShaderSrc/build_shaders.py` compiles the HLSL to ps_3_0
  (vkd3d-compiler) as `Tools/CustomMpq/Shaders/Pixel/ps_3_0/FarBlend.bls` (D3D9) and, through shader-xlate,
  `ps_4_0/FarBlend.bls` (D3D11); both are committed. `Tools/VulkanShaders/translate_shaders.py` reads
  `Tools/CustomMpq/Shaders` after the client's archives, so the Vulkan manifest gets it; build_custom_mpq.py
  packs all three. Needs ps_3_0 (`Caps().unk_0134 == 0`); otherwise the band is off.

wxl-light, vfog and the other wxl plugins do not load in Wow32/Wow64 (they hook stock 12340
addresses; see `/root/d3d11-port/notes/vulkan-plugin-parity.md`). When they come back as an
in-tree WorldPass, a fog/lighting composite must run after RenderHorizon and treat the
horizon depth band as far geometry.

## Adding a far pass (WMOs on the WDL; see Far foliage for one that is done)

1. Draw inside `CWorldScene::RenderHorizon`, between the WDL pass and the WMO silhouettes, so it
   uses the horizon projection and depth band. Use one `RsPush`/`RsPop` for the whole pass and static
   buffers, as `FarField::RenderTerrain` does; never state or uploads per object.
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

## Far foliage (farTrees, farGrass)

Not stock. CVars: `farTrees` (1), `farGrass` (1), `farGrassMinPixels` (4). `farTrees 0` draws nothing
and bakes no pictures (placements are still collected, memory only); `farGrass 0` gives back 12340's grass
exactly. The cards also need the far fog (`wdlTextured 1`, `horizonFog 1`), like every far pass.
Line numbers are for the commit that adds the foliage (2026.10.17-farfoliage).

A port of wxl-farfield's far foliage (`/root/wxl-farbush-wt/wxl-farfield/src/FarFoliage.cpp`) and of the
half wxl-wraithhost bakes for it (`wraithhost-lite/src/host/terrain/FarLod.cpp` placements,
`Impostor.cpp` pictures), all in `WoW/Source/WorldClient/FarFoliage.cpp`. wraithhost does not load in
Wow32/Wow64, so both bakes run in the client, on the far ground's ADT reads.

| Step | Where |
|---|---|
| The ground bake hands each ADT it has read to the foliage scan | `MapLowDetail.cpp:1210` (PumpBake) |
| Scan: MMDX/MMID/MDDF; a model is a tree or a bush by wraithhost's name rules (`IsBushName`, the TREE/PALM/KALIDARBUSH list less STREET/LEAVES/STUMP/LOG/ROOT), header box read once; a placement counts if >= 6 yd (tree) or >= 1.5 yd (bush) tall as placed; one card per uniqueId; its stock size class from the yaw-turned box's largest side (`MapDoodadDef.cpp:85`); filed into its 8 x 8-tile block | `FarFoliage.cpp:1175` (FoliageScanTile) |
| Pictures: the model's skin-00 mesh and textures rasterised on the CPU, orthographic, from 4 sides every 90 deg, bottom-aligned in a square of side max(height, 2 x reach), blend 0-2 batches at the game's own keys (224/255 blend 1, half blend 2), times the batch colour; 128 px down to 64, empty pixels filled from drawn neighbours, levels box-filtered with the coverage kept. A bush with no cut-out texture (coverage >= 0.95: a solid prop) is dropped | `FarFoliage.cpp:545` (RenderPicture) |
| Atlas: one 4096 x 2048 DXT5, 64 px a view, 4 views a model in a row (512 models; past that a model goes without, as wraithhost's kImpMax). Levels 0-4 per view; 5-9 box-filtered from a kept copy of level 4. Alpha: 0 cut out, else 136..255 per-texel noise (`NoiseAlpha`, `:453`), so the distance fade dissolves a card. One model baked a frame (main pass), its rectangle uploaded | `:984` (PumpPictures), `:952` (StoreSmallLevels) |
| Cards: per block, one quad per placement facing the camera, showing the view nearest the camera (view k = the model's +x turned k x 90 deg, yaw = MDDF rot.y + 180), side = frame x scale, bottom at z + picture bottom x scale; normal up leaning toward the camera; sorted by size class, chunks of 16384 cards (one shared 16-bit index buffer). Rebuilt when the camera has moved more than a quarter of the block's distance (min 150 yd), when a missing picture is baked, or when the device drops the buffer; at most 2 rebuilds a frame, nearest first | `:1080` (BuildBlock), `:1348` (RenderFoliage) |
| Draw: main pass before RenderHorizon (normal projection, blocks nearer than farZ) and again inside RenderHorizon after the WDL (horizon projection and depth band, blocks reaching past farZ - 50); blocks front to back, one draw per chunk and size class, 4 constants a block | `CWorldSceneRender.cpp:206`, `World.cpp:4960` |

**Shader and fog.** The cards use the stock **DetailDoodad** shaders (the ground-effect grass's,
`CDetailDoodad::s_vertexShader[0]` / `s_pixelShader[0]`): world-space PNCT, sun light per vertex, and the
same fog formula on the same `DNInfo::m_fog` as the terrain (vertex **c8** here, c12 for the terrain; fog
colour pixel c2 or `GxRs_FogColor`), so a card fades into fog exactly like the far ground behind it. The
constants are the grass's own for the frame (`CDetailDoodad::InitializeShaderConstants`,
`FarFoliage.cpp:1462`, the grass's copy restored after), with c0-c3 = translate(block centre - camera) x
view per block and c9 = the class fade. Nothing new in custom.MPQ: the shaders exist on D3D9, D3D11 (SM4
pack) and Vulkan (translated) already.

**Alpha test on every backend.** Blend `GxBlend_AlphaKey`, `GxRs_AlphaRef` 128, depth write on, culling
off (`:1449`). D3D9 tests natively; D3D11 (`CGxDeviceD3d11.cpp:2292`) and Vulkan (`misc.x`,
`CGxDeviceVulkanShader.cpp:14`) apply the same reference in the shader, as for the grass. Never blended:
a fading card must write depth (memory feedback_sky_before_blended_far_passes), and the sky after it only
fills far-depth pixels.

**Hand-over (no pop).** The shader multiplies the texture's alpha by `saturate(viewZ * c9.x + c9.y)`.
Per size class the card is whole at the place its model starts to fade, `CWorldScene::s_fadeDistMin[c]`
(scaled by environmentDetail), less 24 yd (the stock cull goes by chunk distance), or `farZ - 35` if
nearer (no doodad is drawn past farclip), times the cosine of the half horizontal FOV (the shader fades by
view depth, the game by distance, so the screen edge is covered too). It comes in over one band before
that (max(20 yd, 10%)) (`:1426`). The card is whole before the model starts to go, never a complement
cross-fade (the wxl lesson); the price is a stretch where both are drawn.

**Draws.** About one draw per visible block and class present (trees: classes 2-4, bushes 1-2) in each
pass: typically 10-30 a frame, plus one 4-register constant upload a block, no buffer writes except a
rebuild. No frustum culling (the GPU clips; a block is 4266 yd across).

### Grass (farGrass)

Stock ground effects (`CDetailDoodadInst::DrawBatches`, `DetailDoodad.cpp:821`): per batch (one texture of
a chunk's ground effect), the tallest tuft as placed (model box height x the add's scale) gives the
distance where it is `farGrassMinPixels` tall on screen: `height x proj.b1 x viewportHeightPx / 2 / minPx`
(`FarFoliage.cpp:1528`), so distance, FOV and resolution all count. If that is nearer than
`groundEffectDist`, the batch is skipped when its chunk lies wholly past it (chunk distance - 23.6 yd), else
drawn with the stock fade (alpha over the last 15%) moved in to it (vertex c9, restored after). Taller tufts
are drawn exactly as stock. Shader path only. No far grass cards are added (wxl-farfield has them; the
request was less grass where it cannot be seen, not more).

### Where future light and colour settings hook in (foliage)

- Card brightness / tint: the vertex colour in `BuildBlock` (white now; the shader multiplies it in), or
  the picture colours in `RenderPicture` (a far colour match like wxl's `gMatch` belongs there).
- Card light: the normal in `BuildBlock`, and the sun the grass gets (`CDetailDoodad::InitializeShaderConstants`,
  `DetailDoodad.cpp:224`), which follows `CM2Lighting` and the map light.
- Fog: nothing of its own; it follows `DayNight::UpdateFog` and the stretch like every far pass.
- Hand-over distances: `RenderFoliage` (`:1426`); the stock fade tables are `CWorldScene::ScaleFadeDist`.

### Known limits (foliage)

- No normal map: a card is lit per vertex (one normal), flatter than wxl's per-pixel light. No glow
  picture (additive layers) and no second texture of two-texture batches (Crystalsong).
- Views switch (nearest of 4) on a rebuild rather than turning smoothly; the pop is at far distance.
- Pictures are baked on the main thread, one model a frame (a few ms each) for the first seconds on a map.
- Tilted placements (MDDF rot.x/z) are drawn upright.
- Fixed-function fallback (no DetailDoodad shaders): no cards.

## Where future light settings hook in

- **Fog distance, colour or tint for everything (near and far):** `DayNight::UpdateFog`
  (`DayNight.cpp:2120`), after the stretch. Every consumer reads `DNInfo::m_fog`.
- **Sun, ambient or diffuse for terrain and every far pass:** `CMapRenderChunk::InitializeVertexShaderConstants`
  (`MapRenderChunk.cpp:1961`, sun at `:2003`). The far passes copy these values.
- **Detailed levels** use the same constants and atlas as the WDL pass, so every light or fog change above
  reaches them with no code of their own. Only their geometry (`FarTerrainLod.cpp`) is separate.
- **Far-only adjustments** (e.g. a horizon tint, or a fog bias for far geometry):
  `FarField::TerrainConstants` / `FarField::BindFogColor` (`MapLowDetail.cpp:383`, `:405`). This is
  the one place every far pass reads from. A far colour correction belongs in `CompositeRow` (baked) or
  in the far shader.
- **Per-zone light:** comes in through Light.dbc in `DayNight::CalcColors` (`DayNight.cpp:447`) and
  is already reflected in both of the above.

## Files

- `WoW/Source/WorldClient/MapLowDetail.cpp`: CVars `:357`; FarField `:375-430`; bake, atlas and the
  batched pass `:432-1500`; per-tile RenderLowDetail `:1502` (shader path, the FF textured path and
  the stock silhouette remain as fallbacks).
- `WoW/Source/WorldClient/MapLowDetail.h:47`: `namespace FarField`.
- `WoW/Source/WorldClient/FarTerrainLod.{h,cpp}`: the detailed levels (`namespace FarLod`).
- `Tools/CustomMpq/ShaderSrc/FarBlend.hlsl`, `build_shaders.py`; `Tools/CustomMpq/Shaders/`: the blend band shader.
- `WoW/Source/WorldClient/World.cpp:4949`: RenderHorizon calls the pass.
- `WoW/Source/WorldClient/DayNight.cpp:2123-2140`: the stretch.
- `WoW/Source/WorldClient/World.cpp:1561`: farFog clamp.
- `WoW/Source/WorldClient/FarFoliage.cpp`: far trees and bushes, the grass size rule (farTrees, farGrass).

## Known limits

- Texel detail: 1.04 yd near the view distance, 2.08 yd to about 4000 yd, 8.3 yd past that. It is redone
  every session (no disk cache); a continent's horizon fills in over some seconds after login or a
  teleport, nearest first.
- The blend band paints lake beds under water in the liquid's colour, then the water draws over them.
- Water is a flat colour on the WDL's (bed) heights; nothing lifts lakes to their surface or draws a
  far sea.
- WDL heights are 33 yd apart; `farTerrainLod` replaces them wherever the error would show. Past
  that, and for tiles not read yet, normals at a WDL tile's edge are still one-sided.
- Detailed levels: a WDL tile next to a detailed one has no skirt, so where the WDL edge stands
  higher and farther, the crack shows the WDL's underside (culling is off, so it is ground-coloured).
  Level 2 keeps holes smaller than 2 x 2 bits closed. No geomorphing: a tile changing level pops by
  under `farTerrainLodPixels`.
- The horizon WMO silhouettes are still fog-coloured (stock). With the stretched fog they read as
  flat shapes until "WMOs on the WDL" replaces them.
- Without terrain vertex shaders, the WDL falls back to the fixed-function textured path: linear fog
  with no rate, using the same stretched start/end.
