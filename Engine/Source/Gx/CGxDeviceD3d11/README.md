# CGxDeviceD3d11

Opt-in D3D11 backend (`SET gxApi "D3D11"`, case-insensitive). D3D9 stays the default (`GxDefaultApi`). It is modelled on
the 4.3.4 (15595) backend (`/root/RE/wow434-d3d11-compare.md`) and follows the contracts in `/root/d3d11-port/PLAN.md`.
This class is not part of 12340, so it has no `OFFSET` tags. d3d11.dll and dxgi.dll are loaded at run time, so the exe
gains no imports.

Registered in `Gx.cpp` (`GxApiSupported`, `CreateGxDevice`), `Gx.h`, and `CGxDevice/Win32|Win64 AdapterFormats`, which
reuses the D3D9 mode list.

## Real (phase 1)
- **Library and device.** The library is loaded like 4.3.4 `ILoadD3dLib`. The device uses adapter 0 from
  `CreateDXGIFactory1`, feature levels {11_0, 10_1, 10_0}, and `SINGLETHREADED`.
- **Window and window proc.** Copied from D3D9 with their own class `GxWindowClassD3d11`. There is no hardware cursor:
  `m_hwCursor = 0`, so the base software cursor is used.
- **Swap chain.** DISCARD, B8G8R8A8 (R10G10B10A2 for the 10-bit format). The MSAA count is checked against both the
  colour and depth formats. `SHADER_INPUT` is set when single-sampled. Alt-Enter is off. `fixLag` maps to
  `SetMaximumFrameLatency(1)`.
- **Readable depth** (contract):
  - Typeless texture + DSV + SRV: `R24G8_TYPELESS` / `D24_UNORM_S8_UINT` / `R24_UNORM_X8_TYPELESS`. D16 and D32 have
    their own triples.
  - At FL 10_0 with MSAA there is no SRV.
  - `m_depthSrv` is the SRV for later phases.
  - `DeviceReadDepths` really works when MSAA is off. In 12340 D3D9 it returns zeros.
  - `DeviceCopySceneDepth` (VFOG): `CopyResource(m_depthTexture)` into a backend-owned `GxTex_D24X8` render
    target (`m_sceneDepthCopy`, R24_UNORM_X8 SRV, point filter), remade on a size change. MSAA or D16/D32: nullptr.
- **Render targets.** Texture targets, cube faces, depth targets, and a scratch depth of exactly the RT's size.
  `DeviceSave/RestoreDepthSurface` and `DeviceResolveDepthBuffer` are implemented.
- **Frame calls:**
  - Clear, Present (vsync interval as D3D9), and the viewport, including the D3D9 RT Y rule.
  - `XformSetProjection`, the same math as D3D9.
  - Resize: `ResizeBuffers`, then rebuild the views, recreate the RT textures, and call `NotifyOnDeviceRestored`.
- **Buffer pools:**
  - Static pools are DEFAULT buffers written with `UpdateSubresource`.
  - Dynamic and Stream pools are DYNAMIC buffers written with `Map` NO_OVERWRITE, or DISCARD when the stream ring wraps.
    The ring logic is identical to D3D9.
  - Every pool keeps a CPU copy that locks write into. The unlock pushes the locked range to the GPU.
- **Textures:**
  - Formats keep the D3D9 memory layouts: 8888 is RGBA/BGRA, 4444, 1555 and 565 are the 16-bit BGRA formats, plus BC1-3,
    R8G8_SNORM, RG16F and R32F.
  - Uploads blit into a scratch block, then call `UpdateSubresource` per mip and face.
  - `generateMipMaps` maps to `GenerateMips`.
  - Base-mip and recreation handling matches D3D9.
  - `TexCopy`, equal-size `TexStretch`, `DeviceCopyTex` and `DeviceReadPixels` resolve MSAA first.
- **Draw.**
  - Vertex streams (same grouping and strides as D3D9), the 16-bit index buffer, topology, and Draw/DrawIndexed.
  - The draw uses base vertex 0, because stream offsets are always on (`caps.m_streamOffset = 1`).
  - Triangle fans are rewritten as lists through a scratch IB. Indexed fans read the pool's CPU copy.
