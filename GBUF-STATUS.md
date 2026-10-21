# G-buffer normals (gxNormalBuffer): status

Branch `gbuffer-normals` (worktree `/root/wowdecomp-wt/gbuf`, from main f024f395). Goal: the screen-space lamp
(LegionLights, CVar `lightMode`) and mountain-shadow sun (VFogShadowMask, CVar `shadowMask`) passes light by the
world's smooth interpolated vertex normals instead of normals rebuilt from depth (faceted). CVar `gxNormalBuffer`
(default 1; 0 = the old depth-rebuilt normals). Never push, never publish.

**Default API is Vulkan** (`GxDefaultApi`, Engine/Source/Gx/Gx.cpp:119), not D3D9; D3D9 is the fallback when the
Vulkan device or its shader manifest fails. D3D9 was still done first (simplest runtime, and its bytecode is the
source every other backend translates).

## Design

One rewrite of the stock SM3 bytecode feeds all three backends, so no world shader is rewritten by hand:

- `Tools/VulkanShaders/bls/normals.py` (`inject`, `rewrite_record`, CLI `stats` / `pack`):
  - **vs_3_0** (every record except ui / shadowmap / liquids / waterripples) gains `dcl_texcoord7 oN` + a `def`
    on a free constant. A record with a `dcl_normal` input writes its transformed normal there (taint analysis from
    the normal input; captured before the first `dp3` that dots a fully normal-derived temp with something not
    normal-derived, or duplicated from the instruction that writes it to an output), `.w = 1`. No normal: `oN = 0`.
    Every stock family transforms the normal into **view space** at that point (M2 bone rows c31+, WMO c31-33,
    terrain c0-c2: their fog reads `.z` of the same space), which is LegionLights' space.
  - **ps_3_0** families `combiners_*`, `mapobj*`, `terrain*`, `detaildoodad` gain `dcl_texcoord7 vN` and
    `oC1 = float4(normalize(vN.xyz) * 0.5 + 0.5, vN.w)` before `end`. a = 0 = "no normal here".
  - Stock corpus (`/root/d3d11-port/shader-xlate/stock`): every lit VS record captures; only `mapobjcomposite`
    perms 8-15 are refused (no free input/temp: they keep the stock bytes, so no normal there).
- The passes include `Tools/CustomMpq/ShaderSrc/vf_gbuffer.hlsli`: `GBufferNormal(pixel, p, rebuilt)` samples s4 when
  c9.x = 1 and a > 0.5, else returns the depth-rebuilt normal. So unsupported APIs, `gxNormalBuffer 0`, water,
  fixed-function draws and unpatched shaders all fall back per pixel.
- Host: `VolumetricFog::BindNormalBuffer()` (VolumetricFogRenderer.cpp, next to `Bind`) binds
  `device->DeviceNormalBuffer()` on s4 and sets c9; called by LegionLights `Draw` and the shadow-mask pass
  (`RenderShadowMask`, before `RsSet(GxRs_PixelShader, s_shaders.shadowMask)`). `EndPasses` unbinds s0-s4 and restores c9.
- CVar `gxNormalBuffer` is registered in `LegionLights::RegisterCVars` and copied to `CGxDevice::s_normalBuffer` at the
  top of `LegionLights::OnWorld` every frame.
- Device contract (`CGxDevice.h`, after `DeviceSceneDepthIsCopy`): `virtual CGxTex* DeviceNormalBuffer()` (base
  returns nullptr) + `static bool s_normalBuffer`. `CGxShader::m_writesNormal` (CGxShader.h) = the pixel shader writes
  oC1; the backend sets it.

## Per backend

### D3D9 (`CGxDeviceD3d`): DONE (code), untested in game
`Engine/Source/Gx/CGxDeviceD3d/CGxDeviceD3d.cpp`, search `GBUFFER`:
- `INormalCreate` (from `IStateSetD3DDefaults`, after `ICreateSceneDepth`): A8R8G8B8 render-target texture of the back
  buffer's size; MSAA back buffer = a matching multisampled RT surface, StretchRect-resolved into the texture when read.
  Needs NumSimultaneousRTs >= 2, INDEPENDENTWRITEMASKS and MRTPOSTPIXELSHADERBLENDING, else off (fallback).
- `INormalRelease` (with `IReleaseSceneDepth`, before Reset).
- `INormalSync` (every draw, in `IStateSync` after `IRsSync`): RT1 is bound and `D3DRS_COLORWRITEENABLE1 = 0xF` only for
  a pixel shader with `m_writesNormal`, blend Opaque/AlphaKey, colour write on, RT0 = back buffer; every other draw
  keeps it bound with mask 0. First bind a frame `ColorFill`s it to 0.
- Unbound in `SceneClear` (D3D9 Clear would fill it) and in `DeviceSetRenderTarget` when RT0 is not the back buffer.
- `m_writesNormal` set in `IShaderCreatePixel` by finding the token `0x800F0801` (`mov oC1, r#`).
- `DeviceNormalBuffer`: unbinds RT1 (it is sampled next; the next oC1 draw rebinds it), resolves MSAA, wraps it in a
  CGxTex like `d3d9scenedepth::Copy`. nullptr when nothing was written this frame.
