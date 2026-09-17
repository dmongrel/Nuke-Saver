// The CPU half of the fragment system (spec 7.3).
//
// This is everything the CPU is allowed to know about fragments: how many there are, and the 728
// boxes they will be cut from. The triangles themselves are produced by shaders/fragment_init.comp
// straight into device memory and are never seen from here — spec 7.3 requires it, and the
// consequence is that the whole system costs one 35 KB upload at world build.
//
// ShatterBox and the struct of the same name in shaders/fragment_common.glsl describe the same
// bytes and MUST be changed together.
#ifndef NUKE_SAVER_RENDER_FRAGMENT_DATA_H
#define NUKE_SAVER_RENDER_FRAGMENT_DATA_H

#include "world/board.h"
#include "world/city.h"

#include <cstdint>
#include <vector>

namespace render {

// A box is cut as four sides of S quads and two caps of S/2, two triangles each — so S is the one
// number that describes the cut, and the triangle count follows from it. Carrying S rather than
// the count means the compute shader never has to divide a count back into a grid and get a
// different answer than the packer did.
constexpr int SideQuadsToTriangles(int s) { return 8 * s + 4 * (s / 2 > 0 ? s / 2 : 1); }

struct ShatterBox {
    float centerYaw[4];     // xz footprint centre, y underside, w yaw
    float extentHeight[4];  // xy half-extent, z height, w side quads S
    float color[4];         // rgb linear albedo, w index of this box's first fragment
};

static_assert(sizeof(ShatterBox) == 48, "ShatterBox must stay tight");

struct FragmentLayout {
    uint32_t boxes = 0;
    uint32_t total = 0;
};

// Side quads per building at each quality level. Spec 11.2 gives up fragments per building in the
// order 300, 200, 120, 60, which are the triangle counts these produce.
int SideQuadsForQuality(int quality);

// Packs the city and the board into one box list, buildings first, each carrying the index of its
// own first fragment so a fragment index resolves back to its box with a search rather than a
// division. The board's segment bars keep the lit amber and its structure the dark grey, so the
// cloud carries the board's colours through it as spec 7.3 asks.
//
// Buildings get a fixed cut, because spec 7.3 fixes it. The board's pieces do not: they range from
// a 2 m rail to a 136 m plate, and one budget for all of them either wastes triangles on the rails
// or leaves the plates as a handful of enormous slivers — which is what it did look like.
void PackShatterBoxes(const world::City& city, const world::Board& board, int quality,
                      std::vector<ShatterBox>* out, FragmentLayout* layout);

}  // namespace render

#endif
