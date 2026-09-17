#include "world/world.h"

#include "app/log.h"
#include "app/settings.h"
#include "core/rng.h"

#include <cstdlib>
#include <cmath>
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
    world.timeline     = Timeline::Create(world.seed);
    world.cycleSeconds = world.timeline.total();

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

    world.detonation = Detonation::Create(world.seed, world.cityRadius, world.city.tallest);
    world.missileMesh =
        BuildMissileMesh(world.seed, world.detonation.missileLength, world.detonation.missileRadius);

    world.terrain.seed       = world.seed;
    world.terrain.cityRadius = world.cityRadius;
    world.terrainMesh        = BuildTerrain(world.terrain);

    world.camera = OrbitCamera::Create(world.seed, shot, world.cityRadius, world.cycleSeconds);

    // Spec 11.1's framing solver. Four of its five constraints exist now, and they do not apply
    // at the same time: the city must fit while it is being built, the missile must be in frame
    // for the last two seconds of its run (spec 7.1), the cloud must fit at its widest in phase 8,
    // and the debris field must fit in phase 10. The cloud binds the orbit radius, as spec 11.1
    // says it must.
    {
        Subject city;
        city.radius = world.cityRadius;
        city.height = world.city.tallest;

        Subject cloud;
        cloud.radius = world.detonation.capRadius + world.detonation.capTube;
        cloud.height = world.detonation.capHeight + world.detonation.capTube;

        // Phase 7 throws the city well outside its own footprint, and what is left lies flat.
        Subject debris;
        debris.radius = world.cityRadius * 1.35f;
        debris.height = 40.0f;

        // Spec 7.1 makes this a framing requirement rather than a hope: the missile MUST be on
        // screen for the last two seconds of its run. Sized from where it actually is when those
        // two seconds begin, so the constraint is the real one and not a guess at a bearing.
        const float missileVisibleFrom = world.timeline.End(Phase::Missile) - 2.0f;
        const core::Vec3 missileAt =
            world.detonation.MissileAt(world.timeline, missileVisibleFrom);
        Subject missile;
        missile.radius = std::sqrt(missileAt.x * missileAt.x + missileAt.z * missileAt.z);
        missile.height = missileAt.y;

        const FramingWindow windows[4] = {
            {city, world.timeline.Start(Phase::Growth), world.timeline.End(Phase::Settle)},
            {missile, missileVisibleFrom, world.timeline.End(Phase::Missile)},
            {cloud, world.timeline.Start(Phase::Gather), world.timeline.End(Phase::Disperse)},
            {debris, world.timeline.Start(Phase::Fade), world.timeline.End(Phase::Fade)},
        };

        // And the look-at rises across the cycle from the skyline to the middle of the cloud.
        // Spec 11.1 asks for the look-at to vary slowly over the cycle; this is that variation
        // aimed at something, rather than at a fraction of the city's radius chosen blind.
        world.camera.SetTargetHeights(world.city.tallest * 0.55f, world.detonation.capHeight * 0.5f);

        world.camera.FrameOn(windows, 4);
        app::Log("camera: orbit %.0fm, fill %.2f city / %.2f missile / %.2f cloud / %.2f debris",
                 world.camera.orbitRadius(), world.camera.FramingFill(city, 0.0f),
                 world.camera.FramingFill(missile, missileVisibleFrom),
                 world.camera.FramingFill(cloud, world.timeline.Start(Phase::Disperse)),
                 world.camera.FramingFill(debris, world.timeline.Start(Phase::Fade)));
    }

    // The board comes after the camera, which is the opposite of everything else here. It is
    // placed and turned relative to where the camera will be standing when the countdown runs
    // (spec 7.6): one face, off to one side of the city, square to the viewer somewhere in the
    // five seconds that matter. That cannot be decided before the shot has been solved, because
    // solving the shot is what moves the camera.
    {
        const CameraState view = world.camera.Evaluate(world.timeline.Start(Phase::Countdown) + 2.5f);

        // The camera's right hand, from its own basis rather than from its bearing: the look-at
        // drifts (spec 11.1), so "right" is not simply a quarter turn round the orbit.
        const core::Vec3 forward = core::Normalize(view.target - view.eye);
        const core::Vec3 right   = core::Normalize(core::Cross(forward, view.up));

        world.board = GenerateBoard(world.seed, world.city, view.eye, right);
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
    app::Log("detonation: shell reaches %.0fm, cloud stem %.0fm cap %.0fm r%.0fm, wind %.0fm/s",
             world.detonation.reach, world.detonation.stemHeight, world.detonation.capHeight,
             world.detonation.capRadius,
             std::sqrt(world.detonation.wind.x * world.detonation.wind.x +
                       world.detonation.wind.z * world.detonation.wind.z));
    app::Log("world: terrain %zu verts / %zu tris, horizon %zu peaks %.0f-%.0fm, margin %.2f deg",
             world.terrainMesh.vertices.size(), world.terrainMesh.indices.size() / 3, peaks.size(),
             world.horizon.minHeight, world.horizon.maxHeight, worst / core::kDegToRad);

    return world;
}

float World::BoardRise(float t) const {
    const float start = timeline.Start(Phase::Growth) + board.riseStart;
    if (board.riseDuration <= 0.0f) return t >= start ? 1.0f : 0.0f;

    // The same ease-out the buildings use (spec 6.5): monotonic, reaches exactly one, no bounce.
    const float u = core::Saturate((t - start) / board.riseDuration);
    return core::EaseOutCubic(u);
}

}  // namespace world
