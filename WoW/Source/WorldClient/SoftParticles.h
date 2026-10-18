#ifndef WOW_SOURCE_WORLDCLIENT_SOFTPARTICLES_H
#define WOW_SOURCE_WORLDCLIENT_SOFTPARTICLES_H

// SOFTPARTICLES: not in 12340. Port of wxl-light Soft.cpp (a35): blended M2 particles and ribbons fade where they
// meet the scene behind them, so smoke, fire and spell effects stop cutting a hard line into the ground, a wall or
// the water. docs/BACKLOG_PORT.md "Soft particles".

#include <cstdint>

namespace SoftParticles {

// CVars softParticles / softParticlesDistance (WorldParam_CVar.cpp).
extern int32_t s_enabled;
extern float s_distance;   // fade distance in yards, 0.05..20 (the plugin's soft_distance)

// CGWorldFrame::OnWorldRender, around scene->Draw(M2PASS_1) and scene->Draw(M2PASS_2): copies the scene depth as
// drawn so far (the plugin's MidWorld) and arms g_softParticleDraw for the pass.
void BeginPass();
void EndPass();

} // namespace SoftParticles

#endif
