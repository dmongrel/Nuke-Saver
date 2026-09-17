// The key light's shadow frustum (spec 8.2, 11.2).
//
// The fit is the part of shadow mapping that is worth testing on the CPU: if the box the light
// looks through does not contain the things that cast, the symptom on screen is a shadow that
// stops at an invisible line, and finding that by eye means noticing the absence of something.
// Here it is a containment check over every sun angle the four times of day can produce.

#include "render/shadow.h"
#include "selftest_check.h"

#include <cmath>
#include <cstdio>

namespace selftest {
namespace {

using render::ShadowBounds;
using render::ShadowViewProj;

// Where a world point lands in the light's clip space. Orthographic, so w is 1 and there is no
// divide to undo, but it is done anyway: a fit that quietly produced a projective matrix would
// otherwise pass this and fail on the GPU.
core::Vec3 ToClip(const core::Mat4& m, const core::Vec3& p) {
    const core::Vec4 v = m * core::Vec4{p.x, p.y, p.z, 1.0f};
    const float      w = std::fabs(v.w) > 1e-6f ? v.w : 1.0f;
    return core::Vec3{v.x / w, v.y / w, v.z / w};
}

bool InsideClip(const core::Vec3& c) {
    return c.x >= -1.0001f && c.x <= 1.0001f && c.y >= -1.0001f && c.y <= 1.0001f &&
           c.z >= -0.0001f && c.z <= 1.0001f;
}

// The eight corners of the box the fit was given.
void Corners(const ShadowBounds& b, core::Vec3 out[8]) {
    int n = 0;
    for (int ix = 0; ix < 2; ++ix)
        for (int iy = 0; iy < 2; ++iy)
            for (int iz = 0; iz < 2; ++iz)
                out[n++] = core::Vec3{ix ? b.radius : -b.radius, iy ? b.top : 0.0f,
                                      iz ? b.radius : -b.radius};
}

}  // namespace

void TestShadow() {
    std::printf("shadow frustum (spec 8.2)\n");

    // Sized like a real cycle: a city of a few hundred metres, a debris field out to the shell's
    // reach, a cloud a kilometre up.
    const ShadowBounds bounds{1100.0f, 1300.0f};

    // Every sun angle the four times of day reach (spec 5.4), plus straight overhead and hard on
    // the horizon, which are the two the fit's up-vector guard exists for.
    bool allInside = true;
    bool affine    = true;
    int  sampled   = 0;

    for (int elevationDeg = 0; elevationDeg <= 90; ++elevationDeg) {
        for (int azimuthDeg = 0; azimuthDeg < 360; azimuthDeg += 17) {
            const float e = core::Radians(static_cast<float>(elevationDeg));
            const float a = core::Radians(static_cast<float>(azimuthDeg));
            const core::Vec3 toLight{std::cos(e) * std::cos(a), std::sin(e),
                                     std::cos(e) * std::sin(a)};

            const core::Mat4 m = ShadowViewProj(toLight, bounds);

            core::Vec3 corners[8];
            Corners(bounds, corners);
            for (const core::Vec3& c : corners) {
                const core::Vec4 v = m * core::Vec4{c.x, c.y, c.z, 1.0f};
                if (std::fabs(v.w - 1.0f) > 1e-5f) affine = false;
                if (!InsideClip(ToClip(m, c))) allInside = false;
            }
            ++sampled;
        }
    }

    Check(sampled > 1500, "the sweep covers every elevation at many bearings");
    Check(allInside, "every corner of the world box lands inside the light's clip volume");
    // The receiver divides by nothing and the caster's depth is compared directly, so a fit that
    // quietly turned projective would be wrong everywhere and obvious nowhere.
    Check(affine, "the fit stays orthographic -- w is 1 at every corner");

    // Containment alone is satisfied by a frustum the size of the solar system, which would give
    // one texel per building. The fit must also be tight: something has to touch each bound.
    {
        const core::Vec3 toLight = core::Normalize(core::Vec3{0.3f, 0.8f, 0.5f});
        const core::Mat4 m       = ShadowViewProj(toLight, bounds);

        core::Vec3 corners[8];
        Corners(bounds, corners);

        float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
        for (const core::Vec3& c : corners) {
            const core::Vec3 clip = ToClip(m, c);
            minX = std::fmin(minX, clip.x);  maxX = std::fmax(maxX, clip.x);
            minY = std::fmin(minY, clip.y);  maxY = std::fmax(maxY, clip.y);
        }
        CheckNear(minX, -1.0f, 0.01f, "the fit touches the left edge of the map");
        CheckNear(maxX, 1.0f, 0.01f, "and the right");
        CheckNear(minY, -1.0f, 0.01f, "and the bottom");
        CheckNear(maxY, 1.0f, 0.01f, "and the top");
    }

    // Depth has to be usable, not merely in range: a box a kilometre deep squeezed into the first
    // thousandth of the buffer is a shadow map that cannot tell a roof from the ground under it.
    {
        const core::Vec3 toLight = core::Normalize(core::Vec3{0.0f, 1.0f, 0.0f});
        const core::Mat4 m       = ShadowViewProj(toLight, bounds);
        const float      ground  = ToClip(m, core::Vec3{0.0f, 0.0f, 0.0f}).z;
        const float      roof    = ToClip(m, core::Vec3{0.0f, bounds.top, 0.0f}).z;
        Check(roof < ground, "a higher caster is nearer the light in depth");
        Check(ground - roof > 0.5f, "and the box fills most of the depth range");
    }

    // The map is one of spec 11.2's five levers and the ladder must actually descend.
    Check(render::ShadowMapSize(0) == render::ShadowMapSize(1),
          "shadow resolution does not move on the first step down (spec 11.2 order)");
    Check(render::ShadowMapSize(2) < render::ShadowMapSize(1), "but it moves on the second");
    Check(render::ShadowMapSize(3) < render::ShadowMapSize(2), "and again at the bottom rung");

    // The receiver offsets along its normal by a texel before looking up, so a texel that grew
    // with the map would push every shadow off its caster.
    const float coarse = render::ShadowTexelWorldSize(bounds, 1024);
    const float fine   = render::ShadowTexelWorldSize(bounds, 4096);
    Check(coarse > fine, "a bigger map has smaller texels");
    CheckNear(coarse / fine, 4.0f, 0.01f, "and the two scale with the map exactly");
}

}  // namespace selftest
