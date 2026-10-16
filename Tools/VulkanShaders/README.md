# Tools/VulkanShaders

The offline shader translator of the Vulkan backend: it turns the 3.3.5a client's own DX9 shaders
(the `.bls` containers in its MPQ archives) into the SPIR-V modules `CGxDeviceVulkan` draws the world
and the models with. Plain Python 3.8+ (standard library only) and
[glslang](https://github.com/KhronosGroup/glslang/releases).

Nothing extracted from the client is versioned in this repository: run the translator against your own
client, and keep its output (`translated/` here, gitignored) out of commits.

## Quick start

```bash
python Tools/VulkanShaders/translate_shaders.py --client "C:\Games\World of Warcraft 3.3.5a" --deploy
```

| Option | |
| --- | --- |
| `--client <folder>` | the 3.3.5a (12340) client folder, or its `Data` folder (required) |
| `--glslang <path>` | `glslang.exe` / `glslangValidator.exe`; otherwise `$GLSLANG`, `$VULKAN_SDK`, then `PATH` |
| `--out <folder>` | the output tree (default: `translated/` in this folder) |
| `--deploy` | also copy `manifest.txt`, `index.txt` and every `.spv` into `<client>\VulkanShaders\`, removing stale modules |
| `--names A:0,B --print` | look at the GLSL of a few records, write nothing |

The archives are read in the client's own order: `common.MPQ`, `common-2.MPQ`, `expansion.MPQ`,
`lichking.MPQ`, `patch.MPQ`, then `patch-2.MPQ` .. `patch-9.MPQ` and `patch-A.MPQ` .. `patch-Z.MPQ`; a
later archive wins, so a custom patch that replaces a shader is translated too. A folder patch (loose
files) is not read.

## Where the device looks

`CGxDeviceVulkan` reads `VulkanShaders\manifest.txt` beside the client executable (the folder of
`WowClient64.exe` / `Wow.exe`), with `vertex\` and `pixel\` beside it, or the folder named by the
`GX_VULKAN_SHADERS` environment variable. Without a manifest every stock shader stays invalid and the
engine draws through its fixed-function paths (white or vertex-coloured, texture stage 0 only).

## What a run writes

Per byte-distinct shader record, under `vertex/` or `pixel/`: `<md5>.glsl`, `.spv`, `.spvasm` (the GLSL
4.50 source, the SPIR-V, glslang's disassembly), plus `manifest.txt` (the device's contract: one block per
record, its engine names, samplers, inputs, outputs, flags and companion) and `index.txt` (one line per
engine name, for people). The md5 is of the record's bytes as the engine keeps them in
`CGxShader::m_code`: the `size` bytes after the 16-byte record header, version token through END token.
The device looks modules up by that md5, so a translated tree always matches the client it was made from.

The default run wipes `vertex/` and `pixel/` first: the tree is always one run's output.

## How the translation works

Every record the engine loads is resolved through the engine's own fallback chain (vertex
vs_3_0 -> vs_2_0 -> vs_1_1, pixel ps_3_0 -> ps_2_0 -> ps_1_4 -> ps_1_1), skipping empty containers the way
the engine does, and folded by md5. Each one is decoded with `bls/dx9ir.py` and rewritten by
`bls/emitter.py` as a GLSL 4.50 module whose interface is fixed **by semantic, not by register**, so any
vertex module pairs with any pixel module: vertex inputs at fixed locations per semantic, varyings at
fixed locations per semantic, the constant registers as a uniform block (`v[64]` or `v[256]`, `p[16]`,
set 1), the samplers as combined image samplers at set 0 binding `<slot>`. glslang compiles it with
`-V --target-env vulkan1.0 --spirv-val`.

A record that uses something no rule covers is refused: logged, listed as a comment at the top of the
manifest, nothing emitted. On a stock 3.3.5a client every record translates.

The four vs_2_0 weather records (`patter`, `rain`, `sand`, `snowpoint`) write the fixed-function outputs
`oFog` / `oPts` and have no pixel shader in any profile; for them the translator emits the epilogues
(`fog0`, `gl_PointSize`), flags them `fixed_fog` / `point_size` in the manifest, and generates a
**companion** pixel module (the fixed-function stage-0 cascade, alpha test, fog blend and point-sprite UV)
as a `record ... pixel ff` block.

## File formats, for reference

**`.bls` container**: `<dir>\<profile>\<name>.bls`, e.g. `Shaders\Pixel\ps_3_0\UI.bls`. Header: `uint32
magic 'GXSH'` (`0x47585348`), `uint32 version` (`0x10003`), `uint32 permutationCount`. Then that many
records: `uint32 a; uint32 b; uint16 c; uint16 d; uint32 size; uint8 code[size];` padded to 4. For the D3D
profiles the code is a raw DX9 token stream. A missing or bad file makes the engine fall back one profile
and retry.

**`shaders\effects\*.wfx`**: plain text, the statement of which vertex shader is paired with which pixel
shader, with the fixed-function fallback.
