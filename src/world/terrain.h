// The desert basin (spec 6.2).
//
// A note on LOD, because this departs from the implementation plan. The plan called for a chunked
// LOD grid, which is what you build when the camera can go anywhere. This camera cannot: spec
// 11.1 confines it to an orbit around the city centre for the entire cycle. So the mesh is a
// radial grid centred on that same point, with ring spacing that grows with radius — dense where
// the camera always is, coarse where it never goes.
//
// That is not a shortcut, it is a better fit. Chunked LOD earns its complexity by switching
// detail as the viewer moves, and every switch is a chance to pop; spec 6.2 says the horizon MUST
// NOT visibly pop. A mesh whose detail distribution is fixed at generation time cannot pop at
// all, because nothing about it changes while it is on screen.
#ifndef NUKE_SAVER_WORLD_TERRAIN_H
#define NUKE_SAVER_WORLD_TERRAIN_H

#include "world/mesh.h"

#include <cstdint>

namespace world {

struct TerrainParams {
    uint64_t seed = 0;

    // Spec 6.2 puts the basin at a nominal 8 km across. `skirtRadius` carries the ground out far
    // past it so the horizon range always has something to stand on — without it, the flat plane
    // would end in mid-air short of the mountains and the gap would read as a void.
    float basinRadius = 4000.0f;
    float skirtRadius = 45000.0f;

    // The city stands on flat ground (spec 6.2, "flattened where the city will stand"), and the
    // flattening fades out over a margin so the pad does not read as a plateau.
    float cityRadius = 800.0f;

    // Ring spacing near the city, and the rate it grows with radius beyond it.
    float nearSpacing  = 25.0f;
    float spacingGrowth = 0.035f;

    // Segments around the circle. 256 gives about 20 m of arc at the city edge, matching the ring
    // spacing there, so the triangles near the camera are roughly equilateral.
    uint32_t segments = 256;
};

// Height in metres at a point on the ground plane. Deterministic, and the single source of truth:
// the mesh, the city pad and anything that needs to sit on the ground all call this rather than
// keeping their own idea of where the ground is.
float TerrainHeight(const TerrainParams& params, float x, float z);

// The analytic-ish surface normal, by central difference of TerrainHeight.
core::Vec3 TerrainNormal(const TerrainParams& params, float x, float z);

// The ground's linear albedo at a world position, blended between the sand and rock entries of
// spec 5.3 by `rockiness`. The variation of spec 5.2 comes from a smooth field over the world, not
// from a draw per vertex — see world::Vertex for what a per-vertex draw does to a radial grid.
core::Vec3 GroundAlbedo(const TerrainParams& params, float x, float z, float rockiness);

Mesh BuildTerrain(const TerrainParams& params);

}  // namespace world

#endif
