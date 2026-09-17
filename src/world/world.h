// One cycle's world (spec 6.1).
//
// Everything the scene is made of is generated from a single 64-bit seed at cycle start. Nothing
// is loaded: the `.scr` gets copied into System32 by itself, so anything it cannot carry inside
// its own resource section does not exist.
//
// This is the object that decides what a run looks like. It is created once per cycle, read by
// the renderer every frame, and never mutated by rendering — which is what lets a second monitor
// present the same world from its own aspect ratio without a second generation pass.
#ifndef NUKE_SAVER_WORLD_WORLD_H
#define NUKE_SAVER_WORLD_WORLD_H

#include "world/camera.h"
#include "world/sky.h"

#include <cstdint>

namespace app {
struct Settings;
}

namespace world {

// Nominal cycle length. Spec 4 puts the full eleven-phase run at 80 to 115 seconds; the phase
// state machine that fixes the exact figure per cycle arrives with M5, and until then this is
// what the camera paces itself against.
constexpr float kNominalCycleSeconds = 100.0f;

struct World {
    uint64_t seed = 0;

    Sky         sky;
    OrbitCamera camera;

    // Radius of the city footprint in metres. Spec 6.4 puts the extent at 1.2 to 2 km across.
    // The generator in M3b will set this from the lot layout; until then it is drawn directly,
    // because the camera has to be sized against something and the range is what matters.
    float cityRadius = 800.0f;

    float cycleSeconds = kNominalCycleSeconds;

    CameraState CameraAt(float t) const { return camera.Evaluate(t); }
};

// Generates a world. `seed` of 0 means draw one from the clock, so two consecutive cycles never
// share a skyline (spec 6.4).
World Generate(const app::Settings& settings, uint64_t seed = 0);

}  // namespace world

#endif
