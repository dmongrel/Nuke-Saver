// The GPU side of spec 6.4: one unit cube, five hundred instances.
//
// These two structs and the vertex input in shaders/building.vert describe the same bytes and MUST
// be changed together — same arrangement as SceneUniforms, and for the same reason: there is no
// reflection here, and a mismatch shows up as a skyline of garbage rather than as an error.
#ifndef NUKE_SAVER_RENDER_BUILDING_DATA_H
#define NUKE_SAVER_RENDER_BUILDING_DATA_H

#include "world/board.h"
#include "world/city.h"

#include <cstdint>
#include <vector>

namespace render {

// The unit cube. Spans [-1, 1] in x and z and [0, 1] in y, so scaling y is scaling the building up
// from the ground rather than about its middle — which is what the growth of spec 6.5 needs, and
// what saves a translate in the vertex shader on every frame of every building.
//
// Twenty-four vertices, not eight: each face needs its own normal, and sharing corners would give
// a box smoothly shaded like a sphere.
struct BoxVertex {
    float position[3];
    float normal[3];
    float uv[2];  // 0..1 across the face, used to place windows in metres
};

// Per-instance data. Spec 6.4 allows transform, colour, growth time and fragment range and nothing
// else; the fragment range arrives with M4 and belongs in the spare w below.
struct BuildingInstance {
    float centerRotation[4];  // xz world centre, y height in metres, w yaw in radians
    float extentGrowth[4];    // xy footprint half-extent, z growth start, w growth duration
    float bodyColor[4];       // rgb linear, w window seed
    float windowColor[4];     // rgb linear, w reserved for the fragment range (M4)
};

// The countdown board (spec 7.6): masts, face panels and segment bars, all boxes, all one draw.
// Sharing a draw is not an optimisation here — it is what guarantees spec 7.6's requirement that
// all four faces update in the same frame, because there is no frame in which half of them could
// have been submitted.
struct BoardInstance {
    float baseYaw[4];  // xz footprint centre, y underside, w yaw
    float sizeId[4];   // xz half-extent, y full height, w segment id (-1 for structure)
};

static_assert(sizeof(BoardInstance) == 32, "BoardInstance must stay tight");
static_assert(sizeof(BoxVertex) == 32, "BoxVertex must stay tight");
static_assert(sizeof(BuildingInstance) == 64, "BuildingInstance must stay tight");

// The cube, as 24 vertices and 36 indices. Wound counter-clockwise seen from outside, matching the
// front face the pipeline expects.
void BuildUnitCube(std::vector<BoxVertex>* vertices, std::vector<uint32_t>* indices);

void PackBoard(const world::Board& board, std::vector<BoardInstance>* out);

// Packs the city into instance data, in the order the generator produced it.
void PackBuildings(const world::City& city, std::vector<BuildingInstance>* out);

}  // namespace render

#endif
