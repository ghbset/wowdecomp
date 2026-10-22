#ifndef WOW_SOURCE_UIUTIL_ACTIONCAM_H
#define WOW_SOURCE_UIUTIL_ACTIONCAM_H

// Not stock: Legion 7.3.5's ActionCam (over-shoulder, dynamic pitch, head movement, target focus) and Legion's
// zoom smoothing, ported in-tree from the wxl-camera extension (/root/wxl-camera). Driven by the retail
// test_camera* CVars plus wxl_cameraLegionZoom / wxl_cameraSizeFactor; everything off by default, and with
// every CVar at its default the camera is stock (each call below returns without touching anything).
// Called from CGCamera::CalcTargetCamera, CollideCameraWithWorld and CanSmoothTargetFacing (Camera.cpp).

#include <cstdint>
#include "Engine/Source/Tempest/C3Vector.h"

class CGCamera;
class CGObject_C;

namespace ActionCam {
void RegisterCVars();
void Reset();                                                   // world entry

void FrameBegin(CGCamera* cam, CGObject_C* target, uint32_t now);
void AfterSmoothing(CGCamera* cam, uint32_t now);               // after UpdateTargetSmoothing
void CollideBegin(CGCamera* cam, const C3Vector& base, C3Vector& offset);
C3Vector BoomShift();                                           // added to the boom ray's end
void BoomHit(float t);                                          // the boom ray hit at fraction t
C3Vector SweepOffset(const C3Vector& offset);                   // offset the near-plane sweep sees
void CollideEnd(CGCamera* cam, uint32_t bits, float distance, float height, C3Vector& offset);
void FrameEnd(CGCamera* cam, uint32_t now);

bool RefusePivot(CGCamera* cam);                                // smart pivot refused (dynamic pitch)
float DesiredZoom();                                            // Lua GetCameraZoom()
}

#endif
