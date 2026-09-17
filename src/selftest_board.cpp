// Selftests for the run cycle (spec 4) and the countdown board (spec 7.6).
//
// Two things here are worth testing rather than looking at. The first is the countdown: spec 7.6
// says it counts 5 to 0 stepping on whole seconds, and "it looked like it counted down" is not
// evidence that it never skipped 3 or showed 5 for two seconds. The second is the board's own
// frame. Every box is placed on the CPU and rotated on the GPU, and the two conventions have to
// agree — when they did not, the layout still produced a plausible-looking row of numerals, half
// of which were quietly behind the face they belonged to. So the check here is not "is the board
// shaped right" but "does the placement basis match the one the vertex shader applies", which is
// the thing that was actually wrong.

#include "core/math.h"
#include "selftest_check.h"
#include "world/board.h"
#include "world/city.h"
#include "world/phase.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace selftest {

using namespace core;
using namespace world;

namespace {

// The rotation shaders/board.vert applies to a box: local +x lands on (c, s), local +z on (-s, c).
// Written out here rather than shared with the generator on purpose — a test that calls the same
// helper the code under test calls cannot catch the two disagreeing.
Vec2 ShaderRight(float yaw) { return Vec2{std::cos(yaw), std::sin(yaw)}; }
Vec2 ShaderForward(float yaw) { return Vec2{-std::sin(yaw), std::cos(yaw)}; }

City MakeCity(uint64_t seed) {
    CityParams p;
    p.seed      = seed;
    p.radius    = 760.0f;
    p.gridAngle = 0.4f;
    return GenerateCity(p);
}

int PopCount(uint8_t v) {
    int n = 0;
    while (v) {
        n += v & 1;
        v >>= 1;
    }
    return n;
}

}  // namespace

void TestTimeline() {
    std::printf("run cycle (spec 4)\n");

    bool lengthInRange  = true;
    bool ordered        = true;
    bool countdownFive  = true;
    bool growthInRange  = true;
    bool blastOverlaps  = true;

    for (uint64_t seed = 1; seed <= 200; ++seed) {
        const Timeline tl = Timeline::Create(seed * 6364136223846793005ull + 1442695040888963407ull);

        if (tl.total() < 80.0f - 0.01f || tl.total() > 115.0f + 0.01f) lengthInRange = false;

        // Every phase runs forward, and each starts no earlier than the previous one.
        float previousStart = -1.0f;
        for (int i = 0; i < kPhaseCount; ++i) {
            const Phase p = static_cast<Phase>(i);
            if (tl.End(p) < tl.Start(p)) ordered = false;
            if (tl.Start(p) < previousStart) ordered = false;
            previousStart = tl.Start(p);
        }

        // Spec 7.6: the countdown is exactly five seconds, not "about five".
        if (std::fabs(tl.Duration(Phase::Countdown) - 5.0f) > 0.001f) countdownFive = false;

        // Spec 6.5: the city grows over 5 to 10 seconds.
        const float growth = tl.Duration(Phase::Growth);
        if (growth < 5.0f - 0.01f || growth > 10.0f + 0.01f) growthInRange = false;

        // Spec 4.1: the blast front is still expanding while the nearest triangles already fly.
        if (tl.Start(Phase::Scatter) >= tl.End(Phase::Blast)) blastOverlaps = false;
    }

    Check(lengthInRange, "cycle length stays in 80-115 s (spec 4.1) over 200 seeds");
    Check(ordered, "phases start in order and never run backwards");
    Check(countdownFive, "the countdown phase is exactly 5 s");
    Check(growthInRange, "growth lasts 5-10 s (spec 6.5)");
    Check(blastOverlaps, "blast and scatter overlap (spec 4.1)");

    // The countdown itself, sampled far more finely than it steps. This is the check that would
    // have caught a countdown that skipped a digit or held one twice as long as the rest.
    const Timeline tl = Timeline::Create(20260917ull);

    const float countdownStart = tl.Start(Phase::Countdown);
    const float countdownEnd   = tl.End(Phase::Countdown);

    Check(tl.BoardSeconds(countdownStart - 0.01f) < 0, "the board is dark before the countdown");
    Check(tl.BoardSeconds(countdownStart) == 5, "the countdown opens on 5");
    Check(tl.BoardSeconds(tl.Start(Phase::Flash)) < 0, "the board goes dark at the flash");
    Check(tl.BoardSeconds(tl.Start(Phase::Missile)) == 0,
          "the board holds at zero through the missile (spec 7.6)");

    bool      monotonic = true;
    bool      stepsOnce = true;
    int       previous  = 6;
    int       seen[6]   = {0, 0, 0, 0, 0, 0};
    const int samples   = 50000;
    for (int i = 0; i < samples; ++i) {
        const float t = countdownStart + (countdownEnd - countdownStart) *
                                             (static_cast<float>(i) / static_cast<float>(samples));
        const int v = tl.BoardSeconds(t);
        if (v < 0 || v > 5) {
            monotonic = false;
            continue;
        }
        if (v > previous) monotonic = false;
        if (previous <= 5 && v < previous - 1) stepsOnce = false;
        previous = v;
        ++seen[v];
    }

    Check(monotonic, "the countdown never counts up");
    Check(stepsOnce, "the countdown never skips a digit");

    bool allShown = true;
    for (int v = 1; v <= 5; ++v) {
        if (seen[v] == 0) allShown = false;
    }
    Check(allShown, "every digit from 5 to 1 is shown during the countdown");
    Check(seen[0] == 0, "zero belongs to the missile run, not to the five-second countdown");

    // Each digit holds for the same length: one second out of five, within a sample of tolerance.
    const float perSample = (countdownEnd - countdownStart) / static_cast<float>(samples);
    bool evenHolds = true;
    for (int v = 1; v <= 5; ++v) {
        const float held = static_cast<float>(seen[v]) * perSample;
        if (std::fabs(held - 1.0f) > 0.01f) evenHolds = false;
    }
    Check(evenHolds, "digits 5 down to 1 each hold for one second");
}

