#include "render/shadow.h"

#include <algorithm>
#include <cmath>

namespace render {
namespace {

// The eight corners of the box the map has to cover.
void Corners(const ShadowBounds& b, core::Vec3 out[8]) {
    const float r = std::fmax(b.radius, 1.0f);
    const float h = std::fmax(b.top, 1.0f);
    int         n = 0;
    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                out[n++] = core::Vec3{ix ? r : -r, iy ? h : 0.0f, iz ? r : -r};
            }
        }
    }
}

}  // namespace

core::Mat4 ShadowViewProj(const core::Vec3& towardLight, const ShadowBounds& bounds) {
    core::Vec3 toLight = towardLight;
    if (core::LengthSq(toLight) < 1e-8f) toLight = core::Vec3{0.0f, 1.0f, 0.0f};
    toLight = core::Normalize(toLight);

    core::Vec3 corners[8];
    Corners(bounds, corners);

    const core::Vec3 center{0.0f, std::fmax(bounds.top, 1.0f) * 0.5f, 0.0f};

    // How far back the eye has to sit for the whole box to be in front of it. The box's own
    // half-diagonal is an upper bound on that whatever direction the light comes from, so there
    // is nothing to solve.
    float diagonal = 0.0f;
    for (const core::Vec3& c : corners) {
        diagonal = std::fmax(diagonal, core::Length(c - center));
    }
    const float back = diagonal + 1.0f;

    // Plain world up. A light straight overhead makes that the degenerate choice, but LookAt
    // already falls back to the other axis when the cross product collapses, and the sun never
    // gets above 85 degrees anyway (spec 5.4). A second guard here was tried and removed: it
    // could not be made to fail a test, because it was not protecting against anything.
    const core::Mat4 view =
        core::LookAt(center + toLight * back, center, core::Vec3{0.0f, 1.0f, 0.0f});

    // Fitted to the box rather than to a sphere around it. A sphere is the usual choice because it
    // does not change size as the light turns, but nothing here turns within a cycle, and the box
    // is the tighter of the two by a wide margin on a scene this flat.
    float minX = 1e30f, maxX = -1e30f;
    float minY = 1e30f, maxY = -1e30f;
    float minZ = 1e30f, maxZ = -1e30f;
    for (const core::Vec3& c : corners) {
        const core::Vec4 v = view * core::Vec4{c.x, c.y, c.z, 1.0f};
        minX = std::fmin(minX, v.x);  maxX = std::fmax(maxX, v.x);
        minY = std::fmin(minY, v.y);  maxY = std::fmax(maxY, v.y);
        minZ = std::fmin(minZ, v.z);  maxZ = std::fmax(maxZ, v.z);
    }

    // View space looks down -Z, so the near and far distances are the negated bounds, and the
    // nearer of the two is the larger z. A metre of slack at each end keeps a caster exactly on
    // the plane from being clipped out of its own shadow by rounding.
    const float zNear = std::fmax(-maxZ - 1.0f, 0.01f);
    const float zFar  = -minZ + 1.0f;

    return core::Orthographic(minX, maxX, minY, maxY, zNear, zFar) * view;
}

float ShadowTexelWorldSize(const ShadowBounds& bounds, uint32_t mapSize) {
    if (mapSize == 0) return 0.0f;

    // The widest the fitted box can be across the light's view is its diagonal, which is what the
    // fit gives when the light comes in at a corner. Using that rather than the current fit means
    // one number for the whole cycle and an offset that is never too small.
    const float r = std::fmax(bounds.radius, 1.0f);
    const float h = std::fmax(bounds.top, 1.0f);
    const float across = std::sqrt(4.0f * r * r + h * h);
    return across / static_cast<float>(mapSize);
}

uint32_t ShadowMapSize(int qualityLevel) {
    // The top two rungs share a size. Spec 11.2 sacrifices fragments, particles and bloom before
    // it reaches shadow resolution, so the lever is not supposed to move on the first step down;
    // giving level 0 its own larger map would make it move on every step instead.
    switch (std::clamp(qualityLevel, 0, 3)) {
        case 0:
        case 1:  return 2048;
        case 2:  return 1024;
        default: return 512;
    }
}

}  // namespace render
