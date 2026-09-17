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

// What the framing solver has to keep in frame: an upright cylinder on the city axis, from the
// ground to `height`.
//
// A cylinder rather than a sphere, because everything this has to frame over a cycle is much
// wider than it is tall — a disc of boxes 1.5 km across and 180 m high in phase 1, a debris field
// in phase 10 — and a sphere around a shape like that is nearly all empty air. Framed as a sphere,
// the city was pushed back until a 730 m ball fitted the frame and then occupied a quarter of it.
//
// The mushroom cloud of phase 8 is the one subject that is taller than it is wide, and a cylinder
// describes that correctly too. It is the binding constraint on the orbit once M4 exists.
struct Subject {
    float radius = 0.0f;  // horizontal, metres
    float height = 0.0f;  // top above the ground
};

// One thing that has to be in frame, and the stretch of the cycle it has to be in frame for.
// Spec 11.1 lists five of these and they do not all apply at once: the city has to fit during
// phase 1 and is rubble by phase 8, the cloud does not exist until phase 8 and is the widest thing
// in the cycle when it does. Solving every subject over the whole cycle would frame for a cloud
// that is not there yet and leave the city a speck for the half of the run before it appears.
struct FramingWindow {
    Subject subject;
    float   from = 0.0f;
    float   to   = 0.0f;
};

class OrbitCamera {
public:
    // `cityRadius` is the radius of the city footprint in metres, `cycleSeconds` the full run
    // length. Both set scale only; the shape of the motion is the same regardless.
    static OrbitCamera Create(uint64_t seed, ShotType shot, float cityRadius, float cycleSeconds);

    // `t` is seconds since the cycle began. Defined beyond cycleSeconds as well: the orbit simply
    // continues, so a cycle that overruns drifts rather than snapping.
    CameraState Evaluate(float t) const;

    // The framing solver of spec 11.1, with the constraints that exist so far.
    //
    // Scales the whole shot — radius and height together, so its character survives — until
    // `subject` sits inside the frame with `margin` to spare, for every t in [0, holdFraction] of
    // the cycle, and until the camera is outside the subject sphere for the *whole* cycle. The
    // smallest scale that satisfies both is chosen, because the failure worth avoiding here is a
    // city too small to see, not one too large.
    //
    // `aspect` defaults to 16:9, the narrowest screen this is expected to run on. Solving there
    // is conservative for 21:9, which only ever has more horizontal room — which is what spec
    // 11.1 means by working at both without a cut or a zoom. The world is generated once and
    // presented on every monitor, so this cannot be the actual window's aspect.
    void FrameOn(const Subject& subject, float aspect = 16.0f / 9.0f, float holdFraction = 0.30f,
                 float margin = 0.10f);

    // The same solve against several subjects, each only over the stretch of the cycle where spec
    // 11.1 requires it. The orbit ends up sized by whichever window binds — the cloud, in
    // practice, which is what spec 11.1 says it must be.
    void FrameOn(const FramingWindow* windows, int count, float aspect = 16.0f / 9.0f,
                 float margin = 0.10f);

    // Where the camera looks, at the start and end of the cycle. Set from outside because the
    // cloud is what the end of the cycle is about and the camera is built before it exists: a
    // look-at that stays at city height leaves the cloud in the top third and half the frame full
    // of empty desert.
    void SetTargetHeights(float start, float end) {
        targetHeightStart_ = start;
        targetHeightEnd_   = end;
    }

    ShotType shot() const { return shot_; }
    float    orbitRadius() const { return radiusMid_; }

    // How much of the frame the subject fills at `t`, as a fraction of the distance from the
    // centre to the edge, taking the worse of the two axes. 1 means it exactly touches an edge;
    // above 1 it is being cropped. Exposed so the selftest can assert the solver's result rather
    // than re-deriving it, and so the caller can log what it got.
    float FramingFill(const Subject& subject, float t, float aspect = 16.0f / 9.0f) const;

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
