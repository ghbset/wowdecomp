# CGxDeviceVulkan

Opt-in native Vulkan backend for **Wow32.exe / Wow64.exe only** (`SET gxApi "Vulkan"`, case-insensitive). D3D9 stays
the default. There is no stock-exe host: upstream wxl-vulkan-api covers that and is GPLv3, so it was read for ideas
only and no code was taken from it. The structure mirrors `../CGxDeviceD3d11` (same CGxDevice overrides, same
D3D9-derived viewport/scissor/projection/texture-upload rules). Plan: `/root/d3d11-port/NATIVE-PLAN.md` section 7.
Contracts: `/root/d3d11-port/PLAN.md` and `/root/d3d11-port/shader-xlate/SPIRV.md`.

## Registration
- `GxApi_Vulkan = 6` (after `GxApi_GLL`; `GxApis_Last` = 7), name `"Vulkan"` in `g_gxApiNames`.
- `Gx.cpp` (`GxApiSupported`, `CreateGxDevice`), `Gx.h`, `CGxDevice/Win32|Win64` AdapterFormats (reuses the D3D9 mode
  list), `ShaderEffect/ShadowCache.cpp` (depth-format shadows as D3D11).
- Headers: Khronos Vulkan-Headers **v1.4.364** (b0c3dd68) in `/root/d3d11-port/vendor/Vulkan-Headers`, CMake cache var
  `WOWDECOMP_VULKAN_HEADERS`. No import library: `vulkan-1.dll` is loaded at run time and every function comes from
  `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` (X-macro lists at the top of the .cpp; add a function there before
  using it).

## Real (V1)
- **Instance:** API 1.3, `VK_KHR_surface` + `VK_KHR_win32_surface`; `VK_LAYER_KHRONOS_validation` and
  `VK_EXT_debug_utils` when present. The messenger logs warnings and errors through `CGxDevice::Log` (Logs\gx.log).
- **Device:** first discrete GPU with Vulkan 1.3 and a graphics+present family (integrated next). Features:
  `dynamicRendering`, plus `textureCompressionBC`, `samplerAnisotropy`, `shaderClipDistance` (SPIRV.md's 6-element
  ClipDistance), `fillModeNonSolid`, `geometryShader` (FF point-sprite GS) where supported.
- **Window:** copied from D3D11 (class `GxWindowClassVulkan`, software cursor). No exclusive fullscreen: fullscreen is
  the popup window; focus loss minimises it.
- **Swapchain:** B8G8R8A8_UNORM / sRGB-nonlinear when offered; FIFO with vsync, else MAILBOX (IMMEDIATE, FIFO as
  fallbacks); min+1 images. Recreated on `GxWM_Size` (deferred to the next frame start), on `VK_ERROR_OUT_OF_DATE_KHR`
  from acquire (retried once) and on OUT_OF_DATE/SUBOPTIMAL from present. A zero extent (minimised) leaves no
  swapchain and frames record without presenting.
