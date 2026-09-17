#include "world/sky.h"

#include "core/color.h"
#include "core/rng.h"

namespace world {
namespace {

using core::Vec3;

// Approximate blackbody chromaticity, normalised so the brightest channel is 1. The spec gives
// the key light in kelvin (5.4), and a lookup that only tinted the sky would miss the point:
// these values drive the light itself, so morning and twilight warm the buildings too.
Vec3 KelvinToLinearRgb(float kelvin) {
    // Piecewise fit over 1,500-10,000 K. Adequate for four fixed settings; nothing here needs
    // colorimetric accuracy, it needs the sun to look like the sun.
    const float t = core::Clamp(kelvin, 1500.0f, 10000.0f) / 1000.0f;

    float r, g, b;
    if (t <= 6.6f) {
        r = 1.0f;
        g = core::Saturate(-0.0155f * t * t + 0.28f * t - 0.06f);
        b = t <= 1.9f ? 0.0f : core::Saturate(0.54f * std::log(t - 1.9f) + 0.52f);
    } else {
        r = core::Saturate(1.35f * std::pow(t - 5.5f, -0.13f));
        g = core::Saturate(1.13f * std::pow(t - 5.4f, -0.08f));
        b = 1.0f;
    }

    // These are display-referred primaries; decode so the result is linear like everything else.
    return core::SrgbToLinear(Vec3{r, g, b});
}

// Builds a direction from elevation above the horizon and a compass bearing. Negative elevation
// is legitimate: twilight puts the sun at or just below the horizon (spec 5.4).
Vec3 DirectionFrom(float elevationDegrees, float bearingDegrees) {
    const float el = core::Radians(elevationDegrees);
    const float az = core::Radians(bearingDegrees);
    return core::Normalize(
        Vec3{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)});
}

}  // namespace

TimeOfDay PickTimeOfDay(uint64_t seed) {
    switch (core::Rng(seed).Index(4)) {
        case 0:  return TimeOfDay::Morning;
        case 1:  return TimeOfDay::Noon;
        case 2:  return TimeOfDay::Twilight;
        default: return TimeOfDay::Night;
    }
}

const char* TimeOfDayName(TimeOfDay t) {
    switch (t) {
        case TimeOfDay::Morning:  return "morning";
        case TimeOfDay::Noon:     return "noon";
        case TimeOfDay::Twilight: return "twilight";
        case TimeOfDay::Night:    return "night";
    }
    return "?";
}

