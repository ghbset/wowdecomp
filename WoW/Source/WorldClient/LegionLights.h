// LEGION lights: not in 12340 (Legion survey 2.8, backlog #7). The frame's M2 point lights on the surfaces around
// them, one screen-space pass from the scene depth (LegionLights.cpp, docs/LEGION_PORT.md "Legion lights").
#pragma once

namespace LegionLights {

void RegisterCVars();               // from CWorldParam::Initialize
// CGWorldFrame::OnWorldRender: false before scene->Draw(M2PASS_0) (lightMode 1: terrain and WMO only), true after
// the solid world (lightMode 2: models and grass too).
void OnWorld(bool solidDone);

}  // namespace LegionLights
