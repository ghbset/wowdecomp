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
  stages.

## Real (V2): shaders, pipelines, constants, textures
- **Shader pack:** `ShaderCreate` / `IShaderReload` load through the vs_4_0 / ps_4_0 profile slot with its folder name
  swapped to `spv` for the call (`SpvLoadPath`), so files come from `Shaders\Vertex\spv` / `Shaders\Pixel\spv`
  (`patch-Vulkan.MPQ`); caps keep reporting vs_3_0 / ps_3_0 as on D3D11. `IShaderCreate` parses the `VKS1` body
  (input table + SPIR-V, entry `main`) into a `ShaderApi` (module, input table, serial `m_id`) on
  `CGxShader::m_apiSpecific`. An empty or non-VKS1 body leaves the shader invalid, and **a bound invalid shader skips
  the draw** (SPIRV.md "do not draw"; D3D11 falls back to FF instead). The pack was checked with libstorm (StormLib):
  all 81 files of `build/stock-host/vulkan-mpq.list` are in the MPQ byte-identical; 2749 permutations, none empty, every
  body a valid VKS1 header, inputs only POSITION/BLENDWEIGHT/BLENDINDICES(uint)/NORMAL/TEXCOORD0-1/COLOR0-1.
- **FF:** `FfShadersSpv.h` (copy of `ffshaders/out/ff_shaders_spv.h`, including D3D11's `FfShaders.h` for `FfBlob` and
  the selectors); the 48 VS + 3 PS modules are created with the device and picked with `FfSelectVs` / `FfSelectPs`
  exactly as D3D11. FF constants: D3D11's `IStateSyncFixedFunction` maps, unchanged. No point-sprite GS (D3D11 does
  not use one either); set 0 binding 3 stays on the zero buffer.
- **Pipelines:** keyed on VS `m_id`, PS `m_id`, topology class, the vertex input (attribute descriptions + binding
  count; strides are dynamic state), blend mode, colour mask and pass formats; created through one in-memory
  `VkPipelineCache`. Blend constants (GxRs Unk84, D3D9 blend factor) are dynamic state now.
- **Vertex input:** streams as D3D11's `IStateSyncVertexPtrs` (one binding per distinct buffer, stride = sum of its
  attributes; predefined formats one buffer with `m_primVertexSize`). Each VKS1 entry maps (usage, usageIndex) to a
  GxVA; the format is the D3D9 type's, switched to the shader's numeric class (UBYTE4 read as float = USCALED, SHORT2 =
  SSCALED). Entries the decl lacks read a **zero-stride binding on the zero buffer** (R32G32B32A32 float/uint/sint).
- **Constants:** dynamic-offset UBOs over one set 0 per frame, chosen over a set per draw because it needs no
  descriptor writes per draw: VS b0, PS b0 and cb1 are copied into the frame's UBO ring (32 MB, 4 KB slack) only when
  they change and bound by offset. App b0 = `s_shadowConstants` (dirty range resets as D3D11), pushing registers
  0..highest ever written; the binding stays 4 KB. A full ring skips draws for the rest of the frame (logged once).
- **cb1:** D3D11's `IStateSyncDeviceConstants`: alpha ref (`cb1[16].x`, the shaders `discard`), the 6 user clip planes
  (clip space; FF planes through inv(view·proj)), ints zero.
- **Set 1:** t0..t15 / s0..s15 from GxRs_Texture0..15 for both stages, written as one 16-descriptor write per type into
  a set from the frame's descriptor pools (1024 sets each, reset every frame, grown on demand); a new set only when a
  view or sampler changed since the last draw. Empty slots get a 1x1 zero R8G8B8A8 image. Sampler cache keyed on
  filter, wrap U/V, anisotropy and texLodBias (D3D11's texfilter rules: Nearest/Linear max LOD 0, anisotropic with
  1x = trilinear, clamped to the device limit). Texture states now call `ITexMarkAsUpdated`; TexGen states set the
  texgen matrix (D3D11's `ISetTexGen`).
- **Device recreation:** `IDestroyVk` releases every app shader's module (`m_loaded = 0`), so they rebuild on next use.

## Not done yet
- Never run on a GPU or under the validation layer.
- Every sampled image is described as SHADER_READ_ONLY_OPTIMAL (depth textures sampled while still in attachment
  layout would be wrong); no comparison samplers (`_pcf` families sample plainly, as D3D11).
- Wireframe (PolygonFill off) draws solid. ClipDistance in every shader needs `shaderClipDistance`: a GPU without it
  will fail pipeline creation (no fallback). USCALED/SSCALED vertex formats are not mandatory in Vulkan; no format
  fallback if a driver lacks them.
- Occlusion queries (caps report none), `TexCopy`/`TexStretch` (return 0), `DeviceReadPixels`/`DeviceReadDepths`
  (zero-filled), gamma ramp, stereo, MSAA (single-sampled only), device loss (a failed submit sets `m_context = 0`).
- 1555 data where `A1R5G5B5_PACK16` is not sampleable has no fallback (Blit has no 1555 → 8888), as on D3D11.
- `ponytail:` notes in the code mark the deliberate shortcuts (full barriers per transition, staging freed every frame,
  every dynamic state set per draw, one scratch RT depth, stream pool overwrite after more than 2 wraps in a frame,
  in-memory pipeline cache, solid fill only, shader modules destroyed immediately).

## Next steps
1. **First in-game test:** `Wow64.exe` (or `Wow32.exe`) + `patch-Vulkan.MPQ` in `Data\`, `SET gxApi "Vulkan"` in
   Config.wtf, Vulkan SDK validation layer installed if possible; read `Logs\gx.log` (validation messages land there).
2. **V3:** occlusion queries, texture copies/read-backs, device loss rebuild, MSAA, then WorldPost and in-process FSR
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