- **Shader source for D3D9**: the injected stock .bls must be in an MPQ (the client reads stock bytes otherwise; then
  nothing writes oC1, and the passes fall back). Make it with
  `python3 -I Tools/VulkanShaders/bls/normals.py pack <extracted Shaders tree with vs_3_0/ps_3_0 dirs> <out>`
  and pack `<out>/vs_3_0`, `<out>/ps_3_0` as `Shaders\Vertex\vs_3_0\`, `Shaders\Pixel\ps_3_0\` into a patch MPQ
  (stock-derived bytes: host it like `/root/d3d11-port/stock-host/out/patch-Vulkan.MPQ`, never commit it). Tested on the
  stock corpus: 68 files written, every record decodes back. NOT YET PACKED/SHIPPED.
- `CGxDeviceD3d9Ex` (gxApi D3D9Ex) is not done: it returns nullptr (fallback). Port the same 4 functions if wanted.

### Vulkan (`CGxDeviceVulkan`, the default): DONE (code), untested in game
- Translator: `bls/archive.py` decodes `normals.rewrite_record(...)` (the md5 stays that of the bytes the client
  loads); `bls/emitter.py` emits `layout(location = 1) out vec4 oC1` and the manifest line `flag normal_out`.
  `normals.inject` skips a record that already has `dcl_texcoord7` (so translating a client whose custom.MPQ carries
  the D3D9 normal pack does not inject twice). Every injected record is listed twice in the manifest: under the md5
  of the bytes the client loads and under the md5 of the injected bytes (`Record.alias`, a second `record` block
  with no names, same .spv), so Vulkan finds its modules whether or not custom.MPQ carries the D3D9/D3D11 normal
  pack. Checked: 3183 records (limit 4096); every one of the 2615 records in the normal pack has a manifest entry. Full translation of the user's client (`/root/tswow-server/tswow-client`,
  read only): 1655 records, all validated, 757 pixel records `normal_out`.
- Runtime (search `GBUFFER` under `Engine/Source/Gx/CGxDeviceVulkan`):
  - `Frame/Targets.cpp`: `CreateNormalImages` (after `CreateMsImages`, with the scene): RGBA8 `s.normal` (+ `s.msNormal`
    at the scene's sample count), only with `independentBlend`. The scene's passes (`PassNormal(target)`, PassSet key
    `normal`) get it as colour attachment 1: LOAD/STORE, resting in COLOR_ATTACHMENT_OPTIMAL; MSAA: attachment 4
    (multisampled) resolved into 5 every pass instance. Framebuffers over the scene carry it. Texture targets: unchanged.
  - `Frame.cpp` `ClearNormal`: zeroed (vkCmdClearColorImage) before the first scene pass after each present.
  - Pipelines: `Draw::normalTarget` (set in `CGxDeviceVulkanDraw.cpp` before `Record`: 0 no attachment, 1 masked,
    2 writable when gxNormalBuffer) -> `Key::normal`; `Build.cpp` makes 2 blend attachments, attachment 1 mask 0xF
    only for a `normal_out` pixel module with blend Opaque/AlphaKey and a colour mask, else 0.
  - `Manifest.cpp` parses `flag normal_out` (`Record::normalOut`); `CGxShader::m_writesNormal` set from it.
  - `DeviceNormalBuffer` (CGxDeviceVulkanTexture.cpp): blits `s.normal` (`frame::CopyNormalFromScene`, Readback.cpp)
    into a render-target `GxTex_Argb8888` CGxTex of the scene's size, recreated on resize. Null when off / no
    independentBlend / not cleared this frame.
  - Compatibility: an old exe skips the unknown `flag` line (malformed-line counter) and its 1-attachment passes drop
    location 1; a new exe with old shaders never writes the target and the passes fall back per pixel.
- **Deploy (how translated shaders reach the user):** the Vulkan SPIR-V ships inside `Data/custom.MPQ`
  (release.config.json entry "tswow-client/Data/custom.MPQ"; the exes read `VulkanShaders\` from it). Rebuild it with
  `python3 -I Tools/VulkanShaders/translate_shaders.py --client <client> --deploy` (writes `<client>/VulkanShaders`),
  then `python3 -I Tools/CustomMpq/build_custom_mpq.py --client <client> --out <client>/Data/custom.MPQ`
  (also needs the generated terrain_fade shaders), then the patcher publish. NOT done (no publish from this branch).

### D3D11 (`CGxDeviceD3d11`): DONE (code), untested in game
`CGxDeviceD3d11.cpp`, search `GBUFFER`:
- `IOMSetTargets()` replaces the three `OMSetRenderTargets(1, m_curRtv, m_curDsv)` sites (IBindRenderTargets,
  DeviceRestoreDepthSurface, DeviceResolveDepthBuffer): RT1 = `INormalRtv()` when RT0 is the back buffer,
  gxNormalBuffer is on and the target has not been sampled this frame.
- `INormalRtv()`: `m_normalTex`, a render-target `GxTex_Argb8888` CGxTex of the back buffer's size (its RTV when
  single-sampled); MSAA: `m_normalMs` + `m_normalMsRtv` at the back buffer's sample count. Recreated on resize /
  device loss; cleared to 0 when made. Released with the back buffer.
- Blend: `IndependentBlendEnable = TRUE`, `RenderTarget[1]` = opaque with mask 0; IStateSync sets its mask to 0xF
  only for a pixel shader with `m_writesNormal` and blend <= AlphaKey and a colour mask (part of the hashed desc).
- `m_writesNormal`: `DxbcWritesTarget1` (OSGN has SV_Target register 1), set in IShaderCreate. Checked on the pack.
- `DeviceNormalBuffer()`: first call a frame unbinds RT1 (`m_normalSampled`) and resolves MSAA into the CGxTex;
  returns it. ScenePresent clears the target, drops it from any PS slot, rebinds RT1.
  ponytail: opaque draws after the first lighting pass of a frame write no normal (fine while lights / sun / SSAO
  all run after the opaque world).
- Shaders: the SM4 pack comes from `normal_pack.py` (below), through custom.MPQ.

### D3D9 / D3D11 shader pack (`Tools/CustomMpq/ShaderSrc/normal_pack.py`): DONE, not shipped
- Input: shader-xlate's `stock/{vs,ps}_3_0` (SHA1SUMS-checked), with ours of the same name taken instead
  (terrain_fade's Terrain2/3[_pcf], so run `terrain_fade.py` first). Every `normals.wanted` family is rewritten
  (vs_3_0 / ps_3_0) and compiled by shader-xlate's `compile_one` (+ `pcf_slots`) to vs_4_0 / ps_4_0.
- Output: `Tools/CustomMpq/Normals/{Vertex,Pixel}/{vs,ps}_{3,4}_0/*.bls` (gitignored: stock-derived). 44 ps + 20 vs
  families, 2615 SM3 records, all compile. Two runs gave byte-identical output (sha1 of every file).
- `build_custom_mpq.py` packs `Normals` as `Shaders\...` above ours and the stock SM4 pack (a warning, not a refusal,
  when it is missing: D3D9/D3D11 then light from depth). Test build (scratch, not shipped): 5307 files, 128 normal shaders.
- **Ship (not done):**
  ```
  python3 -I Tools/CustomMpq/ShaderSrc/terrain_fade.py
  python3 -I Tools/CustomMpq/ShaderSrc/normal_pack.py
  python3 -I Tools/VulkanShaders/translate_shaders.py --client <client> --deploy
  python3 -I Tools/CustomMpq/build_custom_mpq.py --client <client> --out <client>/Data/custom.MPQ
  ```
  then the patcher publish of Data/custom.MPQ (+ Wow32.exe / Wow64.exe from this branch).

## Not done yet
- SSAO is DONE (LegionSsao PASS 0 uses `GBufferNormal`; host `BindNormalBuffer()` before `DrawToTarget(ao[0], ...)`
  in VolumetricFogRenderer.cpp). Caustics (`VFogCaustics.hlsl:56`) still rebuilds from depth: include
  `vf_gbuffer.hlsli`, call `GBufferNormal`, `BindNormalBuffer()` in its host pass (caustics already uses c9 for
  cCausticSun: move vf_gbuffer's register or the caustic's), add sampler 4 to its `build_shaders.py` mask, rebuild with
  `python3 -I Tools/CustomMpq/ShaderSrc/build_shaders.py <names>` (needs wine + /root/wxl-depth/tools fxc).
- LegionLights still pays for the 4 depth taps when the normal buffer answers (move them into the fallback branch if it shows).
- In-game verification on every backend; shipping custom.MPQ with the normal pack (commands above).
- `CGxDeviceD3d9Ex` (gxApi D3D9Ex): still returns nullptr (fallback).

## Builds
- Dirs: `/root/wowdecomp-xbuild/wt-gbuf-x64`, `/root/wowdecomp-xbuild/wt-gbuf-x86` (configured like build-both.sh).
- `cd <dir> && flock /run/tswow-deploy.lock /root/d3d11-port/tools/build-capped.sh ninja -k 0 Wow > LOG 2>&1`
- Logs: `/root/wowdecomp-xbuild/wt-gbuf-x64-build.log`, `/root/wowdecomp-xbuild/wt-gbuf-x86-build.log`.
- Phase 1 (commit "gbuffer: ..."): x64 and x86 build clean (rc=0), Wow64.exe / Wow32.exe in `bin/Win`. Not run.

## What to check in game (D3D9, once the normal pack is installed)
`SET gxApi "D3D9"`, `lightMode 2`, `shadowMask 1`: lamps on walls / characters / ground and the mountain shadow's sun
side should shade smoothly across triangles; `gxNormalBuffer 0` must give the old faceted look; MSAA on and off.