void TestBoard() {
    std::printf("countdown board (spec 7.6)\n");

    // --- the seven-segment patterns ---------------------------------------------------------
    static const int kExpectedBars[10] = {6, 2, 5, 5, 4, 5, 6, 3, 7, 6};

    bool barCounts = true;
    for (int d = 0; d <= 9; ++d) {
        if (PopCount(DigitSegments(d)) != kExpectedBars[d]) barCounts = false;
    }
    Check(barCounts, "every digit lights the conventional number of segments");

    bool distinct = true;
    for (int a = 0; a <= 9; ++a) {
        for (int b = a + 1; b <= 9; ++b) {
            if (DigitSegments(a) == DigitSegments(b)) distinct = false;
        }
    }
    Check(distinct, "no two digits share a segment pattern");

    bool inRange = true;
    for (int d = 0; d <= 9; ++d) {
        if (DigitSegments(d) & ~0x7Fu) inRange = false;
    }
    Check(inRange, "no digit lights a bit outside the seven segments");
    Check(DigitSegments(-1) == 0 && DigitSegments(10) == 0, "a digit out of range lights nothing");

    // --- the mask ----------------------------------------------------------------------------
    Check(BoardMask(-1) == 0, "a negative count leaves the board dark");

    const uint64_t colons = (1ull << (2 * kSegmentsPerGlyph)) |
                            (1ull << (2 * kSegmentsPerGlyph + 1)) |
                            (1ull << (5 * kSegmentsPerGlyph)) |
                            (1ull << (5 * kSegmentsPerGlyph + 1));

    bool colonsLit  = true;
    bool withinIds  = true;
    bool secondsRun = true;
    for (int s = 0; s <= 5; ++s) {
        const uint64_t mask = BoardMask(s);
        if ((mask & colons) != colons) colonsLit = false;
        if (mask >> kSegmentIdCount) withinIds = false;

        // The last glyph is the seconds' units digit. Its bits must be exactly that digit's.
        const uint64_t want = static_cast<uint64_t>(DigitSegments(s)) << (7 * kSegmentsPerGlyph);
        const uint64_t got  = mask & (0x7Full << (7 * kSegmentsPerGlyph));
        if (got != want) secondsRun = false;
    }
    Check(colonsLit, "the colons are lit whenever the board is (spec 7.6)");
    Check(withinIds, "no lit bit falls outside the 56 segment ids");
    Check(secondsRun, "the last glyph shows the seconds digit");

    // A full clock, not a display that only works below ten: the board is driven as HH:MM:SS.
    const uint64_t hour = BoardMask(3600 + 2 * 60 + 7);
    Check((hour & (0x7Full << (1 * kSegmentsPerGlyph))) ==
              (static_cast<uint64_t>(DigitSegments(1)) << (1 * kSegmentsPerGlyph)),
          "01:02:07 puts a 1 in the hours");
    Check((hour & (0x7Full << (7 * kSegmentsPerGlyph))) ==
              (static_cast<uint64_t>(DigitSegments(7)) << (7 * kSegmentsPerGlyph)),
          "01:02:07 puts a 7 in the seconds");

    // --- the geometry ------------------------------------------------------------------------
    const City  city  = MakeCity(20260917ull);
    const Board board = GenerateBoard(20260917ull, city);

    Check(!board.boxes.empty(), "the board generates geometry");
    Check(board.glyphHeight > 0.0f, "the glyphs have a height");

    // Spec 7.6: the board never billboards. Nothing here takes a time, so the yaw cannot follow
    // the camera — and generating it twice must give the same object.
    const Board again = GenerateBoard(20260917ull, city);
    Check(again.yaw == board.yaw && again.boxes.size() == board.boxes.size(),
          "the board is a pure function of the seed and the city (never billboards)");

    bool positive = true;
    for (const BoardBox& b : board.boxes) {
        if (b.height <= 0.0f || b.halfExtent.x <= 0.0f || b.halfExtent.y <= 0.0f) positive = false;
    }
    Check(positive, "no box is degenerate");

    // Every segment bar sits on a flat face, so its distance from the board's axis measured along
    // its own outward normal is the same for all of them — the face is a plane. This is the check
    // that fails when the CPU's placement basis and the vertex shader's yaw rotation disagree: the
    // bars still land in a neat row, but the row crosses the plane it is supposed to lie in, and
    // half of them end up behind the face they belong to.
    float depthMin = 1e9f, depthMax = -1e9f;
    bool  outward  = true;
    for (const BoardBox& b : board.boxes) {
        if (b.segmentId < 0) continue;

        const Vec2  fwd   = ShaderForward(b.yaw);
        const float depth = b.center.x * fwd.x + b.center.y * fwd.y;
        if (depth <= 0.0f) outward = false;
        if (depth < depthMin) depthMin = depth;
        if (depth > depthMax) depthMax = depth;

        // And sideways it must stay within the face it is on.
        const Vec2  right = ShaderRight(b.yaw);
        const float side  = b.center.x * right.x + b.center.y * right.y;
        if (std::fabs(side) > board.width * 0.5f) outward = false;
    }
    Check(outward, "every segment bar sits on the outward side of its own face, within its width");
    Check(depthMax - depthMin < 0.5f,
          "all segment bars lie in one plane per face (CPU basis matches the vertex shader)");

    // Four identical faces, driven by one mask so they cannot disagree between frames (spec 7.6).
    std::vector<int>   counts(kSegmentIdCount, 0);
    std::vector<float> heights(kSegmentIdCount, -1.0f);
    std::vector<float> bases(kSegmentIdCount, 0.0f);

    bool sameShape = true;
    bool idsInRange = true;
    for (const BoardBox& b : board.boxes) {
        if (b.segmentId < 0) continue;
        if (b.segmentId >= kSegmentIdCount) {
            idsInRange = false;
            continue;
        }
        ++counts[b.segmentId];
        if (heights[b.segmentId] < 0.0f) {
            heights[b.segmentId] = b.height;
            bases[b.segmentId]   = b.base;
        } else if (std::fabs(heights[b.segmentId] - b.height) > 1e-3f ||
                   std::fabs(bases[b.segmentId] - b.base) > 1e-3f) {
            sameShape = false;
        }
    }

    Check(idsInRange, "segment ids stay inside the mask");

    int used = 0;
    bool fourOfEach = true;
    for (int id = 0; id < kSegmentIdCount; ++id) {
        if (counts[id] == 0) continue;
        ++used;
        if (counts[id] != 4) fourOfEach = false;
    }
    Check(fourOfEach, "every segment appears on exactly four faces");
    Check(sameShape, "the four copies of a segment are the same size and height");

    // Six digits of seven bars and two colons of two: the colons leave five ids each unused.
    Check(used == 6 * kSegmentsPerGlyph + 2 * 2, "the used segment ids are the ones HH:MM:SS needs");

    // The band straddles the skyline (spec 7.6): the city passes in front of the numerals rather
    // than standing clear of them.
    Check(board.bandBottom < city.tallest && board.bandTop > city.tallest,
          "the glyph band straddles the tallest roof");

    // And the whole thing stands inside the city it belongs to.
    float furthest = 0.0f;
    for (const BoardBox& b : board.boxes) {
        const float reach = std::sqrt(b.center.x * b.center.x + b.center.y * b.center.y) +
                            std::sqrt(b.halfExtent.x * b.halfExtent.x +
                                      b.halfExtent.y * b.halfExtent.y);
        if (reach > furthest) furthest = reach;
    }
    Check(furthest < city.params.radius, "the board's footprint fits inside the city");

    // Spec 6.5: the board goes up after the last building.
    Check(board.riseStart >= city.growthEnds, "the board rises after the final building");
    Check(board.riseDuration > 0.0f, "the board takes time to rise");
}

}  // namespace selftest
