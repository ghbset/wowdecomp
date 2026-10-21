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

### Vulkan (`CGxDeviceVulkan`, the default): shaders DONE, runtime TODO
- Translator: `Tools/VulkanShaders/bls/archive.py` `enumerate_records` decodes `normals.rewrite_record(...)` (the md5 the
  device looks modules up by stays the stock record's); `bls/emitter.py` now accepts `oC1` (`layout(location = 1) out
  vec4 oC1`) besides oC0. TEXCOORD7 = varying location 9 (already in VARYING_LOCATION). `Shaders/FfSem.vert` already
  writes `texcoord7 = 0`. Checked: all 3509 stock-corpus records (injected) emit GLSL that glslang compiles (0 refused).
  The user's client tree must be re-translated (`translate_shaders.py --client ... --deploy`).
- Runtime left:
  1. `Vulkan/Frame/Targets.cpp:62-85`: the scene render pass gets a 2nd colour attachment (R8G8B8A8_UNORM, same sample
     count; MSAA: resolve attachment or a resolve copy before sampling), cleared to 0 at the pass's load (loadOp CLEAR
     with clear value 0, independent of the frame's colour clear). Framebuffers + every pass compatible with it.
  2. `Vulkan/Pipelines/Build.cpp:340-456`: `blending.attachmentCount = 2` for pipelines in that render pass; attachment 1
     `colorWriteMask = 0xF` only when the pixel module writes oC1 (manifest outputs / GLSL `oC1`) and blend is
     Opaque/AlphaKey, else 0 (an unwritten location is undefined in Vulkan). It must be part of the pipeline key.
  3. Mark `CGxShader::m_writesNormal` from the manifest record (CGxDeviceVulkanShader.cpp ~line 94-103).
  4. `DeviceNormalBuffer()` override: end/split the pass or use the copy pattern of `DeviceCopySceneDepth`
     (snapshot into a sampled image), return a CGxTex.
  5. The pipelines of other render passes (UI, offscreen RTs) keep attachmentCount 1.

### D3D11 (`CGxDeviceD3d11`): shaders DONE (translation verified), runtime TODO
- `shader-xlate/sm3hlsl.py` already maps oC1 -> SV_Target1 and TEXCOORD7 -> tc7; `xlate.py one` on injected records
  (diffuse_t1, terrain, combiners_opaque, terrain3, mapobjdiffuse) -> ok + spirv ok. To ship: run `xlate.py batch` on the
  `normals.py pack` output instead of the stock dir (`/root/d3d11-port/shader-xlate`, outside this repo).
- Runtime left: `CGxDeviceD3d11.cpp:1238` / `:1297` / `:1313` `OMSetRenderTargets(1, ...)` -> 2 RTVs when RT0 is the
  back buffer and gxNormalBuffer; R8G8B8A8 texture + RTV + SRV (MSAA: ResolveSubresource on read). Blend desc
  (`:2150`, `:2924`): `IndependentBlendEnable = TRUE`, `RenderTarget[1].RenderTargetWriteMask` 0xF only for oC1 shaders
  with Opaque/AlphaKey blend (it is part of the hashed blend desc). Clear RT1 to 0 with `ClearRenderTargetView` each frame.
  `m_writesNormal` from the DXBC OSGN (SV_Target1). Unbind RT1 (or snapshot) in `DeviceNormalBuffer`.

## Not done yet
- SSAO is DONE (LegionSsao PASS 0 uses `GBufferNormal`; host `BindNormalBuffer()` before `DrawToTarget(ao[0], ...)`
  in VolumetricFogRenderer.cpp). Caustics (`VFogCaustics.hlsl:56`) still rebuilds from depth: include
  `vf_gbuffer.hlsli`, call `GBufferNormal`, `BindNormalBuffer()` in its host pass (caustics already uses c9 for
  cCausticSun: move vf_gbuffer's register or the caustic's), add sampler 4 to its `build_shaders.py` mask, rebuild with
  `python3 -I Tools/CustomMpq/ShaderSrc/build_shaders.py <names>` (needs wine + /root/wxl-depth/tools fxc).
- LegionLights still pays for the 4 depth taps when the normal buffer answers (move them into the fallback branch if it shows).
- In-game verification on every backend; the D3D9 normal pack.

## Builds
- Dirs: `/root/wowdecomp-xbuild/wt-gbuf-x64`, `/root/wowdecomp-xbuild/wt-gbuf-x86` (configured like build-both.sh).
- `cd <dir> && flock /run/tswow-deploy.lock /root/d3d11-port/tools/build-capped.sh ninja -k 0 Wow > LOG 2>&1`
- Logs: `/root/wowdecomp-xbuild/wt-gbuf-x64-build.log`, `/root/wowdecomp-xbuild/wt-gbuf-x86-build.log`.
- Phase 1 (commit "gbuffer: ..."): x64 and x86 build clean (rc=0), Wow64.exe / Wow32.exe in `bin/Win`. Not run.

## What to check in game (D3D9, once the normal pack is installed)
`SET gxApi "D3D9"`, `lightMode 2`, `shadowMask 1`: lamps on walls / characters / ground and the mountain shadow's sun
side should shade smoothly across triangles; `gxNormalBuffer 0` must give the old faceted look; MSAA on and off.
