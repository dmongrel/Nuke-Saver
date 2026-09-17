#include "world/world.h"

#include "app/log.h"
#include "app/settings.h"
#include "core/rng.h"

#include <cstdlib>

#include <windows.h>

namespace world {
namespace {

// A seed that differs between runs and between two savers started in the same second. The
// performance counter alone would do on any sane machine; mixing in the process id costs nothing
// and removes the one case where it would not.
// NUKE_SAVER_SEED pins the cycle, so a world that misbehaves can be generated again. Generation
// is deterministic by design (spec 6.1), and that is worth nothing for debugging unless there is
// a way to say which one.
bool SeedFromEnvironment(uint64_t* out) {
    const char* value = std::getenv("NUKE_SAVER_SEED");
    if (!value || !*value) return false;

    *out = std::strtoull(value, nullptr, 10);
    return *out != 0;
}

uint64_t SeedFromClock() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return core::SplitMix64(static_cast<uint64_t>(now.QuadPart) ^
                            (static_cast<uint64_t>(GetCurrentProcessId()) << 32));
}

TimeOfDay Resolve(app::TimeOfDay setting, uint64_t seed) {
    switch (setting) {
        case app::TimeOfDay::Morning:  return TimeOfDay::Morning;
        case app::TimeOfDay::Noon:     return TimeOfDay::Noon;
        case app::TimeOfDay::Twilight: return TimeOfDay::Twilight;
        case app::TimeOfDay::Night:    return TimeOfDay::Night;
        case app::TimeOfDay::Random:   break;
    }
    return PickTimeOfDay(core::SplitMix64(seed ^ 0x7DAull));
}

ShotType Resolve(app::CameraMode setting, uint64_t seed) {
    switch (setting) {
        case app::CameraMode::DistantRidge: return ShotType::DistantRidge;
        case app::CameraMode::LowApproach:  return ShotType::LowApproach;
        case app::CameraMode::HighOblique:  return ShotType::HighOblique;
        case app::CameraMode::StreetLevel:  return ShotType::StreetLevel;
        case app::CameraMode::Random:       break;
    }
    return PickShot(core::SplitMix64(seed ^ 0xCA3ull));
}

// The highest the camera gets over a cycle. Sampled rather than derived, because the height curve
// is the product of two eased schedules and its maximum is not necessarily at either end.
float HighestCameraPoint(const OrbitCamera& camera, float cycleSeconds) {
    float highest = 0.0f;
    for (int i = 0; i <= 200; ++i) {
        const float t = cycleSeconds * static_cast<float>(i) / 200.0f;
        highest       = std::fmax(highest, camera.Evaluate(t).eye.y);
    }
    return highest;
}

}  // namespace

World Generate(const app::Settings& settings, uint64_t seed) {
    World world;
    uint64_t pinned = 0;
    world.seed      = seed ? seed : (SeedFromEnvironment(&pinned) ? pinned : SeedFromClock());
    world.cycleSeconds = kNominalCycleSeconds;

    // Extent 1.2 to 2 km across (spec 6.4), so a radius of 600 to 1000 m.
    world.cityRadius = core::Rng(world.seed).Fork(0xC17E).Range(600.0f, 1000.0f);

    const TimeOfDay timeOfDay = Resolve(settings.timeOfDay, world.seed);
    const ShotType  shot      = Resolve(settings.camera, world.seed);

    world.sky    = MakeSky(timeOfDay, world.seed);
    world.camera = OrbitCamera::Create(world.seed, shot, world.cityRadius, world.cycleSeconds);

    world.terrain.seed       = world.seed;
    world.terrain.cityRadius = world.cityRadius;
    world.terrainMesh        = BuildTerrain(world.terrain);

    // The range is sized against the shot that actually happens. A peak shorter than the camera
    // cannot rise above the horizon line however wide it is, so the minimum height has to know
    // how high this cycle's camera gets - which is why this runs after the camera is built.
    world.horizon = SizeHorizon(world.seed, HighestCameraPoint(world.camera, world.cycleSeconds),
                                world.camera.orbitRadius());

    // Spec 6.3 requires the ring to close the horizon from every orbit point at every shot
    // height. GenerateClosedRange checks it and keeps widening until it holds, rather than
    // trusting the parameters - which is the difference between a requirement and a hope: a gap
    // shows on one bearing in one cycle out of many, exactly the kind of defect that ships.
    float                   worst = 0.0f;
    const std::vector<Peak> peaks = GenerateClosedRange(&world.horizon, &worst);

    world.horizonMesh = BuildHorizon(peaks, world.seed);

    app::Log("world: seed=%llu %s %s cityRadius=%.0fm revolution=%.0fs",
             static_cast<unsigned long long>(world.seed), TimeOfDayName(timeOfDay),
             ShotName(shot), world.cityRadius, world.camera.revolutionSeconds());
    app::Log("world: terrain %zu verts / %zu tris, horizon %zu peaks %.0f-%.0fm, margin %.2f deg",
             world.terrainMesh.vertices.size(), world.terrainMesh.indices.size() / 3, peaks.size(),
             world.horizon.minHeight, world.horizon.maxHeight, worst / core::kDegToRad);

    return world;
}

}  // namespace world
