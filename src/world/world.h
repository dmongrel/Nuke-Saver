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

#include "world/board.h"
#include "world/camera.h"
#include "world/city.h"
#include "world/detonation.h"
#include "world/horizon.h"
#include "world/mesh.h"
#include "world/missile.h"
#include "world/phase.h"
#include "world/sky.h"
#include "world/terrain.h"

#include <cstdint>

namespace app {
struct Settings;
}

namespace world {

// Nominal cycle length, used only where a length is needed before the timeline exists. The real
// figure is drawn per cycle by Timeline::Create and lands in World::cycleSeconds.
constexpr float kNominalCycleSeconds = 100.0f;

struct World {
    uint64_t seed = 0;

    Sky         sky;
    OrbitCamera camera;

    // Radius of the city footprint in metres. Spec 6.4 puts the extent at 1.2 to 2 km across.
    // Drawn before the layout, because the camera and the horizon range are both sized against
    // it; the generator then reports the extent its lots actually reached, and this is updated to
    // that, so the framing of spec 11.1 sees the city that exists rather than the one asked for.
    float cityRadius = 800.0f;

    float cycleSeconds = kNominalCycleSeconds;

    TerrainParams terrain;
    HorizonParams horizon;

    // Static geometry, generated once. Held on the CPU only until the renderer uploads it; the
    // world is the source of truth for shape, the GPU buffers are a copy.
    Mesh terrainMesh;
    Mesh horizonMesh;

    // The missile of spec 7.1. A mesh rather than instances, and the only one that moves; where
    // it is at a given moment comes from the detonation, not from here.
    Mesh missileMesh;

    // Instances, not geometry: every building is the same unit cube (spec 6.4), and so is every
    // piece of the countdown board (spec 7.6).
    City  city;
    Board board;

    // The run cycle of spec 4. Drawn once, then a pure function of elapsed seconds.
    Timeline timeline;

    // The blast and the cloud it leaves (spec 7.2, 7.5), as dimensions rather than as geometry.
    Detonation detonation;

    CameraState CameraAt(float t) const { return camera.Evaluate(t); }

    // Seconds into the growth phase at cycle time `t`. Negative before it starts, which is what
    // keeps phase 0's desert empty without the shader knowing anything about phases.
    float GrowthTime(float t) const { return t - timeline.Start(Phase::Growth); }

    // How far the board has risen at `t`, 0 to 1, eased. It goes up last (spec 6.5), after the
    // final building, so the eye is left on it going into phase 2.
    float BoardRise(float t) const;

    // Forwarded so that nothing outside has to hold both the timeline and the detonation to ask
    // the two questions every pass after phase 5 needs answered.
    float ShellRadius(float t) const { return detonation.ShellRadius(timeline, t); }
    float CloudGrow(float t) const { return detonation.CloudGrow(timeline, t); }
    float FlashIntensity(float t) const { return detonation.FlashIntensity(timeline, t); }
    float FireRadius(float t) const { return detonation.FireRadius(timeline, t); }
    core::Vec3 FireCenter(float t) const { return detonation.FireCenter(timeline, t); }
    core::Vec3 FireColor(float t) const { return detonation.FireColor(timeline, t); }

    bool MissileVisible(float t) const { return detonation.MissileVisible(timeline, t); }
    core::Mat4 MissileTransform(float t) const {
        return world::MissileTransform(detonation.MissileAt(timeline, t),
                                       detonation.MissileDirection());
    }
};

// Generates a world. `seed` of 0 means draw one from the clock, so two consecutive cycles never
// share a skyline (spec 6.4).
World Generate(const app::Settings& settings, uint64_t seed = 0);

}  // namespace world

#endif