- **Frames:** 2 in flight (`kFrames`). Per frame: a command pool with an *upload* and a *main* command buffer
  (submitted together, upload first, so mid-frame uploads land before every draw of the frame), a fence, an acquire
  semaphore, a UBO ring, staging buffers and a deferred-delete list (resources freed while possibly in flight are
  destroyed after that frame's fence). One render-done semaphore per swapchain image.
- **Rendering:** dynamic rendering (VK 1.3), LOAD/STORE, begun lazily at the first draw or clear and ended on target
  change or present. Layouts are tracked per image; texture targets return to SHADER_READ_ONLY when their pass ends.
- **Depth:** D24_UNORM_S8_UINT when it supports attachment + sampled, else D32_SFLOAT_S8_UINT. The back-buffer depth
  and texture depth targets are attachment + SAMPLED, with a depth-aspect view (`Image::m_view`) for readers.
  Colour-only texture targets get a scratch depth of their size, as on D3D11.
- **Clear:** `vkCmdClearAttachments` clipped to the viewport rect, which is D3D9 Clear's behaviour without D3D11's
  clear quad.
- **Present:** layout to PRESENT_SRC, submit, present.
- **Viewport:** D3D11's pixel rect (D3D9 rules incl. the RT Y rule), then a negative height (core 1.1) so D3D clip
  space works with no shader flip. Scissor as D3D11, full pass when the scissor test is off. Projection: D3D11's
  `XformSetProjection` (0..1 depth).
- **Memory:** own first-fit suballocator, 64 MB blocks per (memory type, buffer/image), requests of 32 MB or more
  dedicated; host-visible blocks are persistently mapped. No VMA.
- **Buffer pools:** Static → device-local buffer; locks write a CPU copy and the unlocked range goes through staging
  (`vkCmdCopyBuffer` in the upload command buffer). Dynamic/Stream → host-visible, coherent, `kPoolVersions` (3)
  copies; locks write in place. A stream-ring wrap (D3D DISCARD) moves the pool to its next copy and waits for the
  frame that last used it; Dynamic pools write in place (D3D9 NOOVERWRITE, as D3D11). Ring logic as D3D9/D3D11.
- **Textures:** formats keep the D3D9 memory layouts: Abgr8888 → R8G8B8A8, Argb8888 → B8G8R8A8, 4444 → A4R4G4B4_PACK16,
  1555 → A1R5G5B5_PACK16, 565 → R5G6B5_PACK16, DXT1-5 → BC1-3, Uv88 → R8G8_SNORM, RG16F, R32F. Unsupported formats
  (caps from `optimalTilingFeatures`) fall back through D3D11's tolerable table (4444/8888 → Abgr8888, Blit converts).
  Uploads: Blit into staging, `vkCmdCopyBufferToImage` per mip/face, update-rect rounding as D3D11 (incl. BC block
  rounding). `generateMipMaps` → linear `vkCmdBlitImage` chain where the format can blit. Cube maps: 6 layers + CUBE
  view; render targets get a per-face attachment view.
- **Draw:** indexed (16-bit, `firstIndex` = start + buffer index / 2) and non-indexed draws, base vertex 0 (stream
  offsets on, as D3D11). Triangle fans are native (`VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN`), no rewrite.
- **State:** blend mode (D3D9 factor table, alpha uses the same factors as D3D9 did) and colour mask are baked into
  the pipeline; cull (CW front face, as D3D9), depth compare/write, depth bias (D3D9 units × 2^24), topology, vertex
  stride, viewport and scissor are VK 1.3 dynamic state.
- **Descriptor layouts per SPIRV.md:** set 0 = dynamic UBOs (0 VS b0 4 KB, 1 PS b0 4 KB, 2 device cb1 368 B, 3 FF
  point-sprite GS cb0 when geometry shaders exist); set 1 = t0..t15 at 0..15 and s0..s15 at 16..31, shared by all
  stages. The pipeline layout already has both sets.

## Stubbed in V1
- **Shaders and pipeline state:** every draw uses **one stub pipeline per key** (topology class, position attribute,
  blend, colour mask, pass formats): the VS reads only the position (location 0) and outputs
  `mul(float4(pos, 1), transpose(world·view·proj))`; the PS writes solid magenta. VS b0 c0..c3 are filled from the FF
  transforms and uploaded to the frame's UBO ring when they change; PS b0, cb1 and GS cb0 point at a zero buffer.
  Source: `stub_vs.hlsl` / `stub_ps.hlsl`, compiled to `StubShaders.h` with
  `/root/d3d11-port/vkd3d-install/bin/vkd3d-compiler -x hlsl -p vs_4_0 -b spirv-binary` (ps_4_0 for the PS) and
  spirv-val clean. UI drawn by shaders that set their own constants therefore lands wherever the FF matrices put it.
- `IShaderCreate` marks every shader invalid (logs once). No set 1 binding (textures and samplers), no alpha test,
  clip planes, fog or FF emulation constants.
- Occlusion queries (caps report none), `TexCopy`/`TexStretch` (return 0), `DeviceReadPixels`/`DeviceReadDepths`
  (zero-filled), gamma ramp, stereo, MSAA (single-sampled only), device loss (a failed submit sets `m_context = 0`).
- 1555 data where `A1R5G5B5_PACK16` is not sampleable has no fallback (Blit has no 1555 → 8888), as on D3D11.
- `ponytail:` notes in the code mark the deliberate shortcuts (full barriers per transition, staging freed every frame,
  every dynamic state set per draw, one scratch RT depth, stream pool overwrite after more than 2 wraps in a frame).
- Nothing has run on a GPU yet.

## Next steps
1. **V2 shaders + real pipelines:** load the pack from `Shaders\Vertex\spv` / `Shaders\Pixel\spv`
   (`patch-Vulkan.MPQ`; `ShaderCreate` bumping the target caps to the `spv` folder like D3D11 does for vs_4_0). Body
   `VKS1` + input table (usage, usageIndex, location, compType) + SPIR-V, size-0 permutation = do not draw; entry
   `main`. Build vertex input from the decl matched by (usage, usageIndex), zero-buffer dummy bindings for missing
   inputs. Every VS writes PointSize, so point-list pipelines are fine. FF: `ff_shaders_spv.h` (`g_ffVsSpv[48]`,
   `g_ffPsSpv[3]`, `g_ffGsPointSpriteSpv`) with D3D11's `FfSelectVs`/`FfSelectPs` and its FF constant fill.
   Pipeline key adds (VS, PS, vertex layout hash); constants: port D3D11's `IShaderConstantsFlush` dirty-range upload
   into the UBO ring for b0 (both stages) and cb1 (alpha ref, clip planes).
2. Set 1: per-draw descriptor sets (or push descriptors) for t0..t15 / s0..s15, sampler cache keyed like D3D11's.
3. **V3:** occlusion queries, texture copies/read-backs, device loss rebuild, MSAA, then WorldPost and in-process FSR
   3.1 / FSR 4 (NATIVE-PLAN 7.3).

## Build
Separate build dir `/root/d3d11-port/build-vk/{x64,x86}` (toolchain files copied from `/root/d3d11-port/build`):
```
cmake -G Ninja -S /root/d3d11-port/wowdecomp-vulkan -B /root/d3d11-port/build-vk/x64 \
  -DCMAKE_TOOLCHAIN_FILE=/root/d3d11-port/build-vk/clang-cl-x64.cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON
flock /run/tswow-deploy.lock ninja -C /root/d3d11-port/build-vk/x64 Wow
```
x86: swap `x64` for `x86`.
