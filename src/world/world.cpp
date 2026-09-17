#include "world/world.h"

#include "app/log.h"
#include "app/settings.h"
#include "core/rng.h"

#include <windows.h>

namespace world {
namespace {

// A seed that differs between runs and between two savers started in the same second. The
// performance counter alone would do on any sane machine; mixing in the process id costs nothing
// and removes the one case where it would not.
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

}  // namespace

World Generate(const app::Settings& settings, uint64_t seed) {
    World world;
    world.seed         = seed ? seed : SeedFromClock();
    world.cycleSeconds = kNominalCycleSeconds;

    // Extent 1.2 to 2 km across (spec 6.4), so a radius of 600 to 1000 m.
    world.cityRadius = core::Rng(world.seed).Fork(0xC17E).Range(600.0f, 1000.0f);

    const TimeOfDay timeOfDay = Resolve(settings.timeOfDay, world.seed);
    const ShotType  shot      = Resolve(settings.camera, world.seed);

    world.sky    = MakeSky(timeOfDay, world.seed);
    world.camera = OrbitCamera::Create(world.seed, shot, world.cityRadius, world.cycleSeconds);

    app::Log("world: seed=%llu %s %s cityRadius=%.0fm revolution=%.0fs",
             static_cast<unsigned long long>(world.seed), TimeOfDayName(timeOfDay),
             ShotName(shot), world.cityRadius, world.camera.revolutionSeconds());

    return world;
}

}  // namespace world
