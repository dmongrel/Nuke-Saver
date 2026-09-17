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

    // Phase timing (spec 4). Kept here rather than derived in each shader from the cycle time,
    // because the phase boundaries are drawn per cycle and the GPU has no way to know them.
    float timing[4]{};  // x = seconds into the growth phase, y = board rise, zw reserved

    // The countdown board as a light source (spec 7.6). It MUST illuminate the rooftops beneath
    // it — plainly at night, subtly at noon, never absent — so it is a real light in the scene
    // uniform rather than an emissive surface that happens to be bright.
    float boardLight[4]{};  // xyz world position of the glyph band, w = intensity
    float boardColor[4]{};  // rgb linear amber, w = falloff radius in metres

    // The blast shell (spec 7.2). Here rather than only in the compute push block because the
    // building and board vertex shaders need it too: a box the shell has reached MUST stop being
    // drawn on the frame its fragments start, or the city is both standing and in pieces.
    float blast[4]{};  // xyz impact point, w shell radius in metres

    // The fireball as a light (spec 8.2): from phase 5 it MUST be the dominant source in the
    // scene. Every surface pass reads these, so it is one point light in the scene block rather
    // than a parameter threaded through four pipelines.
    float fireLight[4]{};  // xyz world centre, w radius in metres; w = 0 means there is no fire
    float fireColor[4]{};  // rgb linear emissive magnitude, w = flash intensity (spec 7.2)
};

static_assert(sizeof(SceneUniforms) == 2 * 64 + 14 * 16, "SceneUniforms must stay std140-tight");

// Fills everything the sky and lighting need. The camera matrices are supplied separately
// because the framing depends on the window's aspect ratio, which is per monitor.
void FillSceneUniforms(SceneUniforms* out, const world::Sky& sky, const core::Mat4& view,
                       const core::Mat4& proj, const core::Vec3& cameraPos, float elapsed);

// The two fields that change within a cycle rather than with the camera.
void SetSceneTiming(SceneUniforms* out, float growthTime, float boardRise);
void SetBoardLight(SceneUniforms* out, const core::Vec3& position, float intensity, float radius);
void SetBlast(SceneUniforms* out, const core::Vec3& center, float radius);
void SetFire(SceneUniforms* out, const core::Vec3& center, float radius, const core::Vec3& color,
             float flash);

}  // namespace render

#endif