- **Occlusion queries.** D3D11 occlusion queries.
- **Caps.** Shader targets still report SM3 (vs 3, ps 4) so every client feature check matches D3D9; only the load path
  moves (see Shaders). Point sprites are 0. Clip planes are 6.

## Real (phases 2 and 4)
- **Render states** (`IRsSendToHw`, 4.3.4 0x0086FF70 model): states only edit a rasterizer / blend / depth-stencil desc
  and set dirty bits. `IStateSync` resolves them through `m_stateCache`, one `unordered_map` for the 5 kinds
  (rasterizer, blend, depth-stencil, sampler, input layout), keyed by a 64-bit FNV hash salted with the kind. Objects are
  created lazily and released in `IDestroyD3d`.
  - Blend modes from D3D9 `s_srcBlend`/`s_dstBlend`. Alpha reuses them, with colour factors swapped for alpha factors.
  - Depth func, depth write, colour mask, cull (D3D9 CW/CCW → FRONT/BACK), scissor, multisample, wireframe (master
    enable), blend factor (Unk84) and depth bias are all handled.
  - Depth bias: D3D9 units × 2^24 (exact for D24, off for D16/D32).
- **Textures and samplers:** PS SRVs are compared every draw, so a texture recreated behind the same `CGxTex*` still
  rebinds. Samplers are rebuilt when the texture or its flags change, or when `texLodBias` changes.
  - Filter: Nearest/Linear get MaxLOD 0. Anisotropic is a real `ANISOTROPIC` at the texture's `m_maxAnisotropy`
    (`textureFilteringMode`).
  - LOD bias is the client's `texLodBias` CVar. 12340 D3D9 ignored it; this replaces wxl-texfilter.
  - Render-target changes unbind all PS SRVs.
- **Scissor and viewport:** scissor as D3D9 `IStateSyncScissorRect`. The viewport keeps D3D9's MinZ/MaxZ.
- **Input layouts** (`IStateSyncInputLayout`): built from the same element list as D3D9's vertex decl, with the semantic
  and format table from shader-xlate. They are hashed on (elements, hash of the VS DXBC ISGN chunk). A failed create
  (VS reads an attribute the decl lacks) is cached as null and logged once per combination.
- **Constants:**
  - b0 per stage holds 256 float4. `IShaderConstantsFlush` maps a dirty stage with DISCARD and copies all 4 KB of
    `s_shadowConstants`.
  - b1 (`m_devConstantBuffer`, 23 float4, bound to VS and PS) is rebuilt every draw and uploaded only when it changes:
    - alpha ref = AlphaRef/255 (0 = off);
    - user clip planes enabled by `GxRs_ClipPlaneMask`. Shader draws take them as given, which is clip space as on D3D9.
      Fixed-function draws go through `inv(view*proj)ᵀ`, since D3D9 FF planes are in world space;
    - the int constants stay 0, because stock uses only `defi`.
  - All cbuffers are created zero-filled.
- **Fixed function** (`IStateSyncFixedFunction`): when no valid app VS/PS is bound, `g_ffVs[FfSelectVs(mask, lit)]` /
  `g_ffPs[FfSelectPs(tex0, tex1)]` from `FfShaders.h` are bound. These are created lazily, together with their own
  b0 buffers, filled to the `ff_common.hlsli` map:
  - world·view, `m_projNative`, and the texture matrices/sources/modes as D3D9 `IXformSetTex`/`ISetTexCoordIndex` build
    them (`ISetTexGen` is ported);
  - material, ambient-only mode, linear fog, colour-material source, 4 lights in camera space, normalize, point size
    and scale;
  - PS fog colour, stage ops (6 = no texture) and the specular add.
  - Either FF stage pairs with a translated shader of the other stage.
  - `FfShaders.h` is a copy of `/root/d3d11-port/ffshaders/out/ff_shaders.h` (sha1 9f93ab93); regenerate it with
    `python3 build.py` there and copy it over.
- **Shaders** (`IShaderCreate`): the `.bls` container is unchanged and the body is DXBC. `shader->m_code` keeps the
  bytecode for input layouts. A body without `DXBC` (no pack installed) leaves the shader invalid, logs once, and draws
  fall back to FF.
