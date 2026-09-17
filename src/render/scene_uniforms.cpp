#include "render/scene_uniforms.h"

#include <cstring>

namespace render {
namespace {

void StoreMat4(float* dst, const core::Mat4& m) { std::memcpy(dst, &m.m[0][0], sizeof(float) * 16); }

void Store4(float* dst, const core::Vec3& v, float w) {
    dst[0] = v.x;
    dst[1] = v.y;
    dst[2] = v.z;
    dst[3] = w;
}

}  // namespace

void FillSceneUniforms(SceneUniforms* out, const world::Sky& sky, const core::Mat4& view,
                       const core::Mat4& proj, const core::Vec3& cameraPos, float elapsed) {
    const core::Mat4 viewProj = proj * view;

    StoreMat4(out->viewProj, viewProj);
    StoreMat4(out->invViewProj, core::Inverse(viewProj));

    Store4(out->cameraPos, cameraPos, elapsed);

    // The key direction is the only place the sun or moon exists. The skybox draws the body here
    // and the lighting takes its direction from here, so the two cannot disagree (spec 6.3).
    Store4(out->keyDirection, sky.keyDirection, sky.bodyAngularRadius);

    // Radiance is folded into the colour rather than passed alongside it, so no shader can apply
    // the intensity twice or forget it.
    Store4(out->keyColor, sky.keyColor * sky.keyIntensity, sky.starBrightness);

    Store4(out->zenithColor, sky.zenithColor, sky.bodyIsMoon ? 1.0f : 0.0f);
    Store4(out->horizonColor, sky.horizonColor, sky.baseExposure);
    Store4(out->groundColor, sky.groundColor, sky.ambientScale);
    Store4(out->bodyColor, sky.bodyColor, 0.0f);
    Store4(out->ambientColor, sky.ambientColor, sky.windowEmission);
}

}  // namespace render
