#ifndef ENGINE_SOURCE_SHADEREFFECT_SHADOWCACHE_H
#define ENGINE_SOURCE_SHADEREFFECT_SHADOWCACHE_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/C4Vector.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/CFrustum.h"

class CGxTex;
class CTexture;
struct CShadowCacheQueryData;

typedef void (*ShadowMatrixCallback)(C3Vector& center, float radius, C44Matrix& matrix, C3Vector& direction, int32_t band);
typedef void (*ShadowFrustumCallback)(CFrustum& frustum, C44Matrix& matrix, const C3Vector& direction, CShadowCacheQueryData& data, int32_t band);
typedef int32_t (*ShadowQueryCallback)(CShadowCacheQueryData& data, uint32_t frame);
typedef int32_t (*ShadowRenderCallback)(CShadowCacheQueryData& data, int32_t band, CGxTex* target, CGxTex* colorTarget, C3Vector& direction);

struct CShadowBand {
    /* 0000 */ CTexture* m_textures[2];
    /* 0008 */ float m_radius;
    /* 000C */ float m_updateDistance;
    /* 0010 */ C3Vector m_lastPos;
    /* 001C */ C3Vector m_pos;
    /* 0028 */ C3Vector m_dir;
    /* 0034 */ uint32_t m_updateCount;
    /* 0038 */ uint32_t m_current;

    CShadowBand();
};

struct CShadowCacheQueryData {
    /* 0000 */ int32_t m_queryFlags[3];
    /* 000C */ int32_t m_active[3];
    /* 0018 */ int32_t m_useCullFrustum[3];
    /* 0024 */ CAaBox m_extents[3];
    /* 006C */ CFrustum m_frustums[3];
    /* 0348 */ CFrustum m_cullFrustums[3];
    /* 0624 */ CFrustum m_bandFrustums[3];
    /* 0900 */ float m_cullRects[3][4];
    /* 0930 */ C3Vector m_pos[3];
    /* 0954 */ uint8_t m_textureIndex[3];
    /* 0958 */ float m_radius[3];
    /* 0964 */ float m_bounds[3][4];
    /* 0994 */ float m_texBounds[3][4];
    /* 09C4 */ C44Matrix m_matrices[3];
    /* 0A84 */ int32_t m_lastBand;
    /* 0A88 */ int32_t m_useFrustum;

    CShadowCacheQueryData();
};

// Not stock (SHADOW_PORT_PLAN S3): the cascade bands as the receivers saw them this frame, for consumers outside the
// receiver shaders (vfog's shadowed sun). rows[i]: the texgen of band i, dp4 of a view-space position (w = 1) under
// `view` gives (u', v', ref) with uv = u'v' * 0.5 + 0.5; lit where ref <= the stored depth (Terrain2[_pcf]).
struct ShadowView {
    CGxTex* map[3];          // the front map of each band (D24X8 with pcf, else R32F); borrowed
    C4Vector rows[3][3];
    C44Matrix view;          // the Gx view rows[] are relative to (PostUpdate's); a consumer composes its own view
    uint32_t size;           // texels per side
    int32_t pcf;             // 1 = hardware compare (depth maps), 0 = R32F compared in the shader
    uint32_t stamp;          // PostUpdate count: unchanged since the consumer's last frame = not updated
};

class CShadowCache {
    public:
    static const float s_bandRadius[3];
    static const float s_bandUpdateDistance[3];
    static C4Vector s_cutoffPlane;
    static C4Vector s_cutoffPlaneDefault;
    static float s_shadowFade;   // not stock: gfxShadowFade, 0..1 of the sun/moon shadow shown (DayNight sets it)
    static void SetShadowFade(float fade) { s_shadowFade = fade; }
    static float s_debugAspect;
    static int32_t s_debugLevel;
    static CShadowBand s_extDynShadow;
    static float s_extDynShadowCullRect[4];
    static CShadowBand s_extShadowBands[3];
    static uint8_t s_frameCounter;
    static ShadowFrustumCallback s_frustumCallback;
    static CTexture* s_intDynShadow;
    static ShadowMatrixCallback s_matrixCallback;
    static int32_t s_needsRebuild;
    // Not stock (CShadowQuery sets these each frame; see ShadowCache.cpp):
    static uint32_t s_bandRedraw;     // gfxSunShadows: bit i = redraw cached band i once, wherever the camera is
    static int32_t s_texelSnap;       // gfxShadowSnap: map centres snapped to whole texels in light space
    static int32_t s_haveBandCentre;  // gfxShadowAnchor: the bands follow s_bandCentre instead of the viewer
    static C3Vector s_bandCentre;
    static C4Vector s_pcfOffsets[8];
    static C44Matrix s_publishedView;  // S3: PostUpdate's Gx view, for GetPublished
    static uint32_t s_publishStamp;
    static ShadowQueryCallback s_queryCallback;
    static int32_t s_recreateTextures;
    static ShadowRenderCallback s_renderCallback;
    static EShadowMode s_shadowMode;
    static C4Vector s_shadowMatrices[12];
    static float s_shadowScale;
    static uint32_t s_shadowSize;
    static const char* s_shadowModeDescs[NUM_SHADOW_MODES + 1];
    static const uint32_t s_shadowValues[NUM_SHADOW_MODES + 1];
    static C3Vector s_sunDir;
    static C4Vector s_sunDirViewSpace;
    static float s_textureSize[2];

    static void ClearBandTextures();
    static void Destroy();
    static void GatherBands(CShadowCacheQueryData& data, const C3Vector& pos, CFrustum& frustum, int32_t useCullFrustum);
    static uint32_t GetMaxShadowMode();
    static int32_t GetPublished(ShadowView& out);
    static EShadowMode GetShadowMode();
    static const char* GetShadowModeDesc(uint32_t mode);
    static uint32_t GetShadowValue();
    static void Init();
    static int32_t Initialize();
    static void InitPcfOffsets();
    static void __fastcall NotifyTexturesRecreate();
    static void PostUpdate();
    static void PreUpdate(const C3Vector& sunDir);
    static void RenderBands(CShadowCacheQueryData& data);
    static void SetDebugParams(float aspect, int32_t level);
    static void SetFrustumCallback(ShadowFrustumCallback callback);
    static void SetMatrixCallback(ShadowMatrixCallback callback);
    static void SetQueryCallback(ShadowQueryCallback callback);
    static void SetRenderCallback(ShadowRenderCallback callback);
    static void SetShadowMapDetailDoodad();
    static void SetShadowMapGenericGlobal();
    static void SetShadowMapGenericInterior(int32_t interior);
    static void SetShadowMapTerrain();
    static int32_t SetShadowMode(EShadowMode mode);
    static void SetShadowScale(float scale);
    static int32_t Update(const C3Vector& pos, int32_t useCullFrustum);
    static int32_t ValidateShadowMode(EShadowMode mode);
};

#endif
