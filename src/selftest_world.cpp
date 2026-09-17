// Selftests for world generation: the sky and the orbiting camera.
//
// Both are pure functions of a seed, which makes them checkable exactly. The rendering they drive
// is not — a shader can only be judged by looking at a frame — so these tests cover the part that
// decides what a cycle looks like, and the captures cover whether it was drawn correctly.

#include "core/math.h"
#include "selftest_check.h"
#include "world/camera.h"
#include "world/sky.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace selftest {

using namespace core;
using namespace world;

namespace {

float ElevationDegrees(const Vec3& dir) { return std::asin(Clamp(dir.y, -1.0f, 1.0f)) / kDegToRad; }

}  // namespace

void TestSky() {
    std::printf("sky and time of day (spec 5.4, 6.3)\n");

    // Sun elevation per setting, spec 5.4. Checked across many seeds because the elevation is
    // drawn from a range, and a range that drifts outside its bounds would only show on some runs.
    float morningLo = 1e9f, morningHi = -1e9f;
    float noonLo = 1e9f, noonHi = -1e9f;
    float twilightLo = 1e9f, twilightHi = -1e9f;

    bool  allNormalised = true;
    for (uint64_t s = 1; s <= 400; ++s) {
        const Sky morning  = MakeSky(TimeOfDay::Morning, s);
        const Sky noon     = MakeSky(TimeOfDay::Noon, s);
        const Sky twilight = MakeSky(TimeOfDay::Twilight, s);
        const Sky night    = MakeSky(TimeOfDay::Night, s);

        for (const Sky* sky : {&morning, &noon, &twilight, &night}) {
            if (std::fabs(Length(sky->keyDirection) - 1.0f) > 1e-3f) allNormalised = false;
        }

        const float m = ElevationDegrees(morning.keyDirection);
        const float n = ElevationDegrees(noon.keyDirection);
        const float t = ElevationDegrees(twilight.keyDirection);

        morningLo  = std::fmin(morningLo, m);
        morningHi  = std::fmax(morningHi, m);
        noonLo     = std::fmin(noonLo, n);
        noonHi     = std::fmax(noonHi, n);
        twilightLo = std::fmin(twilightLo, t);
        twilightHi = std::fmax(twilightHi, t);
    }

    Check(allNormalised, "the key direction is always normalised");
    Check(morningLo >= 11.9f && morningHi <= 20.1f, "morning sun sits at 12 to 20 degrees");
    Check(noonLo >= 69.9f && noonHi <= 85.1f, "noon sun sits at 70 to 85 degrees");
    // "2-6 degrees, below horizon at the low end" (spec 5.4), so the range must straddle zero.
    Check(twilightHi <= 6.1f, "twilight sun stays at or below 6 degrees");
    Check(twilightLo < 0.0f, "twilight sun reaches below the horizon");

    // The bearing must vary, or every cycle at one setting throws its shadows the same way.
    bool bearingVaries = false;
    const Sky a = MakeSky(TimeOfDay::Morning, 11);
    for (uint64_t s = 12; s <= 40 && !bearingVaries; ++s) {
        const Sky b = MakeSky(TimeOfDay::Morning, s);
        if (std::fabs(b.keyDirection.x - a.keyDirection.x) > 0.05f) bearingVaries = true;
    }
    Check(bearingVaries, "the sun bearing varies between cycles");

    const Sky morning  = MakeSky(TimeOfDay::Morning, 7);
    const Sky noon     = MakeSky(TimeOfDay::Noon, 7);
    const Sky twilight = MakeSky(TimeOfDay::Twilight, 7);
    const Sky night    = MakeSky(TimeOfDay::Night, 7);

    // Spec 5.4: "a time of day that only tints the sky is not done". Every setting must move the
    // light, the sky, the stars and the exposure together.
    Check(noon.keyIntensity > morning.keyIntensity, "noon is the strongest key light");
    Check(night.keyIntensity < twilight.keyIntensity, "night is the weakest key light");
    Check(night.starBrightness > twilight.starBrightness, "stars are brightest at night");
    Check(twilight.starBrightness > 0.0f, "stars are already emerging at twilight");
    Check(morning.starBrightness == 0.0f && noon.starBrightness == 0.0f,
          "no stars in daylight");
    Check(night.baseExposure > noon.baseExposure, "night is exposed differently, not just darker");

    // Window emission scales inversely with ambient light (spec 5.4).
    Check(night.windowEmission > twilight.windowEmission, "windows dominate at night");
    Check(twilight.windowEmission > morning.windowEmission, "windows fade as the sky brightens");
    Check(morning.windowEmission > noon.windowEmission, "windows are dimmest at noon");
    Check(noon.windowEmission > 0.0f, "window emission is subtle at noon but never absent");

    // The body is emissive well above 1 so bloom has something real to work with (spec 5.1).
    Check(noon.bodyColor.x > 100.0f, "the sun is emissive far above 1");
    Check(night.bodyColor.x > 10.0f, "the moon is bright enough to read as the light source");
    Check(night.bodyIsMoon, "night draws the moon");
    Check(!twilight.bodyIsMoon, "twilight draws the sun");

    // The body is drawn larger than life on purpose: at its true angular radius it would cover a
    // few pixels and could not read as the source of the scene's light.
    Check(noon.bodyAngularRadius > 0.01f, "the body is enlarged beyond its true angular radius");

    // Every colour reaching the renderer is linear scene-referred (spec 5.1). The palette is
    // authored in display terms, so a value that survived undecoded would land near its sRGB
    // figure; twilight's orange horizon at 0.95 sRGB decodes to about 0.89.
    Check(twilight.horizonColor.x > 0.80f && twilight.horizonColor.x < 0.95f,
          "the twilight horizon is decoded to linear, not left in sRGB");
    Check(twilight.horizonColor.x > twilight.horizonColor.z,
          "the twilight horizon is orange, not blue");
    Check(noon.zenithColor.z > noon.zenithColor.x, "the noon zenith is blue");

    Check(PickTimeOfDay(1) == PickTimeOfDay(1), "the random pick is stable for a seed");
}

