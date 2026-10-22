#ifndef ENGINE_SOURCE_GX_GXUPSCALE_H
#define ENGINE_SOURCE_GX_GXUPSCALE_H

// CUSTOM (FSR, the wxl-fsr port): not in the 3.3.5a binary. The world drawn at a render scale into the top-left of
// its viewport with a jittered projection, then upscaled into the whole viewport before the UI. Only the Vulkan device
// sets g_gxUpscale (FSR 4 through a run-time provider DLL, Vulkan/Upscale.cpp); D3D9 and D3D11 leave it null and draw
// at full size. CGWorldFrame::OnWorldRender brackets the world:
//   BeginWorld   with the world viewport set: the fraction of it to draw into (1, 1 = full size: nothing else
//                happens this frame); the jitter is on from here
//   SetupView    in CWorldScene::Render once the camera is set (the view the motion vectors start from)
//   EndWorld     after the post passes (fog, tonemap), before the chat bubbles and FFX: the jitter off, the world
//                upscaled into the viewport BeginWorld saw

class C3Vector;

class IGxUpscale {
    public:
    virtual void BeginWorld(float& scaleX, float& scaleY) = 0;
    virtual void SetupView(const C3Vector& origin) = 0;   // origin: the render-relative view's origin (s_activeWorldView)
    virtual void EndWorld() = 0;
};

extern IGxUpscale* g_gxUpscale;

#endif
