// The orbiting camera (spec 11.1).
//
// "The camera is never still." That is the one hard requirement here: a screen saver showing a
// still frame is the single failure this project cannot afford, so every parameter below is a
// function of time and none of them has a hold, a cut or a stop anywhere in its range.
//
// The framing solver that spec 11.1 describes needs the phase 8 cloud extent, which does not
// exist until M4. What lands here is the orbit itself and the shot library; the solver arrives
// with the thing it has to solve for. Until then the radius is sized from the city, which is the
// same calculation with one fewer constraint.
#ifndef NUKE_SAVER_WORLD_CAMERA_H
#define NUKE_SAVER_WORLD_CAMERA_H

#include "core/math.h"

#include <cstdint>

namespace world {

// The shot library of spec 11.1. Each is a different relationship between height, radius and
// look-at over the cycle, not merely a different starting position.
enum class ShotType {
    DistantRidge,  // high and far, the whole basin in frame, creeping in as it descends
    LowApproach,   // low and fast across the desert floor, rising slightly
    HighOblique,   // steep and descending, the city read as a plan becoming an elevation
    StreetLevel    // close, rising out from among the buildings
};

struct CameraState {
    core::Vec3 eye{};
    core::Vec3 target{};
    core::Vec3 up{0.0f, 1.0f, 0.0f};
    float      fovY = core::Radians(55.0f);

    core::Mat4 View() const { return core::LookAt(eye, target, up); }
    core::Mat4 Projection(float aspect, float zNear, float zFar) const {
        return core::Perspective(fovY, aspect, zNear, zFar);
    }
};

class OrbitCamera {
public:
    // `cityRadius` is the radius of the city footprint in metres, `cycleSeconds` the full run
    // length. Both set scale only; the shape of the motion is the same regardless.
    static OrbitCamera Create(uint64_t seed, ShotType shot, float cityRadius, float cycleSeconds);

    // `t` is seconds since the cycle began. Defined beyond cycleSeconds as well: the orbit simply
    // continues, so a cycle that overruns drifts rather than snapping.
    CameraState Evaluate(float t) const;

    ShotType shot() const { return shot_; }
    float    orbitRadius() const { return radiusMid_; }

    // Seconds for a full revolution. Spec 11.1 asks for 4 to 8 minutes, so one 80 to 115 second
    // cycle sweeps 30 to 90 degrees of arc rather than a full turn.
    float revolutionSeconds() const { return revolutionSeconds_; }

private:
    ShotType shot_ = ShotType::DistantRidge;

    float startBearing_      = 0.0f;
    float direction_         = 1.0f;  // plus or minus one, randomised per cycle (spec 11.1)
    float revolutionSeconds_ = 360.0f;

    // Radius and height each ease between two values across the cycle, on their own schedules,
    // so the track is not a flat circle. Spec 11.1 requires them to vary slowly and
    // independently.
    float radiusStart_ = 0.0f, radiusEnd_ = 0.0f, radiusMid_ = 0.0f;
    float heightStart_ = 0.0f, heightEnd_ = 0.0f;
    float targetHeightStart_ = 0.0f, targetHeightEnd_ = 0.0f;
    float fovStart_          = 0.0f, fovEnd_ = 0.0f;
    float cycleSeconds_      = 100.0f;

    // A slow lateral drift of the look-at, small enough to read as a hand on the tripod rather
    // than as a second motion competing with the orbit.
    float driftAmplitude_ = 0.0f;
    float driftPhase_     = 0.0f;
};

ShotType    PickShot(uint64_t seed);
const char* ShotName(ShotType shot);

}  // namespace world

#endif
