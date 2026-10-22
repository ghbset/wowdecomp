# Sky + clouds under the sun arc: plan (2026-10-22, read-only survey, nothing built)

User idea: "if we use sun and moon across the sky i think we should switch to default day and night skys and use
our volumetric clouds from wxl-pack". Clarified: the user's HD pack ships LightSkybox M2 skyboxes, some of which
draw their own sun/moon that disagree with gfxSunArc's light. Wanted: with the arc on, no skybox M2 (or at least
none of its sun/moon), the stock dome + stars + our arc sun/moon, wxl clouds on top.

## 1. What the decomp draws today

`DayNight::RenderSky` (DayNight.cpp ~1843, stock 0x007F09B0), called from `CWorldScene::Render`
(CWorldSceneRender.cpp ~229) after terrain, WMO, horizon and portals, before entities/liquids; skipped under water.
Also called by `CataWater::RenderReflection` (CataWater.cpp:641) with the mirrored camera.

- Depth range forced to [s_skyMinZ 0.99902, 1], scissored to the portal rect.
- `drawObjects` path (procedural sky): `g_stars` (stars M2) -> `g_planets.m_sun` -> `m_moon` (moon2 only when
  arc is off) -> `g_sky` (vertex-coloured dome, LightIntBand sky colours) -> `g_clouds` (DNClouds sheet,
  `DNClouds::Render`, DayNightRender.cpp:195).
- `drawObjects` goes false when ANY skybox at weight > 0.99 (blend flag 2 clear) is drawable: then stars, sun,
  moon, dome and clouds are all skipped.
- Then LightSkybox M2s: `DayNight::DrawSky(sky, weight)` (DayNight.cpp:1797) via the private `s_skyScene`.
- Where skyboxes come from: `DayNight::SetColors` (2669) turns `info.m_bands.m_skyboxes[3]` (Light.dbc blend,
  `BlendOutputs`) into `info.m_blendSkybox[]/Weight[]/Flags[]` (loop at ~2830, `g_lightSkyboxDB.GetRecord` ->
  `GetOverrideSky` 2643 loads the M2 once into s_skyScene). The override-params skybox becomes `info.m_skybox` at
  weight 1 (~2731). Interior (WMO MOSB) skyboxes: `CWorldScene::s_interiorSkybox` ->
  `DayNight::SetBlendSky(0, …)` right before RenderSky (CWorldSceneRender.cpp, after SetColors).
- Other skybox readers: `DNGlare` fade (`(1 - skyboxWeight)`, ~3110) and `DNClouds::UpdateTextureAnimation`
  (~1472, cloud texture not animated under an opaque skybox).

