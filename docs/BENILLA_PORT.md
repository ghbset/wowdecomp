
## 2026-10-18 benillafx: foam wakes, cloud-dimmed glare, glow clamp, steady weather

Source: /root/benilla (Rust/Bevy 1.12 remake). Three of the four effects are stock 1.12 behaviour that the
3.3.5a client already has, so only the weather rate is ported.

- **Foam wakes (skipped, stock):** benilla `water_fx` is the 1.12 `CWater0Ripple` wade wake/ring/step-in
  splash (splash.blp, wake.blp), units only, no boats. 3.3.5a has the same system: `CGUnit_C` →
  `World::CreateWaterRipple` → `CMap::CreateWaterRipple`, drawn by `CMap::RenderWaterRipples` 0x0079D5E0
  (WaterRipple.cpp) on the stock water path. vfog7 already hides those sprites only while its own ripples draw
  (`VolumetricFog::WaterHidesClientRipples`), so with vfogWater 0 / vfog off the stock wakes show.
- **Glare dimmed by clouds (skipped, stock):** benilla `occ1_sun = 1 − coverage` (1.12 0x6cf7b0) and the moon tent
  (0x6cf7d0) are exactly `DNSunGlare::GetCloudDensityFade` 0x007F1020 / `DNMoonGlare::GetCloudDensityFade`
  0x007F1040, which `DNGlare::Update` already multiplies into the fade (`DNClouds::GetDensity` at the glare point).
- **Glow clamp (skipped, stock):** benilla `ffx_glow.wgsl` clamps every scene read to 1.0 only to mimic the
  reference's byte targets. 3.3.5a FFX targets are Argb8888 (`FFX::CreateTex`), and `FFX::Finish` copies the
  back buffer into `s_full[0]`, so the Box4/Gauss4 blur already sees saturated input.
- **Steady weather rate (ported), CVar `weatherSteadyRate` (default 1):** stock Rain/Snow/Sand `UpdatePackets`
  spawn `lrint(min(dt, 1/60)·rate − 0.5)` per frame and drop the remainder, so weather thins below 60 fps (half
  at 30) and loses a fraction every frame above it (benilla documents the same at 0x67846f and works around it with a
  fixed 30 fps gain). `WeatherSpawnCount` (MapWeather.cpp) accumulates `min(dt, 0.25)·rate` with the fraction
  carried, so the per-second count is the nominal rate at any frame rate. 0 = stock. CPU only, no backend or shader
  change (D3D9/D3D11/Vulkan identical); cost a few flops per frame.