Sky MakeSky(TimeOfDay timeOfDay, uint64_t seed) {
    core::Rng rng = core::Rng(seed).Fork(0x5C1);

    Sky sky;
    sky.timeOfDay = timeOfDay;

    // Compass bearing is free in every setting: it decides which way the long shadows run, and
    // holding it fixed would make every morning cycle look like the last one.
    const float bearing = rng.Range(0.0f, 360.0f);

    // The body is drawn far larger than life. At 0.0047 rad - the true angular radius of both
    // sun and moon - it covers about four pixels at this resolution, which cannot read as the
    // source of the scene's light. Film enlarges it for the same reason.
    switch (timeOfDay) {
        case TimeOfDay::Morning: {
            const float elevation = rng.Range(12.0f, 20.0f);  // spec 5.4
            sky.keyDirection      = DirectionFrom(elevation, bearing);
            sky.keyColor          = KelvinToLinearRgb(4500.0f);
            sky.keyIntensity      = 3.2f;

            sky.zenithColor  = core::SrgbToLinear(Vec3{0.38f, 0.55f, 0.82f});
            sky.horizonColor = core::SrgbToLinear(Vec3{0.85f, 0.70f, 0.52f});
            sky.groundColor  = core::SrgbToLinear(Vec3{0.20f, 0.16f, 0.13f});

            sky.ambientColor  = core::SrgbToLinear(Vec3{0.34f, 0.42f, 0.55f});
            sky.ambientScale  = 0.45f;
            sky.starBrightness = 0.0f;

            sky.bodyAngularRadius = 0.030f;
            sky.bodyColor         = KelvinToLinearRgb(4500.0f) * 600.0f;
            sky.baseExposure      = 1.0f;
            sky.windowEmission    = 0.35f;
            break;
        }

        case TimeOfDay::Noon: {
            const float elevation = rng.Range(70.0f, 85.0f);
            sky.keyDirection      = DirectionFrom(elevation, bearing);
            sky.keyColor          = KelvinToLinearRgb(6500.0f);
            sky.keyIntensity      = 5.0f;  // highest contrast on building faces

            sky.zenithColor  = core::SrgbToLinear(Vec3{0.22f, 0.42f, 0.80f});
            sky.horizonColor = core::SrgbToLinear(Vec3{0.72f, 0.80f, 0.88f});
            sky.groundColor  = core::SrgbToLinear(Vec3{0.26f, 0.22f, 0.18f});

            sky.ambientColor   = core::SrgbToLinear(Vec3{0.45f, 0.55f, 0.72f});
            sky.ambientScale   = 0.60f;
            sky.starBrightness = 0.0f;

            sky.bodyAngularRadius = 0.022f;
            sky.bodyColor         = KelvinToLinearRgb(6500.0f) * 1200.0f;
            sky.baseExposure      = 0.75f;
            // Barely visible at noon (spec 5.4), but never absent.
            sky.windowEmission = 0.10f;
            break;
        }

        case TimeOfDay::Twilight: {
            // 2-6 degrees, "below horizon at the low end" (spec 5.4). Sampled across zero so
            // some cycles get the sun just clear of the ridge and others get only its glow.
            const float elevation = rng.Range(-2.0f, 6.0f);
            sky.keyDirection      = DirectionFrom(elevation, bearing);
            sky.keyColor          = KelvinToLinearRgb(2200.0f);
            sky.keyIntensity      = 2.4f;

            sky.zenithColor  = core::SrgbToLinear(Vec3{0.16f, 0.13f, 0.34f});  // violet
            sky.horizonColor = core::SrgbToLinear(Vec3{0.95f, 0.42f, 0.16f});  // orange band
            sky.groundColor  = core::SrgbToLinear(Vec3{0.10f, 0.07f, 0.07f});

            sky.ambientColor   = core::SrgbToLinear(Vec3{0.30f, 0.22f, 0.32f});
            sky.ambientScale   = 0.28f;
            sky.starBrightness = 0.45f;  // emerging, not yet full night

            sky.bodyAngularRadius = 0.034f;
            sky.bodyColor         = KelvinToLinearRgb(2200.0f) * 400.0f;
            sky.baseExposure      = 1.35f;
            sky.windowEmission    = 0.75f;
            break;
        }

        case TimeOfDay::Night: {
            const float elevation = rng.Range(25.0f, 60.0f);
            sky.keyDirection      = DirectionFrom(elevation, bearing);
            sky.keyColor          = KelvinToLinearRgb(7500.0f);
            // Dim, but it must still read as the source. 0.35 put the desert floor at a mid
            // tone: the base exposure of 3.0 below is there to lift the city's own windows and
            // the stars, and it lifts the moonlit ground with them.
            sky.keyIntensity = 0.16f;

            sky.zenithColor  = core::SrgbToLinear(Vec3{0.015f, 0.020f, 0.045f});
            sky.horizonColor = core::SrgbToLinear(Vec3{0.05f, 0.06f, 0.10f});
            sky.groundColor  = core::SrgbToLinear(Vec3{0.02f, 0.02f, 0.03f});

            sky.ambientColor   = core::SrgbToLinear(Vec3{0.10f, 0.13f, 0.22f});
            sky.ambientScale   = 0.06f;
            sky.starBrightness = 1.0f;

            sky.bodyAngularRadius = 0.026f;
            // The moon is the key light and must read as such, so it is bright in absolute terms
            // even though the light it casts is dim.
            sky.bodyColor      = KelvinToLinearRgb(7500.0f) * 90.0f;
            sky.bodyIsMoon     = true;
            sky.baseExposure   = 3.0f;
            // At night the city is lit by its own windows and the board (spec 5.4).
            sky.windowEmission = 1.0f;
            break;
        }
    }

    return sky;
}

}  // namespace world
