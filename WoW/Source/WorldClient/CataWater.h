// Cata water: Cataclysm 4.3.4's max-quality water surface (ProcWater VS + ProcWaterAbove / Below PS) drawn by the
// stock water materials in place of vsLiquidWater / psLiquidWater, waterStyle 1. Not in the 3.3.5a binary.
// Ported from wxl-water (src/Shaders.hpp, src/Water.cpp kCataRows @ 17cf9af); docs/BACKLOG_PORT.md "Cata water".
#pragma once

#include <cstdint>

class C3Vector;

namespace Liquid {
    class CInstance;
    class CMaterialSettings;
}

namespace CataWater {

void RegisterCVars();
// Liquid::CRenderer::Flush, transparent pass, before the first draw: with waterStyle 1 and water in the pass, copies
// the world viewport's colour and the scene depth for the refraction and shoreline. False: Cata water is off this pass.
bool Arm(Liquid::CInstance** instances, uint32_t count);
void Done();                    // after the pass's last draw
// CMaterialWater(NoSpec)::Render, after Liquid::SetShaderConstants, in place of the stock shader binds: our shaders,
// textures and constants. False (not armed, no Cata data for this row, a texture still loading): bind the stock ones.
bool Bind(Liquid::CMaterialSettings* settings, bool specular, const C3Vector& cameraPos);
// CGWorldFrame::OnWorldRender, after the world (vfog's OnWorldDone), before the chat bubbles: Cata's underwater
// screen tint with the camera in water.
void OnWorldDone();
bool DrawsRipples();            // waterStyle 1 with waterCataRipples: the client's splash sprites give way
void Shutdown();

}  // namespace CataWater
