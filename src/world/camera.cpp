#include "world/camera.h"

#include "core/rng.h"

#include <initializer_list>

namespace world {

using core::Lerp;
using core::Radians;
using core::SmoothStep;
using core::Vec3;

ShotType PickShot(uint64_t seed) {
    switch (core::Rng(seed).Index(4)) {
        case 0:  return ShotType::DistantRidge;
        case 1:  return ShotType::LowApproach;
        case 2:  return ShotType::HighOblique;
        default: return ShotType::StreetLevel;
    }
}

const char* ShotName(ShotType shot) {
    switch (shot) {
        case ShotType::DistantRidge: return "distant-ridge";
        case ShotType::LowApproach:  return "low-approach";
        case ShotType::HighOblique:  return "high-oblique";
        case ShotType::StreetLevel:  return "street-level";
    }
    return "?";
}

OrbitCamera OrbitCamera::Create(uint64_t seed, ShotType shot, float cityRadius,
                                float cycleSeconds) {
    core::Rng rng = core::Rng(seed).Fork(0xCAFE);

    OrbitCamera cam;
    cam.shot_         = shot;
    cam.cycleSeconds_ = cycleSeconds > 1.0f ? cycleSeconds : 100.0f;
    cam.startBearing_ = rng.Range(0.0f, core::kTwoPi);
    cam.direction_    = rng.Chance(0.5f) ? 1.0f : -1.0f;  // spec 11.1

    // Four to eight minutes per revolution, so a cycle sweeps 30 to 90 degrees of arc. Drawn per
    // cycle rather than fixed, so the drift rate itself varies between runs.
    cam.revolutionSeconds_ = rng.Range(240.0f, 480.0f);

    // Radii are multiples of the city radius, so a small city does not get a distant camera.
    // Heights are absolute metres where the shot is defined by height: street level is street
    // level whatever the city measures.
    switch (shot) {
        case ShotType::DistantRidge:
            cam.radiusStart_       = cityRadius * rng.Range(3.4f, 4.2f);
            cam.radiusEnd_         = cam.radiusStart_ * rng.Range(0.82f, 0.94f);
            cam.heightStart_       = cityRadius * rng.Range(0.85f, 1.15f);
            cam.heightEnd_         = cam.heightStart_ * rng.Range(0.70f, 0.85f);
            cam.targetHeightStart_ = cityRadius * 0.16f;
            cam.targetHeightEnd_   = cityRadius * 0.22f;
            cam.fovStart_          = Radians(rng.Range(46.0f, 52.0f));
            cam.fovEnd_            = Radians(rng.Range(44.0f, 50.0f));
            break;

        case ShotType::LowApproach:
            cam.radiusStart_       = cityRadius * rng.Range(2.6f, 3.2f);
            cam.radiusEnd_         = cam.radiusStart_ * rng.Range(0.70f, 0.82f);
            cam.heightStart_       = rng.Range(18.0f, 40.0f);
            cam.heightEnd_         = rng.Range(90.0f, 160.0f);
            cam.targetHeightStart_ = cityRadius * 0.22f;
            cam.targetHeightEnd_   = cityRadius * 0.30f;
            cam.fovStart_          = Radians(rng.Range(58.0f, 66.0f));
            cam.fovEnd_            = Radians(rng.Range(54.0f, 62.0f));
            break;

        case ShotType::HighOblique:
            cam.radiusStart_       = cityRadius * rng.Range(2.0f, 2.6f);
            cam.radiusEnd_         = cam.radiusStart_ * rng.Range(1.05f, 1.25f);
            cam.heightStart_       = cityRadius * rng.Range(1.9f, 2.5f);
            cam.heightEnd_         = cityRadius * rng.Range(0.95f, 1.25f);
            cam.targetHeightStart_ = 0.0f;
            cam.targetHeightEnd_   = cityRadius * 0.28f;
            cam.fovStart_          = Radians(rng.Range(50.0f, 56.0f));
            cam.fovEnd_            = Radians(rng.Range(52.0f, 60.0f));
            break;

        case ShotType::StreetLevel:
            cam.radiusStart_       = cityRadius * rng.Range(1.15f, 1.45f);
            cam.radiusEnd_         = cam.radiusStart_ * rng.Range(1.25f, 1.55f);
            cam.heightStart_       = rng.Range(8.0f, 22.0f);
            cam.heightEnd_         = cityRadius * rng.Range(0.55f, 0.80f);
            cam.targetHeightStart_ = cityRadius * 0.10f;
            cam.targetHeightEnd_   = cityRadius * 0.34f;
            cam.fovStart_          = Radians(rng.Range(62.0f, 70.0f));
            cam.fovEnd_            = Radians(rng.Range(52.0f, 60.0f));
            break;
    }

    cam.radiusMid_      = (cam.radiusStart_ + cam.radiusEnd_) * 0.5f;
    cam.driftAmplitude_ = cityRadius * rng.Range(0.02f, 0.05f);
    cam.driftPhase_     = rng.Range(0.0f, core::kTwoPi);

    return cam;
}

float OrbitCamera::FramingFill(const Subject& subject, float t, float aspect) const {
    const CameraState state = Evaluate(t);

    const Vec3 forward = core::Normalize(state.target - state.eye);

    // A view basis. Degenerate only if the camera looks straight up or down, which no shot does.
    Vec3 right = core::Cross(forward, state.up);
    if (core::Length(right) < 1e-4f) return 1e6f;
    right          = core::Normalize(right);
    const Vec3 realUp = core::Cross(right, forward);

    const float halfV = state.fovY * 0.5f;
    const float halfH = std::atan(std::tan(halfV) * (aspect > 0.0f ? aspect : 1.0f));

    // The silhouette of an upright cylinder is bounded by its two rims, so sampling the rims is
    // enough — no point on the surface projects outside the hull of those samples.
    constexpr int kSamples = 32;

    float worst = 0.0f;
    for (int i = 0; i < kSamples; ++i) {
        const float a  = core::kTwoPi * static_cast<float>(i) / static_cast<float>(kSamples);
        const float cx = std::cos(a) * subject.radius;
        const float cz = std::sin(a) * subject.radius;

        for (float y : {0.0f, subject.height}) {
            const Vec3 p{cx, y, cz};
            const Vec3 d = p - state.eye;

            const float dz = core::Dot(d, forward);

            // Behind the camera, or level with it. Either way the subject is not in front of the
            // lens and no scale of this shot frames it.
            if (dz <= 1.0f) return 1e6f;

            const float h = std::fabs(std::atan(core::Dot(d, right) / dz)) / halfH;
            const float v = std::fabs(std::atan(core::Dot(d, realUp) / dz)) / halfV;

            worst = std::fmax(worst, std::fmax(h, v));
        }
    }

    return worst;
}

void OrbitCamera::FrameOn(const Subject& subject, float aspect, float holdFraction, float margin) {
    if (subject.radius <= 0.0f) return;

    const float limit = 1.0f - core::Clamp(margin, 0.0f, 0.6f);

    const float radiusStart = radiusStart_;
    const float radiusEnd   = radiusEnd_;
    const float heightStart = heightStart_;
    const float heightEnd   = heightEnd_;

    const auto applyScale = [&](float k) {
        radiusStart_ = radiusStart * k;
        radiusEnd_   = radiusEnd * k;

        // Height scales with radius so the shot keeps its angle onto the city — scale only the
        // radius and a high oblique becomes a plan view and a street-level shot becomes a crane.
        // Floored, because a camera below eye height is not a camera angle, it is a bug.
        heightStart_ = std::fmax(heightStart * k, 6.0f);
        heightEnd_   = std::fmax(heightEnd * k, 6.0f);

        radiusMid_ = (radiusStart_ + radiusEnd_) * 0.5f;
    };

    const auto fits = [&](float k) {
        applyScale(k);

        // The subject has to be framed through the opening stretch, and has to stay in front of
        // the camera for the whole cycle — the orbit creeps inward, and a shot that frames the
        // city at t=0 can still fly into it at t=0.8.
        for (int i = 0; i <= 48; ++i) {
            const float t    = cycleSeconds_ * static_cast<float>(i) / 48.0f;
            const float fill = FramingFill(subject, t, aspect);
            if (fill > 1e5f) return false;
            if (t <= cycleSeconds_ * holdFraction && fill > limit) return false;
        }
        return true;
    };

    // Monotone in k: pulling the camera out can only shrink what it sees and can only move it
    // further from the centre, so a bisection is exact rather than a search over a bumpy space.
    // The upper bound has to cover the largest subject this will ever be asked to frame, which is
    // the phase 8 cloud and not the city — at 8 the search gave up on anything a few kilometres
    // across and silently returned the shot unchanged, which is a framing solver that does
    // nothing on exactly the case spec 11.1 calls the binding constraint.
    float lo = 0.08f, hi = 0.08f;
    while (hi < 64.0f && !fits(hi)) hi *= 1.25f;

    if (!fits(hi)) {
        // Nothing in the range worked. Leave the shot as it was drawn rather than committing to
        // an arbitrary scale: a shot that ignores the solver is recoverable, a shot inside the
        // city is not.
        applyScale(1.0f);
        return;
    }

    for (int i = 0; i < 28; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (fits(mid)) hi = mid;
        else lo = mid;
    }

    applyScale(hi);
}

CameraState OrbitCamera::Evaluate(float t) const {
    // Bearing is strictly linear in time. Everything else eases, but the orbit itself must not:
    // an eased bearing would slow at the ends of the cycle, and spec 11.1 forbids the camera
    // stopping or holding at any point, including during the countdown.
    const float bearing = startBearing_ + direction_ * core::kTwoPi * (t / revolutionSeconds_);

    // The eased parameters run on a normalised cycle position. Past the end of the cycle they
    // hold at their end values while the orbit continues, so an overrun drifts rather than snaps.
    const float u = SmoothStep(core::Clamp(t / cycleSeconds_, 0.0f, 1.0f));

    // Independent schedules: radius leads, height lags by 8% of the cycle. Sharing one curve
    // would make the pair move as a single zoom, which is the flat circular track spec 11.1
    // rules out.
    const float uRadius = SmoothStep(core::Clamp(t / cycleSeconds_ * 1.15f, 0.0f, 1.0f));
    const float uHeight = SmoothStep(core::Clamp((t / cycleSeconds_ - 0.08f) / 0.92f, 0.0f, 1.0f));

    const float radius = Lerp(radiusStart_, radiusEnd_, uRadius);
    const float height = Lerp(heightStart_, heightEnd_, uHeight);

    CameraState state;
    state.eye = Vec3{std::sin(bearing) * radius, height, std::cos(bearing) * radius};

    // The look-at drifts laterally, perpendicular to the view direction, by a couple of metres.
    const float drift = std::sin(t * 0.21f + driftPhase_) * driftAmplitude_;
    state.target      = Vec3{std::cos(bearing) * drift,
                        Lerp(targetHeightStart_, targetHeightEnd_, u),
                        -std::sin(bearing) * drift};

    state.up   = Vec3{0.0f, 1.0f, 0.0f};
    state.fovY = Lerp(fovStart_, fovEnd_, u);
    return state;
}

}  // namespace world