**Nothing hides the skyboxes when gfxSunArc is on.** In a skybox zone the arc's sun/moon disc and the dome are not
drawn at all (drawObjects false) while the light, shadows (CShadowQuery) and vfog rays follow the arc: that is the
mismatch the user sees. Stock 3.3.5 LightSkybox is mostly Outland/Northrend/instances (Nagrand, Storm Peaks,
Zul'Drak, Icecrown, Dalaran, Wintergrasp, auroras with flag 2 = drawn over the procedural sky); the HD pack
presumably adds more via its own Light/LightParams/LightSkybox DBCs. The HD pack is not on this server (no client
Data here), so which of its M2s carry suns is unknown.

"Default day and night skies" = the stock procedural path: the LightIntBand gradient dome (per zone, per hour),
stars at night, our arc sun and moon, and (replacing the DNClouds sheet) the wxl clouds.

## 2. Simplest skybox gate (do first, ~1-2 h + a look in game)

CVar `gfxSunArcSkybox` (default 0 = skyboxes off while `SunArc()`; 1 = stock; 2 = only flag-2 overlays kept,
i.e. auroras/Battlefield dirt that draw OVER the procedural sky).

Gate at the source so every reader agrees: at the end of `DayNight::SetColors`, when `SunArc()` and the CVar
says so, zero `info.m_skyboxWeight` / null `info.m_skybox`, and drop `m_blendSkybox[i]` entries (all, or those
without flag 2 for mode 2). Then `RenderSky` takes the drawObjects path, `DNGlare` fades normally, DNClouds
animates. Interior skyboxes are untouched (SetBlendSky runs after SetColors): caves/WMO interiors keep theirs.
One edit in DayNight.cpp plus the CVar beside `s_cvSunArc`.

Risk: zones built for a skybox can have poor LightIntBand sky colours (black/odd tops, e.g. Icecrown, Outland,
some instances), since the M2 always covered the dome. Mitigation if seen: per-map exclusion list, or mode 2.

### Strip only the sun/moon from a skybox M2?

Practical but only per model, not generically:
- `CM2Model::SetGeometryVisible(first, last, visible)` (Engine/Source/Model2/M2Model.h:318) hides geosets by id.
  Works only if the pack's sun/moon sit in their own geoset ids; many skybox M2s keep everything in geoset 0.
- Otherwise a per-batch skip: after `GetOverrideSky` loads the model, mark skin batches whose texture path matches
  sun/moon/planet names and skip them in the M2 batch loop (a model flag + check in M2 render). New code in the M2
  renderer, ~0.5-1 day, plus a dump command listing each loaded skybox's geosets/batches/textures so names can be
  picked. Fails where the sun is painted into the sky texture itself (Nagrand-style) — nothing to strip there.
- Even stripped, the skybox's own sky colours/clouds stay, and its clouds would sit under ours. So stripping is
  worth it only for skyboxes the user specifically wants to keep. Recommend: gate whole skyboxes first, add the
  per-batch strip only if the user names skyboxes they miss (needs the HD pack's files here to inspect).

## 3. The wxl-pack volumetric clouds

Repo `/root/wxl-light` (shipped as wxl-pack `Extensions/wxl-light/wxl-light.dll`, settings in light.ini
`clouds = 1`, `cloud_cover = 0.35`). Not in wxl-vfog. Files:
- `src/Clouds.cpp` (723 lines): hooks, constants, draw.
- `src/CloudShaders.hpp` (ps_3_0 HLSL, baked by fxc into `CloudBaked.inc`): one-shell ray march, 20 steps,
  Perlin-Worley shape eroded by Worley detail, height profile, a few light steps toward the sun, HG phase.
  Out: rgb premultiplied, a = sky transmittance; blend ONE, SRCALPHA (rgb only).
- `src/CloudField.hpp` (WXL_CLOUD_HLSL): Weather/Shape/Height; small planet R = 60, base 1, thick 0.7 (units of
  1500 yd); `StockUv` = DNClouds' dome mapping (DNClouds::WorldToTexture 0x007EF920).
- `src/Atmosphere.hpp/.cpp`: SkyAt() sky colour (c40..c44) used for ambient/shade of the clouds and by the haze.
- `src/CloudNoise.bin` 64^3 BGRA (r = Perlin-Worley, gba = Worley octaves) from `tools/clouds/cloudnoise.py`;
  `tools/clouds/preview.py` = the same maths in numpy (look-dev without a client).

How it renders (D3D9 plugin): hooks `Sky__DrawCloudLayer` 0x009ACD40 (skips the stock sheet, records it was
asked = the engine wanted procedural clouds this frame), `DrawSkyBody` 0x009AC660 (hides stock disc when its own
disc is up), `RenderGlares` 0x007F0870, `Weather__Render` 0x0078CA50, and `beforewater` 0x00793D20 (the "early
sky" draw at the end of wxl-farfield's far-pass chain). Draws a full-screen quad at the far plane (hardware depth
test = sky pixels only). Inputs: view/proj from 0x00ADF5E8/0x00ADF628, DNClouds density array copied each frame
into a texture (the game's own cloud pattern says where, the shader says shape), DNClouds threshold, zone colours
(sky horizon 0x00D38BF4, direct light 0x00D38BD8), storm weight 0x00D38B88, sun/moon direction from its own
Sun.cpp arc, wind = a drift accumulator (`speed * 0.004` per second), moon texture (its own disc), linear depth +
wxl-water surface for under-horizon sea pixels.

Dependencies: wxl-depth (DepthApi/PostPass/Host, linear depth, SavedState), wxl-renderdistance header,
wxl-water (sea surface texture), wxl-farfield (far-pass chain order, terrain height), wxl-light's own Sun.cpp,
Atmosphere, and Haze/Bloom (`CloudVis` blocks the haze glow -> shafts; `CloudShadow` darkens ground). In the
decomp most of that collapses: g_dnInfo/g_clouds/g_planets are native, the sun arc is already ported
(DayNight::SunArc, g_sunToward/g_moonToward, g_lightBody), vfog owns fog and god rays.

## 4. Port plan (native, vfog-style)

Files (new, none of them VolumetricFog* / fog shaders / FFX.cpp, so no clash with the agent building now):
- `WoW/Source/WorldClient/SkyClouds.cpp/.h`: CVars, noise atlas, constants, draw. Reuse (read-only)
  `VolumetricFog::DrawQuad` and the vfog noise-atlas pattern (VolumetricFogNoise.cpp: 2D slice atlas with wrap
  borders — Gx has no volume textures).
- `Tools/CustomMpq/ShaderSrc/SkyClouds.hlsl` (from CloudShaders.hpp + CloudField.hpp + the SkyAt part of
  Atmosphere.hpp), one `SHADERS` entry in `build_shaders.py` -> `Shaders/Pixel/ps_3_0/SkyClouds.bls` (D3D9 +
  Vulkan source via Tools/VulkanShaders) and `ps_4_0` via shader-xlate. Already ps_3_0 under fxc in wxl-light,
  so the temp budget is known to fit.
- `tools`: port `cloudnoise.py` output into the atlas (bake 64 slices as 8x8 tiles of 66^2 at load, or ship a
  BLP in custom.MPQ). Keep `preview.py` as the look-dev check.
- DayNight.cpp: in `RenderSky`, `if (SkyClouds::Active()) SkyClouds::Draw(); else DayNight::g_clouds.Render();`
  plus the skybox gate (section 2). `CVar` registration next to the vfog ones.

CVars: `skyClouds` 0/1 (default 0 until seen), `skyCloudCover` (0.35), `skyCloudSpeed`, `skyCloudSteps` (12/20),
`skyCloudStockPattern` 0/1 (use DNClouds density as the weather map, as wxl-light a18).

### Where it draws
Inside RenderSky, in the stock sheet's slot: stars -> arc sun -> moon -> dome -> **our clouds** -> (skyboxes, if
any kept). So clouds cover stars and the sun/moon discs and are under everything opaque.
- Depth: quad at z just inside the sky slice (> 0.99902, i.e. behind horizon/terrain, in front of the dome),
  depth test LEQUAL, no depth write — passes only where no terrain/WMO/horizon was drawn. The dome may write
  depth: check DNSky::Render's depth state; if it writes, the quad must sit at <= its depth.
- After: entities, liquids, then vfog `OnLiquidDone` composite and god rays. VFogRayMask takes "bright sky near
  the sun" from the pre-fog scene copy at sky-depth pixels, so clouds drawn here darken it -> rays through cloud
  gaps for free. Whether the vfog composite fogs sky pixels (IsSky) decides horizon fade: check, no change needed.
- Glare: `RenderGlares` later; stock DNGlare already dims by DNClouds density, which is our weather map when
  skyCloudStockPattern = 1, so they agree.
- Reflection: CataWater calls RenderSky with the mirrored camera; build the rays from the current Gx view/proj at
  draw time (not a cached frame value), and offer `skyCloudsReflect` 0 (stock sheet there) to cap cost. Lesson
  from wxl-light a31/revert: reflection passes re-enter sky code.
- Under water RenderSky does not run: nothing to do.

### Sun/moon direction
Direct: `const C3Vector& L = DayNight::g_lightBody == 1 ? DayNight::g_moonToward : DayNight::g_sunToward;` (same as
ShadowQuery.cpp:134-147). With the arc off, fall back to VolumetricFog's ReadLightDirection logic (planet position
minus `g_dnInfo.m_cameraPos`). Light colour: `m_sunCloudColors[0..4]` (LightIntBand 9-13: sun, cloud emissive /
layer colours) and `m_skyColors` from g_dnInfo current light, storm `m_stormBlend`, `g_shadowFade` /
`g_lightStrength` for the dawn/dusk handover; the moon tints at night. Drop wxl-light's own disc, moon texture,
wxl-water sea and haze coupling.

### Risks
- Noise: 3D texture -> 2D atlas costs 2 taps per lookup (slice lerp) and loses the volume mips that `gCloudLod`
  relied on; need per-level atlases or LOD 0 (shimmer at the horizon). Biggest shader change.
- Cost: full-res 20-step march + light steps over all sky pixels; wxl-light never measured it. Add half-res +
  upsample later if a frame capture shows it (ponytail note already in Clouds.cpp).
- Vulkan/D3D11 translators: `tex2Dlod` in loops, `[branch]`, dynamic loop counts — vfog marches prove these work,
  but check SkyClouds.bls translates cleanly; `pow`/`acos` edge cases in StockUv.
- Skybox-designed zones with ugly dome colours once the gate drops their M2s.
- DNClouds density: stock clouds texture only animates without an opaque skybox (fine once gated); layout read
  must match `DNClouds` fields (m_size, m_density) — native, so safer than the plugin's raw addresses.
- The look was tuned against wxl-light's own SkyAt sky, not the stock dome; colours will need a look-dev pass.
- Interaction with the decomp's FSR/temporal (if any jitter is applied, clouds need it too or they shimmer).

### Effort
- Skybox gate + CVar: 1-2 h code, one in-game look. 
- Clouds port (C++ ~350 lines, HLSL ~200, atlas, build_shaders entry, both backends): 2-3 days including a
  look-dev pass with preview.py and the user's verdicts.
- Optional per-batch sun/moon strip in skybox M2s: +0.5-1 day, needs the HD pack's skybox files on this server.
- Optional later: cloud shadows on the ground / cloud occlusion in vfog's shadowed air (wxl-light CloudShadow,
  CloudVis): +1-2 days, touches VolumetricFog* — only after the current vfog work lands.

## 5. Decisions for the user
1. Skyboxes with the arc on: drop all (outdoors), or keep flag-2 overlays (auroras), or per-batch strip of
   suns/moons in named skyboxes (then send/point to the HD pack's skybox M2s)?
2. Sky colour: stock per-zone LightIntBand dome (as asked), or wxl-light's own SkyAt sky (zone hue at its own
   brightness), which the clouds were tuned against?
3. Cloud placement: follow the game's DNClouds pattern (zone cloud density, as wxl-light a18) or free noise
   with a coverage CVar?
4. Default on or off for skyClouds and the skybox gate in the patcher build; and in water reflections or not.

## User decisions (2026-10-22)
1. Skyboxes while SunArc on: drop them, keep the flag-2 overlays (auroras).
2. Dome colours: wxl-light's own sky colours (the ones the clouds were tuned against).
3. Cloud placement: the zone's own DNClouds density.
4. Clouds on by default, drawn in water reflections too.
