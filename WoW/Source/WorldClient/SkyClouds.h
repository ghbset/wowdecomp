#ifndef WOW_SOURCE_WORLDCLIENT_SKYCLOUDS_H
#define WOW_SOURCE_WORLDCLIENT_SKYCLOUDS_H

// Not stock: wxl-light's sky and volumetric clouds (SKY-CLOUDS-PLAN.md), drawn by DayNight::RenderSky in place of
// the stock cloud sheet (DNClouds::Render). Shaders\Pixel\SkyClouds + VFog\cloudnoise.bin from custom.MPQ.
namespace SkyClouds {

void RegisterCVars();   // skyClouds, skyCloudCover, skyCloudSpeed, skyCloudsReflect
bool Active();          // this RenderSky draws ours (CVars on, shader and noise loaded)
bool Disc();            // ... and our disc stands in for the stock sun and moon (gfxSunArc on)
bool HidesGlares();     // our disc was drawn in the world view lately: DayNight::RenderGlares stays out
void Draw();            // in RenderSky's sky slice, after the dome

}  // namespace SkyClouds

#endif
