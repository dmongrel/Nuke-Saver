// The city (spec 6.4) and its growth (spec 6.5).
//
// 500 boxes, hard limit. Every one of them is drawn from a single unit cube, so nothing here
// produces geometry — it produces a list of instances, and the shape of the skyline has to come
// out of the distribution of those instances rather than out of any modelling.
//
// The layout is generated in the order spec 6.4 sets: an extent, a road grid rotated by a random
// angle with a few arterials cut across it, blocks split into lots, one box per lot. The reason
// for that order is that it is the only part of this that makes the result look designed. A field
// of boxes scattered by rejection sampling reads as a field of boxes; the same boxes aligned to a
// grid and separated by streets read as a city, because the streets are what the eye follows.
#ifndef NUKE_SAVER_WORLD_CITY_H
#define NUKE_SAVER_WORLD_CITY_H

#include "core/math.h"

#include <cstdint>
#include <vector>

namespace world {

// One box. This is per-instance data and nothing else: no mesh, no index, no bounding volume.
// Spec 6.4 limits instance data to transform, colour, growth time and fragment range, and the
// fragment range belongs to M4 — when it arrives it belongs here, not in a parallel array.
struct Building {
    core::Vec2 center{};      // world XZ of the footprint centre
    core::Vec2 halfExtent{};  // footprint half-size along the building's own axes
    float      rotation = 0.0f;  // yaw in radians; every building shares the road grid's angle
    float      height   = 0.0f;

    core::Vec3 bodyColor{};    // linear, spec 5.3 kBuildingBody
    core::Vec3 windowColor{};  // linear, spec 5.3 kBuildingWindow

    // Spec 6.5. `growthStart` is seconds from the beginning of the growth phase, not from the
    // beginning of the cycle: M5 owns when the phase starts, and a building that stored an
    // absolute time would have to be regenerated if the phase moved.
    float growthStart    = 0.0f;
    float growthDuration = 0.0f;

    // Decorrelates this building's window pattern from every other. Passed to the shader rather
    // than the id, because the shader wants something it can feed straight into a hash.
    // The window pitch is derived from it too, in the shader: a single pitch across the whole
    // city makes it one texture sampled five hundred times, and the eye reads that as a texture
    // rather than as buildings. Derived there rather than stored here because spec 6.4 limits
    // instance data to transform, colour, growth time and fragment range, and this is none of
    // those — it is one more thing the seed already determines.
    float windowSeed = 0.0f;
};

struct CityParams {
    uint64_t seed = 0;

    // Spec 6.4: 1.2 to 2 km across.
    float radius = 800.0f;

    int buildingCount = 500;  // hard limit for this revision

    // The grid's angle. Drawn per cycle, because a city aligned to the world axes reads as a
    // spreadsheet and — more to the point — would look identical from the same point of every
    // orbit, which spec 6.4 calls a defect.
    float gridAngle = 0.0f;

    float blockSize = 95.0f;  // nominal block edge before splitting
    float roadWidth = 18.0f;  // gap left between blocks
    int   arterials = 2;      // 1 to 3, cut across the grid at their own angles

    // How long the whole city takes to appear, and how long any one building takes to rise.
    // Spec 6.5 puts the phase at 5 to 10 seconds and a single rise at 0.4 to 0.9.
    float growthSeconds = 7.0f;
};

struct City {
    CityParams            params;
    std::vector<Building> buildings;

    // The tallest thing standing, which the camera framing of spec 11.1 and the countdown board
    // of spec 7.6 both have to clear.
    float tallest = 0.0f;

    // When the last building finishes rising. The board goes up after this (spec 6.5).
    float growthEnds = 0.0f;
};

City GenerateCity(const CityParams& params);

}  // namespace world

#endif
