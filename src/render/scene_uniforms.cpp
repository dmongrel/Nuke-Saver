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

void SetSceneTiming(SceneUniforms* out, float growthTime, float boardRise) {
    out->timing[0] = growthTime;
    out->timing[1] = boardRise;
    out->timing[2] = 0.0f;
    out->timing[3] = 0.0f;
}

void SetBoardLight(SceneUniforms* out, const core::Vec3& position, float intensity, float radius) {
    out->boardLight[0] = position.x;
    out->boardLight[1] = position.y;
    out->boardLight[2] = position.z;
    out->boardLight[3] = intensity;

    // Amber, per spec 7.6. Held in linear scene-referred terms like every other colour here.
    out->boardColor[0] = 1.0f;
    out->boardColor[1] = 0.55f;
    out->boardColor[2] = 0.16f;
    out->boardColor[3] = radius;
}

void SetBlast(SceneUniforms* out, const core::Vec3& center, float radius) {
    out->blast[0] = center.x;
    out->blast[1] = center.y;
    out->blast[2] = center.z;
    out->blast[3] = radius;
}

void SetFire(SceneUniforms* out, const core::Vec3& center, float radius, const core::Vec3& color,
             float flash) {
    out->fireLight[0] = center.x;
    out->fireLight[1] = center.y;
    out->fireLight[2] = center.z;
    out->fireLight[3] = radius;

    out->fireColor[0] = color.x;
    out->fireColor[1] = color.y;
    out->fireColor[2] = color.z;
    out->fireColor[3] = flash;
}

void SetShadow(SceneUniforms* out, const core::Mat4& lightViewProj, uint32_t mapSize,
               float texelMetres, float depthBias) {
    StoreMat4(out->lightViewProj, lightViewProj);
    out->shadow[0] = mapSize ? 1.0f / static_cast<float>(mapSize) : 0.0f;
    out->shadow[1] = texelMetres;
    out->shadow[2] = depthBias;
    out->shadow[3] = mapSize ? 1.0f : 0.0f;
}

}  // namespace render