void TestCamera() {
    std::printf("orbit camera (spec 11.1)\n");

    const float kCityRadius = 800.0f;
    const float kCycle      = 100.0f;

    const ShotType shots[] = {ShotType::DistantRidge, ShotType::LowApproach, ShotType::HighOblique,
                              ShotType::StreetLevel};

    for (ShotType shot : shots) {
        const OrbitCamera cam = OrbitCamera::Create(4242, shot, kCityRadius, kCycle);

        // 4 to 8 minutes per revolution (spec 11.1).
        Check(cam.revolutionSeconds() >= 240.0f && cam.revolutionSeconds() <= 480.0f,
              "a revolution takes 4 to 8 minutes");

        // "The camera is never still." Sampled densely across the whole cycle: every step must
        // move the eye, with no hold, no cut and no stop anywhere - including through the
        // countdown, which is where a naive implementation would be tempted to settle.
        CameraState prev      = cam.Evaluate(0.0f);
        float       minStep   = 1e9f;
        float       maxStep   = 0.0f;
        bool        finite    = true;
        for (int i = 1; i <= 1000; ++i) {
            const float       t     = static_cast<float>(i) * (kCycle / 1000.0f);
            const CameraState state = cam.Evaluate(t);

            const float step = Length(state.eye - prev.eye);
            minStep          = std::fmin(minStep, step);
            maxStep          = std::fmax(maxStep, step);

            if (std::isnan(state.eye.x) || std::isnan(state.eye.y) || std::isnan(state.eye.z) ||
                std::isnan(state.target.y) || std::isnan(state.fovY)) {
                finite = false;
            }
            prev = state;
        }

        Check(finite, "the camera never evaluates to NaN");
        Check(minStep > 0.0f, "the camera never stops (spec 11.1)");

        // ...and never lurches either. A jump would be a cut, which spec 11.1 also forbids.
        Check(maxStep < minStep * 25.0f, "the camera moves smoothly, without a cut");

        // The eye must stay outside the city and above the ground, or the shot is inside a
        // building looking at the inside of a building.
        bool outside = true, aboveGround = true;
        for (int i = 0; i <= 200; ++i) {
            const CameraState state = cam.Evaluate(static_cast<float>(i) * (kCycle / 200.0f));
            const float       ground = std::sqrt(state.eye.x * state.eye.x +
                                                 state.eye.z * state.eye.z);
            if (ground < kCityRadius) outside = false;
            if (state.eye.y < 1.0f) aboveGround = false;
        }
        Check(outside, "the camera stays outside the city footprint");
        Check(aboveGround, "the camera stays above the ground");
    }

    // Radius and height must vary independently, or the track is a flat circle with a zoom.
    const OrbitCamera cam    = OrbitCamera::Create(99, ShotType::DistantRidge, kCityRadius, kCycle);
    const CameraState start  = cam.Evaluate(0.0f);
    const CameraState middle = cam.Evaluate(kCycle * 0.5f);
    const CameraState end    = cam.Evaluate(kCycle);

    const float r0 = std::sqrt(start.eye.x * start.eye.x + start.eye.z * start.eye.z);
    const float r1 = std::sqrt(end.eye.x * end.eye.x + end.eye.z * end.eye.z);
    Check(std::fabs(r1 - r0) > 1.0f, "the orbit radius changes across the cycle");
    Check(std::fabs(end.eye.y - start.eye.y) > 1.0f, "the camera height changes across the cycle");

    // One cycle sweeps 30 to 90 degrees of arc, not a full turn (spec 11.1).
    const float bearing0 = std::atan2(start.eye.x, start.eye.z);
    const float bearing1 = std::atan2(end.eye.x, end.eye.z);
    float       swept    = std::fabs(bearing1 - bearing0);
    if (swept > kPi) swept = kTwoPi - swept;
    Check(swept > Radians(20.0f) && swept < Radians(100.0f),
          "a cycle sweeps a fraction of a revolution, not all of it");

    // Direction of travel is randomised per cycle (spec 11.1), so both must occur across seeds.
    bool sawClockwise = false, sawAnticlockwise = false;
    for (uint64_t s = 1; s <= 60; ++s) {
        const OrbitCamera c  = OrbitCamera::Create(s, ShotType::DistantRidge, kCityRadius, kCycle);
        const CameraState p0 = c.Evaluate(0.0f);
        const CameraState p1 = c.Evaluate(5.0f);
        // Cross product of the two radius vectors in the ground plane gives the sense of travel.
        const float cross = p0.eye.z * p1.eye.x - p0.eye.x * p1.eye.z;
        if (cross > 0.0f) sawClockwise = true;
        if (cross < 0.0f) sawAnticlockwise = true;
    }
    Check(sawClockwise && sawAnticlockwise, "the direction of travel is randomised per cycle");

    // The opening elevation of the low shots varies per cycle (spec 11.1), and the distribution is
    // the point rather than the range: most cycles stay near the desert floor, a few climb. A
    // uniform draw would turn "a low pass, occasionally higher" into "some height between nought
    // and thirty degrees", which is a different shot library.
    for (ShotType shot : {ShotType::LowApproach, ShotType::StreetLevel}) {
        int   low = 0, high = 0;
        float steepest = 0.0f;

        for (uint64_t s = 1; s <= 400; ++s) {
            const OrbitCamera c  = OrbitCamera::Create(s * 7919ull, shot, kCityRadius, kCycle);
            const CameraState p0 = c.Evaluate(0.0f);

            const float ground = std::sqrt(p0.eye.x * p0.eye.x + p0.eye.z * p0.eye.z);
            const float angle  = std::atan2(p0.eye.y, ground);

            if (angle < Radians(8.0f)) ++low;
            if (angle > Radians(14.0f)) ++high;
            steepest = std::fmax(steepest, angle);
        }

        Check(low > 200, "most cycles of a low shot open near the desert floor");
        Check(high > 5, "and a few of them open well above it");
        Check(steepest > Radians(20.0f) && steepest < Radians(34.0f),
              "the steepest opening is about thirty degrees up, and no more");
    }

    // Two cycles must not share a shot, and the same seed must reproduce one exactly.
    const OrbitCamera again = OrbitCamera::Create(99, ShotType::DistantRidge, kCityRadius, kCycle);
    const CameraState repeat = again.Evaluate(kCycle * 0.5f);
    Check(repeat.eye.x == middle.eye.x && repeat.eye.y == middle.eye.y,
          "the same seed reproduces the same shot");

    // Past the end of the cycle the orbit continues rather than snapping: the phase timing in M5
    // may overrun the nominal length, and a camera that jumped there would be a cut.
    const CameraState over = cam.Evaluate(kCycle * 1.2f);
    Check(Length(over.eye - end.eye) < Length(end.eye) * 0.5f,
          "the orbit continues past the nominal cycle length without snapping");
}

}  // namespace selftest
