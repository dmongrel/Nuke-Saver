#include "world/world.h"

#include "app/log.h"
#include "app/settings.h"
#include "core/rng.h"

#include <cstdlib>
#include <cstring>

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

// The same idea as NUKE_SAVER_SEED, for the two settings a cycle is otherwise free to draw for
// itself. Without these, checking a change against all four times of day means editing the
// registry between runs, and the preview stills of spec 13.3 have no way to ask for one.
//
// Environment only. These are not settings: spec section 10 owns what the user can choose, and
// adding a hidden fifth option to that list would be a different thing from a way to reproduce a
// frame.
bool NameFromEnvironment(const char* variable, const char* const* names, int count, int* out) {
    const char* value = std::getenv(variable);
    if (!value || !*value) return false;

    for (int i = 0; i < count; ++i) {
        if (_stricmp(value, names[i]) == 0) {
            *out = i;
            return true;
        }
    }

    app::Log("%s: '%s' is not a recognised value, ignoring", variable, value);
    return false;
}

TimeOfDay Resolve(app::TimeOfDay setting, uint64_t seed) {
    static const char* const kNames[] = {"morning", "noon", "twilight", "night"};
    int                      picked   = 0;
    if (NameFromEnvironment("NUKE_SAVER_TIME", kNames, 4, &picked)) {
        return static_cast<TimeOfDay>(picked);
    }

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
    static const char* const kNames[] = {"distant-ridge", "low-approach", "high-oblique",
                                         "street-level"};
    int                      picked   = 0;
    if (NameFromEnvironment("NUKE_SAVER_SHOT", kNames, 4, &picked)) {
        return static_cast<ShotType>(picked);
    }

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

    world.sky = MakeSky(timeOfDay, world.seed);

    // The city comes before the camera, and the camera before the horizon range. Each one is a
    // constraint on the next: the lots decide the real extent, the extent decides where the
    // camera has to stand to frame it, and how high the camera gets decides how tall the range
    // has to be to close the horizon behind it.
    {
        core::Rng  cityRng = core::Rng(world.seed).Fork(0xC17E5Full);
        CityParams params;
        params.seed      = world.seed;
        params.radius    = world.cityRadius;
        params.gridAngle = cityRng.Range(0.0f, core::kPi * 0.5f);
        params.blockSize = cityRng.Range(82.0f, 112.0f);
        params.roadWidth = cityRng.Range(14.0f, 22.0f);
        params.arterials = 1 + static_cast<int>(cityRng.Index(3));
        params.growthSeconds = cityRng.Range(5.0f, 10.0f);  // spec 6.5

        world.city       = GenerateCity(params);
        world.cityRadius = world.city.params.radius;
    }

    world.terrain.seed       = world.seed;
    world.terrain.cityRadius = world.cityRadius;
    world.terrainMesh        = BuildTerrain(world.terrain);

    world.camera = OrbitCamera::Create(world.seed, shot, world.cityRadius, world.cycleSeconds);

    // Spec 11.1's framing solver, with the one subject that exists at this milestone. The cloud
    // of phase 8 is the binding constraint and arrives with M4; until it does, framing on the
    // city is the same solve with one fewer thing to satisfy.
    {
        Subject city;
        city.radius = world.cityRadius;
        city.height = world.city.tallest;

        world.camera.FrameOn(city);
        app::Log("camera: framed on city r=%.0fm h=%.0fm -> orbit %.0fm, fill %.2f at t=0",
                 city.radius, city.height, world.camera.orbitRadius(),
                 world.camera.FramingFill(city, 0.0f));
    }

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
    app::Log("world: city %zu buildings, tallest %.0fm, grown by %.1fs",
             world.city.buildings.size(), world.city.tallest, world.city.growthEnds);
    app::Log("world: terrain %zu verts / %zu tris, horizon %zu peaks %.0f-%.0fm, margin %.2f deg",
             world.terrainMesh.vertices.size(), world.terrainMesh.indices.size() / 3, peaks.size(),
             world.horizon.minHeight, world.horizon.maxHeight, worst / core::kDegToRad);

    return world;
}

}  // namespace world
