#include "world/city.h"

#include "app/log.h"
#include "core/color.h"
#include "core/rng.h"

#include <algorithm>

namespace world {
namespace {

using core::Rng;
using core::Vec2;
using core::Vec3;

// An axis-aligned rectangle in *grid* space — the frame the road grid is laid out in, before the
// whole city is rotated into the world by CityParams::gridAngle. Everything up to the last step
// happens here, because axis-aligned rectangles split cleanly and rotated ones do not.
struct Rect {
    Vec2 min{};
    Vec2 max{};

    float width() const { return max.x - min.x; }
    float depth() const { return max.y - min.y; }
    float area() const { return width() * depth(); }
    Vec2  center() const { return Vec2{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f}; }
};

// Recursive subdivision of a block into lots. Splits the longer axis, which keeps lots from
// degenerating into slivers — split a random axis instead and a block occasionally comes out as a
// row of corridors, which then have to be thrown away, which pulls the count around.
//
// Stops on area rather than on depth. Depth gives every block the same number of lots regardless
// of how big it is, so the arterials — which cut blocks down to odd sizes — end up lined with
// lots a quarter the size of everything else.
void SplitBlock(const Rect& block, Rng& rng, float targetArea, float minSide,
                std::vector<Rect>* out) {
    if (block.width() < minSide || block.depth() < minSide) return;

    const bool  alongX = block.width() >= block.depth();
    const float length = alongX ? block.width() : block.depth();

    if (block.area() <= targetArea || length < minSide * 2.0f) {
        out->push_back(block);
        return;
    }

    // Never so close to an edge that one side is unbuildable; within that, off centre, because a
    // block split down the middle every time produces a grid inside the grid.
    const float cut = core::Clamp(length * rng.Range(0.36f, 0.64f), minSide, length - minSide);

    Rect a = block;
    Rect b = block;
    if (alongX) {
        a.max.x = block.min.x + cut;
        b.min.x = a.max.x;
    } else {
        a.max.y = block.min.y + cut;
        b.min.y = a.max.y;
    }

    SplitBlock(a, rng, targetArea, minSide, out);
    SplitBlock(b, rng, targetArea, minSide, out);
}

// An arterial, in grid space: a line at its own angle with a half-width. Spec 6.4 asks for 1 to 3
// cutting across the primary grid, and "across" is the whole point — at the grid's own angle it
// would be indistinguishable from a wide street.
struct Arterial {
    Vec2  normal{};    // unit, perpendicular to the road
    float offset = 0.0f;  // signed distance of the centre line from the origin
    float halfWidth = 0.0f;
};

// True when any part of the rectangle lies inside the band. Testing the four corners is exact
// here: the band is the region between two parallel lines, so the rectangle intersects it if any
// corner is inside, or if corners fall on both sides — in which case the band cuts straight
// through even though no corner is in it.
bool Intersects(const Rect& r, const Arterial& road) {
    const Vec2 corners[4] = {r.min, Vec2{r.max.x, r.min.y}, r.max, Vec2{r.min.x, r.max.y}};

    bool anyBelow = false, anyAbove = false;
    for (const Vec2& c : corners) {
        const float d = core::Dot(c, road.normal) - road.offset;
        if (std::fabs(d) <= road.halfWidth) return true;
        if (d < 0.0f) anyBelow = true;
        else anyAbove = true;
    }
    return anyBelow && anyAbove;
}

// Height distribution. The mean falls off with distance from the centre so a downtown emerges
// without anyone authoring one (spec 6.4), and the draw around that mean is skewed — squaring a
// uniform draw puts most buildings low and a few well above their neighbours, which is what makes
// a skyline a skyline rather than a comb.
float DrawHeight(Rng& rng, float distanceFraction, float footprint) {
    const float t    = core::Saturate(distanceFraction);
    const float mean = core::Lerp(135.0f, 16.0f, std::pow(t, 0.75f));

    const float u = rng.Unit();
    float       h = mean * core::Lerp(0.40f, 2.1f, u * u);

    // A rare tower. Capped hard, because one building tall enough to leave frame would drag the
    // camera framing solver of spec 11.1 out for every shot in the cycle.
    if (rng.Chance(0.015f)) h *= rng.Range(1.4f, 1.9f);

    // Nothing narrower than it is tall by more than a factor of six. A 12 metre footprint at 140
    // metres is a chimney, and a field of them reads as a pin cushion.
    h = std::fmin(h, footprint * 5.0f);

    return core::Clamp(h, 7.0f, 260.0f);
}

}  // namespace

City GenerateCity(const CityParams& params) {
    City city;
    city.params = params;

    Rng rng = Rng(params.seed).Fork(0xC17E0Full);

    const float cos0 = std::cos(params.gridAngle);
    const float sin0 = std::sin(params.gridAngle);
    const auto  toWorld = [&](const Vec2& g) {
        return Vec2{g.x * cos0 - g.y * sin0, g.x * sin0 + g.y * cos0};
    };

    // The arterials. Angles are relative to the grid and kept away from multiples of a right
    // angle, so each one reads as cutting across rather than as a street that came out wide.
    std::vector<Arterial> arterials;
    for (int i = 0; i < params.arterials; ++i) {
        const float angle = rng.Range(0.35f, 1.22f) + (rng.Chance(0.5f) ? core::kPi * 0.5f : 0.0f);

        Arterial road;
        road.normal    = Vec2{-std::sin(angle), std::cos(angle)};
        road.offset    = rng.Range(-0.45f, 0.45f) * params.radius;
        road.halfWidth = rng.Range(13.0f, 21.0f);
        arterials.push_back(road);
    }

    // Blocks: the cells of the primary grid that fall inside the footprint, inset by the road
    // width. The grid is generated over the bounding square and filtered by the disc rather than
    // being built radially — a radial layout would put its own structure into the street pattern,
    // and the streets are the most legible thing in the whole city.
    const size_t wanted = static_cast<size_t>(params.buildingCount);

    // The lots, for a given stopping area. The area decides the lot's *shape* as much as its size,
    // because SplitBlock halves the longer side each time: at a fifth of the block a 90 m block
    // went one split too far and came out as 22 by 45 strips, and a two hundred metre building on
    // a twenty metre footprint is a slab, not a tower.
    const auto layOutLots = [&](float areaFraction) {
        std::vector<Rect> lots;

        const float half       = params.blockSize * 0.5f;
        const float inset      = params.roadWidth * 0.5f;
        const int   span       = static_cast<int>(params.radius / params.blockSize) + 2;
        const float targetArea = params.blockSize * params.blockSize * areaFraction;
        const float minSide    = 18.0f;

        for (int i = -span; i <= span; ++i) {
            for (int j = -span; j <= span; ++j) {
                Rect block;
                block.min = Vec2{static_cast<float>(i) * params.blockSize - half + inset,
                                 static_cast<float>(j) * params.blockSize - half + inset};
                block.max = Vec2{static_cast<float>(i) * params.blockSize + half - inset,
                                 static_cast<float>(j) * params.blockSize + half - inset};

                // Outside the footprint, in world terms. The centre is enough: a block straddling
                // the edge is kept or dropped whole, which keeps the outline blocky rather than
                // cutting buildings in half along a circle.
                const Vec2 w = toWorld(block.center());
                if (core::Length(w) > params.radius) continue;

                // Blocks the grid leaves whole but the arterials do not. The block is dropped
                // rather than clipped: a clipped block leaves lots hard against a main road with
                // no setback, and spec 6.4 has no mechanism for a building to know it is on a
                // corner.
                bool cut = false;
                for (const Arterial& road : arterials) {
                    if (Intersects(block, road)) cut = true;
                }
                if (cut) continue;

                // A block's lots are drawn from its own stream, keyed on the grid cell, so adding
                // an arterial does not reshuffle the lots of every block after it.
                Rng blockRng = rng.Fork(static_cast<uint64_t>((i + 4096) * 8192 + (j + 4096)));
                SplitBlock(block, blockRng, targetArea, minSide, &lots);
            }
        }

        return lots;
    };

    // Spec 6.4 asks for 500, and how many lots a footprint yields depends on the block size, the
    // road width and how much the arterials happened to cut — all drawn per cycle, so no single
    // stopping area hits the target for every seed. Subdividing further is the right knob to turn:
    // it fills the same streets more finely rather than spreading the city out, which changing the
    // radius or the block size would do.
    float             areaFraction = 0.32f;
    std::vector<Rect> lots         = layOutLots(areaFraction);
    for (int attempt = 0; attempt < 5 && lots.size() < wanted; ++attempt) {
        areaFraction *= 0.78f;
        lots = layOutLots(areaFraction);
    }

    // Spec 6.4: excess lots are discarded from the outside in, so the city stays dense at the
    // centre. Sorting by distance and truncating is that rule exactly — and it also means the
    // count is met by dropping the outskirts rather than by adjusting the grid, which would
    // change the street pattern every time the target moved.
    std::sort(lots.begin(), lots.end(), [&](const Rect& a, const Rect& b) {
        const float da = core::Dot(toWorld(a.center()), toWorld(a.center()));
        const float db = core::Dot(toWorld(b.center()), toWorld(b.center()));
        if (da != db) return da < db;
        // A stable tiebreak, so the same seed gives the same city on any standard library.
        return a.min.x != b.min.x ? a.min.x < b.min.x : a.min.y < b.min.y;
    });

    if (lots.size() > wanted) lots.resize(wanted);

    if (lots.size() < wanted) {
        app::Log("city: only %zu lots for %zu buildings (block %.0fm, radius %.0fm)", lots.size(),
                 wanted, params.blockSize, params.radius);
    }

    // The outermost lot that survived is the city's real extent, which is what the camera should
    // be framed against rather than the nominal radius.
    float extent = 0.0f;

    city.buildings.reserve(lots.size());
    for (size_t index = 0; index < lots.size(); ++index) {
        const Rect& lot = lots[index];
        const Vec2  worldCenter = toWorld(lot.center());
        const float distance    = core::Length(worldCenter);

        Rng lotRng = rng.Fork(0x1010ull + index);

        // Setback. Buildings do not fill their lots, or every block reads as one solid mass with
        // lines scored into it.
        const float setback = lotRng.Range(1.5f, 3.5f);
        float       ex      = std::fmax(lot.width() * 0.5f - setback, 4.0f);
        float       ez      = std::fmax(lot.depth() * 0.5f - setback, 4.0f);

        // Spec 6.4 caps the footprint aspect at 1:2.5. The lot can be longer than that, so the
        // long side is brought in rather than the short side pushed out — growing it would run
        // the building into the street.
        if (ex > ez * 2.5f) ex = ez * 2.5f;
        if (ez > ex * 2.5f) ez = ex * 2.5f;

        Building b;
        b.center     = worldCenter;
        b.halfExtent = Vec2{ex, ez};
        b.rotation   = params.gridAngle;
        b.height     = DrawHeight(lotRng, distance / params.radius, std::fmin(ex, ez) * 2.0f);

        b.bodyColor   = core::Albedo(core::palette::kBuildingBody, params.seed, index, 0);
        b.windowColor = core::Albedo(core::palette::kBuildingWindow, params.seed, index, 1);

        // Drives which panes are lit and, in the shader, how big a bay is.
        b.windowSeed = core::HashFloat(params.seed, index, 64) * 512.0f;

        // Spec 6.5: the centre starts first and the outskirts follow, so the city grows outward.
        // The exponent flattens the front — with a linear ramp the middle of the city appears in
        // a rush and the last two hundred buildings trickle in over the back half of the phase.
        const float wave = std::pow(core::Saturate(distance / params.radius), 0.7f);
        b.growthDuration = lotRng.Range(0.4f, 0.9f);
        b.growthStart =
            std::fmax(0.0f, wave * (params.growthSeconds - b.growthDuration) +
                                lotRng.Range(-0.25f, 0.25f));

        city.tallest    = std::fmax(city.tallest, b.height);
        city.growthEnds = std::fmax(city.growthEnds, b.growthStart + b.growthDuration);
        extent          = std::fmax(extent, distance + std::fmax(ex, ez));

        city.buildings.push_back(b);
    }

    city.params.radius = extent > 0.0f ? extent : params.radius;

    app::Log("city: %zu buildings, extent %.0fm, tallest %.0fm, grown by %.1fs",
             city.buildings.size(), city.params.radius, city.tallest, city.growthEnds);

    return city;
}

}  // namespace world
