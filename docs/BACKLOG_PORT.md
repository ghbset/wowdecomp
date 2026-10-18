# Backlog ports (FEATURE_BACKLOG.md items)

## Grass wind (2026-10-18)

Ported from wxl-grasswind (`/root/wxl-grasswind`, main, src/GrassWind.cpp). The plugin swapped the 6 stock
`Shaders\Vertex\vs_3_0\DetailDoodad.bls` blobs at create time for copies with an injected motion block and wrote
its constants into the detail-doodad constant block. Natively the whole shader is ours:

- `Tools/CustomMpq/ShaderSrc/DetailDoodad.hlsl`: the stock grass VS, all 6 permutations (3 * point lights +
  shadow level, `CDetailDoodad::SetupVertexShader`), rewritten in HLSL, plus the plugin's motion block
  (`kMotionInject`) as HLSL: two travelling sine waves and a downwind lean bend each blade by `1 - uv.y`
  (anchored at 0.3, squared, per-blade variance and phase jitter, view-distance fade), and blades inside 2 yd of
  the player lean away from them (height cone above the player's feet). Same constants, same layout, moved from
  the plugin's c14-c21 (the point-light registers of permutations 3-5) to **c35-c42** (after the shadow matrices
  c23-c34).
- `build_shaders.py` gained `MULTI` (several permutations in one container, stock record masks a/b) and builds
  `Shaders\Vertex\vs_3_0\DetailDoodad.bls` (fxc, D3D9 and the Vulkan translation's source) and
  `Shaders\Vertex\vs_4_0\DetailDoodad.bls` (shader-xlate, D3D11). Both are committed; build_custom_mpq.py packs
  them over patch-R's SM4 copy, and translate_shaders.py picks up the vs_3_0 one from Tools/CustomMpq/Shaders.
- `DetailDoodad.cpp`: `UpdateGrassWind` (the plugin's `WriteFrameConstants` with its default settings) runs at the
  end of `InitializeShaderConstants`, once per detail-doodad pass; phases advance by frequency * dt from
  `FrameTime::s_curTimeMs`. The eye comes from the pass's own view matrix and `s_activeWorldView`, so the water
  reflection pass is correct too. `SetupVertexShader` uploads c35-c42 with every batch after the stock block.
- CVars (WorldParam_CVar.cpp): `grassWind` (default 1), `grassWindStrength` (0..4, default 1: scales both wave
  amplitudes and the lean), `grassParting` (default 1). Default on: a few dozen VS instructions on grass only, and
  off means zero constants, the stock blade positions. Advanced options: page "Grass" (before Debug).
- Not ported: the plugin's runtime disassemble/assemble fallback and blob hashing (not needed with our own
  source), its Settings.ini, the vs_2_0 profile (the plugin skipped it too; vs_2_0 clients keep still grass).

Unverified: nothing has run in game. The HLSL is a rewrite of the stock asm, checked by reading the disassembly,
not by an A/B render; with `grassWind 0` it should be pixel-identical to stock. Player position is
`CGPlayer_C::GetPosition` (world); on a transport it may not match the grass frame.
