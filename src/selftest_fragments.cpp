// Selftests for the detonation's numbers and the fragment box list (spec 7.2 to 7.7).
//
// Almost nothing about the fragment system can be checked from the CPU: the triangles are cut on
// the device and never come back. What can be checked is the contract the device code depends on,
// and it is a sharp one. Every fragment index has to resolve to exactly one box by binary search,
// which is true only if the boxes are in fragment order, their ranges are contiguous with no gap
// and no overlap, and every offset survives the trip through a float. Break any of those and the
// symptom is not an error — it is a few thousand triangles quietly wearing another building's
// colour and flying to another building's place on the cloud.
//
// So the search the shader performs is written out again here, over the same data, and asked about
// every fragment in the world.

#include "core/math.h"
#include "render/fragment_data.h"
#include "selftest_check.h"
#include "world/city.h"
#include "world/detonation.h"
#include "world/phase.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace selftest {

using namespace render;
using namespace world;

namespace {

City MakeCity(uint64_t seed) {
    CityParams p;
    p.seed      = seed;
    p.radius    = 760.0f;
    p.gridAngle = 0.4f;
    return GenerateCity(p);
}

// The search from shaders/fragment_common.glsl, transcribed rather than shared: a test that calls
// the code under test cannot catch the two disagreeing.
uint32_t FragmentBox(const std::vector<ShatterBox>& boxes, uint32_t fragment) {
    uint32_t lo = 0;
    uint32_t hi = static_cast<uint32_t>(boxes.size()) - 1;
    while (lo < hi) {
        const uint32_t mid = (lo + hi + 1) / 2;
        if (static_cast<uint32_t>(boxes[mid].color[3] + 0.5f) <= fragment) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

int SideQuadsOf(const ShatterBox& box) {
    return static_cast<int>(box.extentHeight[3] + 0.5f);
}

}  // namespace

void TestDetonation() {
    std::printf("detonation (spec 7.2, 7.5)\n");

    bool startsAtZero  = true;
    bool crossesCity   = true;
    bool shellGrows    = true;
    bool cloudGrows    = true;
    bool cloudEndsFull = true;
    bool windIsFlat    = true;
    bool capOverStem   = true;

    for (uint64_t s = 1; s <= 200; ++s) {
        const uint64_t   seed = s * 6364136223846793005ull + 1442695040888963407ull;
        const Timeline   tl   = Timeline::Create(seed);
        const float      radius = 600.0f + static_cast<float>(s % 400);
        const Detonation det    = Detonation::Create(seed, radius, 180.0f);

        if (det.ShellRadius(tl, tl.Start(Phase::Blast)) != 0.0f) startsAtZero = false;
        if (det.ShellRadius(tl, tl.Start(Phase::Blast) - 0.1f) != 0.0f) startsAtZero = false;

        // Spec 7.2: it MUST cross the whole city inside phase 6.
        if (det.ShellRadius(tl, tl.End(Phase::Blast)) < radius) crossesCity = false;

        float previous = -1.0f;
        for (int i = 0; i <= 64; ++i) {
            const float t = tl.Start(Phase::Blast) +
                            tl.Duration(Phase::Blast) * (static_cast<float>(i) / 64.0f);
            const float r = det.ShellRadius(tl, t);
            if (r < previous) shellGrows = false;
            previous = r;
        }

        if (det.CloudGrow(tl, tl.Start(Phase::Gather) - 0.1f) != 0.0f) cloudGrows = false;

        previous = -1.0f;
        for (int i = 0; i <= 64; ++i) {
            const float t = tl.Start(Phase::Gather) +
                            tl.Duration(Phase::Gather) * (static_cast<float>(i) / 64.0f);
            const float g = det.CloudGrow(tl, t);
            if (g < previous || g > 1.0f) cloudGrows = false;
            previous = g;
        }
        if (std::fabs(det.CloudGrow(tl, tl.End(Phase::Gather)) - 1.0f) > 1e-4f) {
            cloudEndsFull = false;
        }

        // Spec 7.5: the drift is a wind, so it is horizontal.
        if (det.wind.y != 0.0f) windIsFlat = false;
        const float speed = std::sqrt(det.wind.x * det.wind.x + det.wind.z * det.wind.z);
        if (speed < 4.9f || speed > 11.1f) windIsFlat = false;

        // A mushroom, not a ball on a stick: the cap sits above the top of the stem.
        if (det.capHeight <= det.stemHeight) capOverStem = false;
        if (det.capRadius <= det.capTube) capOverStem = false;
    }

    Check(startsAtZero, "the shell has no radius before the blast phase");
    Check(shellGrows, "the shell only ever expands");
    Check(crossesCity, "the shell crosses the whole city inside phase 6 (spec 7.2)");
    Check(cloudGrows, "the cloud grows monotonically across the gather phase and never exceeds 1");
    Check(cloudEndsFull, "the cloud is fully grown by the end of the phase");
    Check(windIsFlat, "the cloud's drift is a horizontal wind of 5 to 11 m/s");
    Check(capOverStem, "the cap sits above the stem and is wider than it is thick");
}

void TestFragmentLayout() {
    std::printf("fragment layout (spec 7.3, 11.2)\n");

    // Spec 11.2 gives up fragments per building in the order 300, 200, 120, 60. The compute shader
    // is handed S and derives the count; these must be the counts that come out.
    static const int kWanted[4] = {300, 200, 120, 60};
    bool             levels     = true;
    for (int q = 0; q < 4; ++q) {
        if (SideQuadsToTriangles(SideQuadsForQuality(q)) != kWanted[q]) levels = false;
    }
    Check(levels, "the quality levels produce 300, 200, 120 and 60 triangles a building");

    const City  city  = MakeCity(20260917ull);
    const Board board = GenerateBoard(20260917ull, city, core::Vec3{1500.0f, 120.0f, 0.0f},
                                      core::Vec3{0.0f, 0.0f, -1.0f});

    std::vector<ShatterBox> boxes;
    FragmentLayout          layout;
    PackShatterBoxes(city, board, 1, &boxes, &layout);

    Check(layout.boxes == city.buildings.size() + board.boxes.size(),
          "every building and every board box is shatterable");
    Check(!boxes.empty() && layout.total > 0, "the pack produces fragments");

    // Contiguous, ordered, no gaps, no overlap — the precondition the device-side binary search
    // rests on, and the one that breaks silently.
    bool     contiguous = true;
    bool     ordered    = true;
    bool     quadsSane  = true;
    uint32_t expected   = 0;
    for (const ShatterBox& box : boxes) {
        const uint32_t first = static_cast<uint32_t>(box.color[3] + 0.5f);
        if (first != expected) contiguous = false;
        if (first < expected) ordered = false;

        const int s = SideQuadsOf(box);
        if (s < 1 || s > 30) quadsSane = false;

        expected += static_cast<uint32_t>(SideQuadsToTriangles(s));
    }
    Check(contiguous, "fragment ranges are contiguous with no gap and no overlap");
    Check(ordered, "boxes are in fragment order, as the binary search requires");
    Check(quadsSane, "every box is cut at least once and no more than thirty times a side");
    Check(expected == layout.total, "the reported total is the sum of the ranges");

    // Offsets travel to the device inside a float. Above 2^24 that stops being exact, and a
    // fragment would resolve to the box next door.
    Check(layout.total < (1u << 24), "every fragment offset is exactly representable as a float");

    bool buildingsFixed = true;
    for (size_t i = 0; i < city.buildings.size(); ++i) {
        if (SideQuadsToTriangles(SideQuadsOf(boxes[i])) != 200) buildingsFixed = false;
    }
    Check(buildingsFixed, "every building gets the same cut, as spec 7.3 requires");

    // The board's pieces do not, and that is the point: a 2 m rail and a 136 m plate must not get
    // the same budget.
    int boardMin = 1000, boardMax = 0;
    for (size_t i = city.buildings.size(); i < boxes.size(); ++i) {
        const int s = SideQuadsOf(boxes[i]);
        if (s < boardMin) boardMin = s;
        if (s > boardMax) boardMax = s;
    }
    Check(boardMax > boardMin, "the board's pieces are cut according to their size");

    // And now the search itself, over every fragment in the world.
    bool resolves = true;
    for (uint32_t f = 0; f < layout.total; ++f) {
        const uint32_t b     = FragmentBox(boxes, f);
        const uint32_t first = static_cast<uint32_t>(boxes[b].color[3] + 0.5f);
        const uint32_t count = static_cast<uint32_t>(SideQuadsToTriangles(SideQuadsOf(boxes[b])));
        if (f < first || f >= first + count) resolves = false;
    }
    Check(resolves, "every fragment index resolves to the box that owns it");

    // The count itself. Spec 7.3 puts it at roughly 125,000 across the city; the board adds its
    // own on top, and the whole thing has to stay in the range the buffers are sized for.
    std::printf("  fragments        : %u from %u boxes\n", layout.total, layout.boxes);
    Check(layout.total > 90000 && layout.total < 200000,
          "the fragment count stays near the 125,000 of spec 7.3");
}

}  // namespace selftest
