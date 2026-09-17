#include "world/camera.h"

#include "core/rng.h"

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
