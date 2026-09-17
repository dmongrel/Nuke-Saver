// The per-frame scene uniform block.
//
// This struct and the `Scene` block in shaders/scene.glsl describe the same bytes and MUST be
// changed together. There is no reflection and no generated binding: the two are kept in step by
// hand, which is why every field here names its shader counterpart.
//
// Laid out for std140 the blunt way: every member is a vec4 or a mat4, so no member can straddle
// a 16-byte boundary and no padding rule has to be remembered. Several w components carry an
// unrelated scalar — that is deliberate packing, and each one says what it holds.
#ifndef NUKE_SAVER_SCENE_UNIFORMS_H
#define NUKE_SAVER_SCENE_UNIFORMS_H

#include "core/math.h"
#include "world/sky.h"

namespace render {

struct SceneUniforms {
    float viewProj[16]{};
    float invViewProj[16]{};  // clip -> world, for reconstructing the view ray in the sky pass

    float cameraPos[4]{};     // xyz world, w = seconds since the cycle began
    float keyDirection[4]{};  // xyz towards the light, w = body angular radius (radians)
    float keyColor[4]{};      // rgb linear radiance already scaled by intensity, w = star brightness
    float zenithColor[4]{};   // rgb linear, w = 1 when the body is the moon
    float horizonColor[4]{};  // rgb linear, w = base exposure
    float groundColor[4]{};   // rgb linear, w = ambient scale
    float bodyColor[4]{};     // rgb linear emissive, far above 1 so it blooms (spec 5.1)
    float ambientColor[4]{};  // rgb linear, w = window emission multiplier
};

static_assert(sizeof(SceneUniforms) == 2 * 64 + 8 * 16, "SceneUniforms must stay std140-tight");

// Fills everything the sky and lighting need. The camera matrices are supplied separately
// because the framing depends on the window's aspect ratio, which is per monitor.
void FillSceneUniforms(SceneUniforms* out, const world::Sky& sky, const core::Mat4& view,
                       const core::Mat4& proj, const core::Vec3& cameraPos, float elapsed);

}  // namespace render

#endif
