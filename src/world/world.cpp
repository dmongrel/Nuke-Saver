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

// Spec 7.1's framing constraint, as a distance from the impact point rather than a time: the
// missile must be on screen for the last stretch of its approach.
//
// 0.85 city radii, which on a typical run is where the missile is about two seconds out — the
// figure spec 7.1 states. Stated as a distance because the run length is not settled when the
// solver runs, and taken at the low end because the approach is now steep: at 40 degrees the
// missile is 700 m up while it is still 850 m out, and framing it from any further back made it
// the binding constraint on the orbit instead of the cloud, pushing the camera 30% further out
// and shrinking the city through the whole first half of the cycle for the sake of three seconds.
constexpr float kMissileFramedWithin = 0.85f;

// How far off the centre of the frame the entry point may sit, as a fraction of the camera's
// horizontal half field of view. Not 1.0: an entry point exactly on the edge is a missile that
// enters by being clipped.
constexpr float kMissileOnAxis = 0.80f;

// And how far up it may sit, as a fraction of the vertical field of view measured from where the
// camera is already looking. Half the field is the top edge exactly; this is just under it.
constexpr float kMissileFrameTop = 0.44f;

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

        // Spec 7.1 makes this a framing requirement rather than a hope: the arrival MUST be on
        // screen. Sized from a distance along the path rather than from a clock time, because the
        // run length is not settled yet — AimOverRidge stretches it below, once the range behind
        // the camera exists. A distance is the honest statement of the requirement anyway: what
        // has to be in frame is the last stretch of the approach, and how many seconds that takes
        // is a property of the run, not of the shot.
        // Sized from the bounds of the descent angle rather than from the angle itself, which
        // is not solved until the aiming below: a shallow approach is the widest arrival and a
        // steep one is the tallest, so taking the width of the first and the height of the second
        // frames whichever the solver settles on.
        const float missileFrom = world.cityRadius * kMissileFramedWithin;
        Subject     missile;
        missile.radius = missileFrom * std::cos(Detonation::kDescentMin);
        missile.height = world.detonation.center.y + missileFrom * std::sin(Detonation::kDescentMax);

        // The window is still a pair of times, because the camera moves. It opens at the latest
        // moment the missile can still be that far out, which is the whole phase: a wider window
        // asks the solver to satisfy the constraint from more camera positions, so erring wide is
        // erring safe.
        const float missileVisibleFrom = world.timeline.Start(Phase::Missile);

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

    // One height field rather than 216 overlapping solids: see the Shell comment in horizon.h for
    // why the range is built this way.
    const Shell shell = BuildShell(peaks, world.horizon, world.seed);
    world.horizonMesh = BuildHorizon(shell, world.seed);

    // Spec 7.1: the missile comes in over the mountains, and is seen doing it. Both halves are
    // statements about the entry point's elevation as seen from the camera — one measured against
    // the range behind it, one against the top of the frame — so this is the last thing settled,
    // after the camera and the range both exist. Solving the descent angle leaves the bearing and
    // the run alone, and so leaves the arrival where the framing above was solved for it.
    {
        const CameraState view = world.camera.Evaluate(world.timeline.Start(Phase::Missile));

        // The top of the frame, in the same terms: how far the camera is already looking up or
        // down, plus most of half its vertical field of view. Not all of it: an entry point on
        // the edge of the frame is a missile that enters by being clipped, and the sliver held
        // back leaves a little sky above it to have come out of. Only a sliver, though -- every
        // degree reserved here is a degree of the gap between the summits and the top of the
        // frame, and on the shots that look down into the basin that gap is already thin.
        const core::Vec3 toTarget = view.target - view.eye;
        const core::Vec3 forward  = core::Normalize(toTarget);
        const float      pitch    = std::atan2(
            toTarget.y, std::fmax(std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z),
                                       1.0f));
        const float ceiling = pitch + view.fovY * kMissileFrameTop;

        // And the sides of the frame. The aspect ratio belongs to a monitor this cycle has not met
        // yet, so the narrowest one likely to run it is assumed: on anything wider there is more
        // room than this, never less.
        const float halfWide =
            std::atan(std::tan(view.fovY * 0.5f) * 16.0f / 9.0f) * kMissileOnAxis;

        // Two degrees of sky between the missile and the summits. Below about one the entry sits
        // on the ridgeline and reads as having come *off* the mountains rather than over them.
        //
        // The bearing is drawn at random (spec 7.1) and a random bearing is sometimes the one the
        // camera has its back to, where no descent angle can put the entry both over the range and
        // on screen. So the bearing is walked outward from the draw, two degrees at a time, until
        // one works -- keeping as much of the random direction as the shot allows rather than
        // replacing it with a fixed one. The ridge has to be re-measured at each candidate,
        // because the silhouette is different in every direction, which is why this loop lives
        // here with the peaks rather than inside Detonation.
        const float drawn = world.detonation.missileBearing;

        // Two tiers, because the two requirements are not equally negotiable. Being on screen is
        // the harder one -- a missile entering behind the viewer is not an entrance at all -- so a
        // bearing that is in shot but level with the peaks beats one that clears them off the side
        // of the frame. Both tiers still prefer the drawn direction: the walk goes outward from it
        // and stops at the first bearing that answers, so a cycle gives up only as much of its
        // random approach as the shot makes it.
        float bestBoth = core::kPi * 4.0f;  // the nearest bearing that is over the range and in shot
        float bestSeen = core::kPi * 4.0f;  // the nearest that is merely in shot

        for (int step = 0; step < 90; ++step) {
            for (int side = 0; side < 2; ++side) {
                const float bearing =
                    drawn + core::Radians(2.0f) * static_cast<float>(side ? step : -step);

                world.detonation.SetMissileBearing(bearing);
                const float here = SilhouetteElevation(peaks, view.eye, bearing);
                const float got =
                    world.detonation.AimApproach(view.eye, here, core::Radians(2.0f), ceiling);

                if (world.detonation.MissileEntryOffAxis(view.eye, forward) <= halfWide) {
                    if (bestSeen > core::kPi * 3.0f) bestSeen = bearing;
                    if (got > here && bestBoth > core::kPi * 3.0f) bestBoth = bearing;
                }
                if (step == 0) break;  // both sides are the same bearing at zero
            }
            if (bestBoth < core::kPi * 3.0f) break;
        }

        const float chosen = bestBoth < core::kPi * 3.0f
                                 ? bestBoth
                                 : (bestSeen < core::kPi * 3.0f ? bestSeen : drawn);

        world.detonation.SetMissileBearing(chosen);
        const float bestRidge = SilhouetteElevation(peaks, view.eye, chosen);
        const float best =
            world.detonation.AimApproach(view.eye, bestRidge, core::Radians(2.0f), ceiling);

        app::Log("missile: bearing %.0f deg (drawn %.0f), run %.0fm, descent %.0f deg, entry "
                 "%.0fm up at %.1f deg, %.1f off axis (ridge %.1f, frame top %.1f)",
                 world.detonation.missileBearing / core::kDegToRad, drawn / core::kDegToRad,
                 world.detonation.missileRun,
                 world.detonation.missileDescent / core::kDegToRad,
                 world.detonation.missileStart.y, best / core::kDegToRad,
                 world.detonation.MissileEntryOffAxis(view.eye, forward) / core::kDegToRad,
                 bestRidge / core::kDegToRad, ceiling / core::kDegToRad);
    }

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
    app::Log("world: terrain %zu verts / %zu tris, horizon %zu peaks %.0f-%.0fm on a %dx%d field, "
             "margin %.2f deg",
             world.terrainMesh.vertices.size(), world.terrainMesh.indices.size() / 3, peaks.size(),
             world.horizon.minHeight, world.horizon.maxHeight, shell.bearings, shell.rings,
             worst / core::kDegToRad);

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
