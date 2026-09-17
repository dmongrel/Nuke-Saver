// Time of day, the key light, and the sky it is drawn against (spec 5.4 and 6.3).
//
// One rule shapes this whole file: the celestial body's position on the skybox and the direction
// of the key light come from the same value. Spec 6.3 calls a sun drawn in one place while
// shadows fall from another a defect, and the long shadows of morning and twilight make it
// obvious. So there is exactly one direction here, `keyDirection`, and both the skybox and the
// lighting read it. There is deliberately no second setting to drift out of sync with the first.
#ifndef NUKE_SAVER_WORLD_SKY_H
#define NUKE_SAVER_WORLD_SKY_H

#include "core/math.h"

#include <cstdint>

namespace world {

enum class TimeOfDay { Morning, Noon, Twilight, Night };

// Everything downstream needs to know about the lighting environment. All colour is linear
// scene-referred per spec 5.1.
struct Sky {
    TimeOfDay timeOfDay = TimeOfDay::Twilight;

    // Direction **towards** the light, normalised. The body is drawn here too. At twilight the
    // sun sits at or just below the horizon, so this may point slightly below y=0 — the sky
    // still carries its glow, which is the entire look of the default setting.
    core::Vec3 keyDirection{0.0f, 1.0f, 0.0f};
    core::Vec3 keyColor{1.0f, 1.0f, 1.0f};  // linear radiance, may exceed 1
    float      keyIntensity = 1.0f;

    // Ambient/bounce, standing in for the sky as an area source until M5 needs better.
    core::Vec3 ambientColor{0.1f, 0.1f, 0.12f};
    float      ambientScale = 1.0f;

    core::Vec3 zenithColor{0.05f, 0.10f, 0.25f};
    core::Vec3 horizonColor{0.4f, 0.3f, 0.2f};
    core::Vec3 groundColor{0.1f, 0.08f, 0.06f};  // below the horizon, behind the mountains

    // 0 at noon, 1 at night. Stars emerge as the sky darkens (spec 6.3).
    float starBrightness = 0.0f;

    // Angular radius of the body in radians. The real sun and moon are both about 0.0047 rad,
    // which on a 3440-wide display is a handful of pixels — too small to read as the source of
    // the light. Enlarged deliberately, the way film does.
    float bodyAngularRadius = 0.0f;
    // Emissive, well above 1 so it blooms (spec 5.1, 6.3).
    core::Vec3 bodyColor{0.0f, 0.0f, 0.0f};
    bool       bodyIsMoon = false;

    // Base exposure for the tonemap, before M5's adaptation. Night is not simply "darker": it is
    // a different exposure, or the city's own windows read as dim rather than as the light.
    float baseExposure = 1.0f;

    // Multiplier on window emission. Spec 5.4 requires it to scale inversely with ambient:
    // barely visible at noon, a main source of city light at night.
    float windowEmission = 1.0f;
};

// Builds the lighting environment. `seed` randomises within the ranges of spec 5.4 — the sun's
// elevation and compass bearing vary per cycle so two cycles at the same setting do not light
// the city identically.
Sky MakeSky(TimeOfDay timeOfDay, uint64_t seed);

// Resolves the Random setting to a concrete one. Kept separate from MakeSky so the choice is
// made once per cycle and logged, rather than being re-rolled by anything that asks.
TimeOfDay PickTimeOfDay(uint64_t seed);

const char* TimeOfDayName(TimeOfDay t);

}  // namespace world

#endif
