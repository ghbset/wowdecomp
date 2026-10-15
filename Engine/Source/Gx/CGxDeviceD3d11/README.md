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
  - The draw uses base vertex 0, because stream offsets are always on (`caps.unk_0010 = 1`).
  - Triangle fans are rewritten as lists through a scratch IB. Indexed fans read the pool's CPU copy.
- **Occlusion queries and constant buffers.**
  - Queries are D3D11 occlusion queries.
  - Two dynamic cbuffers are created and bound at b0 (VS and PS). Their size is `(256 + 32) * 16` bytes.
- **Caps.**
  - The shader targets report the SM3 profiles (vs 3, ps 4), so `IShaderLoad` reads the vs_3_0/ps_3_0 `.bls` files that
    phase 4 translates.
  - Point sprites are 0 and clip planes are 0.

## Stubs (each marked `TODO(phase N)` in the code)
| What | Phase |
|---|---|
| `IRsSendToHw`: render states into the 5 hashed state objects; texture/sampler/shader/scissor binding | 2 |
| `IShaderConstantsFlush`: dirty `s_shadowConstants` ranges to the cbuffers; final int/bool/alpha-test tail size | 2 |
| `IStateSyncInputLayout`: input layout hashed per (decl, VS signature). **Nothing draws visibly until this exists** | 2 |
| Fixed-function lights, material, xforms into the FF shaders; user clip planes (`SV_ClipDistance`) | 3 |
| `IShaderCreate`: VS/PS objects from the shader-xlate output (the VS bytecode must be kept for input layouts) | 4 |
| Scaled `TexStretch` (needs a StretchRect shader) | 4 |
| Clears are full-view, while D3D9 clears only the viewport rect (`ID3D11DeviceContext1::ClearView` if it matters) | 5 |
| MSAA depth resolve for readers; `DeviceReadDepths` with MSAA returns zeros | 5 |
| Argb1555/Rgb565 *data* on pre-11.1 runtimes (no 16-bit BGRA, and Blit has no 1555/565 to 8888 converter) | 5 |
| Gamma ramp (D3D9 sets it only in fullscreen; DXGI `SetGammaControl`), stereo (no-op) | later |

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
