# vk-camfix status (2026-10-22, budget reached, no fix yet)

## Symptom, read from the screenshots
The "hexagonal umbrella" is a hexagonal tent seen from inside its own origin, looking up the model's +Z.
The orange hexagon is the smoke hole, with the orange ground behind it showing through. It sits at exactly the same
screen position and size in every bad frame while the camera turns. The log and the white tusk in the other frames
look the same way. So whole M2 draws get an IDENTITY model-view (bones or world times view) with the correct
projection. They do not get another draw's matrix or the light view.

## Ruled out (diffs read line by line)
- 9d04ece0 (upstream merge): every Gx, Model2, CFrustum, Camera, World, Map, shadow and M2Shared change is a
  rename that keeps the value. GxTexGen_World/View = 2/3 as before. M2BLEND, M2COMBINER, GxBlend, GxCmp and
  TYPE_* (masks 0x8 and so on) keep their values. The particle-to-GxBlend mapping is unchanged. The frustum
  planes and corners are unchanged.
- 65da1c8a fork-fixes: no rendering code.
- f024f395 msdf: adds only FragmentProgram::Msdf = 5 (the pipeline Key is not bit-packed, no collision) and
  needsRegs for Msdf.
- So the published exe differs from Wow64.exe.bak-pre-msdf (built about 84a081c1, vfog39) only by the three
  merges above, and none of them touches the M2 transform path. bak-pre-msdf very probably has the same bug.
  The user should A/B it to confirm.
- Vulkan uniform ring (Vulkan/Pipelines/Regs.cpp): the generation skip and the slice reuse look correct.
- An invalid shader has m_valid = 0, so the engine's own fixed path (proper world matrix) draws it. It does not
  fall to identity.

## Open leads, in order
1. vfog DrawFadingEarly (WoW/Source/WorldClient/VolumetricFog.cpp:998, b14e5a20 vfogFadeEarly). It draws M2s
   that are fading at their draw distance with a new CM2SceneRender outside the normal pass. The shapes are distant
   doodads fading in while the camera turns. Test: `vfogFadeEarly 0`. If the giants stop, the early draw is missing
   the per-model bone/view set-up.
2. CM2SceneRender::Draw sets View = World = identity (CM2SceneRender.cpp:1157-1162). A draw that reaches the
   Vulkan FixedFunctionSemantic path (IModelViewProj) instead of the translated M2 VS renders exactly this image.
   Count it with g_stats.pairsFixedVertex, or log the draw's vertex program when world and view are both identity.
3. AnimateSM (M2Model.cpp:6218) writes bone 0 without setting m_frameStamp, from the shadow gather.
4. Tree sway or leaves: ruled out as the cause. The tent, log and tusk are not trees.

## User checks
`vfogFadeEarly 0`, then `gxApi d3d11` on the same spot. A/B against Wow64.exe.bak-pre-msdf.