- **Loading** (`ShaderCreate`/`IShaderReload`): the shader target caps are bumped to vs_4_0 / ps_4_0 only for the length
  of the load. Files are therefore read from `Shaders\Vertex\vs_4_0\*.bls` and `Shaders\Pixel\ps_4_0\*.bls`, and
  only on this device. `IShaderLoad` has no fallback below these, so SM3 bytecode never reaches D3D11.
- **Stock-host layout:** `m_reserved397C` is an always-null `void*` at gx +0x397C, the offset where wxl add-ons read the
  D3D9 device pointer. It has a `static_assert` on x86. The object grew by 4 bytes plus the phase-2 members, which the
  stock host's 0x3F28 zeroed allocation covers (see `/root/d3d11-port/stock-host/DESIGN.md`).

## Shader pack: producing and placing it
```
cd /root/d3d11-port/shader-xlate && python3 xlate.py batch stock out   # -> out/vs_4_0/*.bls, out/ps_4_0/*.bls
```
Pack `out/vs_4_0` as `Shaders\Vertex\vs_4_0\` and `out/ps_4_0` as `Shaders\Pixel\ps_4_0\` into an MPQ that the client
loads, such as a `patch-<letter>.MPQ` in `Data\`. SFile reads loose files only from archives, so a folder on disk is not
enough. The SM3 files stay where they are, and D3D9 keeps using them.

## Taken from 4.3.4 (2026-10-16, see /root/d3d11-port/COMPARE-434.md)
- Partial-viewport clears draw a quad (SceneClearToViewport); stencil is not cleared on that path.
- Present rebuilds the device on DEVICE_REMOVED/RESET (`IRecreateD3dDevice`, shaders recreated from `m_code`).
- Fullscreen focus: `SetFullscreenState` on focus change, minimise on loss; `DXGI_MWA_NO_WINDOW_CHANGES`.
- Texture fixes: no GENERATE_MIPS on BC, per-mip update rect rounded up, DXT base under 4 px decodes to 8888.
- Half-pixel: settled. Caps pixel/texel centre on edge = 1 is right for D3D10+; 4.3.4's device applies no offset either.

## Stubs and gaps (each marked `TODO(phase N)` in the code where it has a site)
| What | Phase |
|---|---|
| Hardware-PCF shadow sampling: the `_pcf` families and the s4 shadow slot need `SampleCmp` plus a comparison sampler (translator and device) | 5 |
| SHORT2 attributes reach float VS inputs as R16G16_SINT bits (needs an int input in the translator, or conversion) | 5 |
| VS inputs that are missing from the decl fail the input layout, where D3D9 supplied defaults | 5 |
| Point sprites (caps 0; `g_ffGsPointSprite` is not bound) and vertex textures (wxl-light; SRVs are bound to PS only) | 5 / 6 |
| Hardware PCF, device half (once the translator emits `SampleCmp`): 4.3.4 IGetSamplerState 0x0086FBF0 uses `D3D11_FILTER_COMPARISON_*` + `LESS`; 12340 has no compare flag, so key it off a D24X8 texture in the slot | 5 |
| Scaled `TexStretch` (needs a StretchRect shader) | 4 |
| MSAA depth resolve for readers; `DeviceReadDepths` with MSAA returns zeros | 5 |
| Argb1555/Rgb565 *data* on pre-11.1 runtimes (no 16-bit BGRA, and Blit has no 1555/565 to 8888 converter) | 5 |
| Gamma ramp (D3D9 sets it only in fullscreen; DXGI `SetGammaControl`), stereo (no-op) | later |
| Nothing has run on a GPU yet | 5 |

## Build
These commands use the separate build dirs in `/root/d3d11-port/build` (`x64`, `x86`), with the release config and
signature checks off:
```
cmake -G Ninja -S /root/d3d11-port/wowdecomp-d3d11 -B /root/d3d11-port/build/x64 \
  -DCMAKE_TOOLCHAIN_FILE=/root/d3d11-port/build/clang-cl-x64.cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON
ninja -C /root/d3d11-port/build/x64 Wow     # -> bin/Win/Wow64.exe
```
For x86, swap `x64` for `x86` in both paths. The output is `bin/Win/Wow32.exe`, the 32-bit Wow.exe. The x86 build needs
nasm, and under clang-cl it uses `-msse2` (see the root `CMakeLists.txt`).
