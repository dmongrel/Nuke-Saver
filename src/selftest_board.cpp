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

    // Lit from the moment it exists, holding at five. The board goes up at the end of phase 1 and
    // the countdown does not begin until phase 3; standing dark across the whole of phase 2 made
    // it read as scenery. Showing 00:00:05 from the frame it appears is also what makes the first
    // digit change an event rather than the display switching on.
    Check(tl.BoardSeconds(countdownStart - 0.01f) == 5,
          "the board is lit at 00:00:05 from the moment it appears");
    Check(tl.BoardSeconds(tl.Start(Phase::Growth)) == 5, "and all the way back through phase 1");
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
    //
    // The board is placed and turned relative to where the camera stands during the countdown
    // (spec 7.6), so the tests need a viewpoint. This one stands off to the east at a plausible
    // orbit radius and height; nothing here depends on the particular numbers, only on the
    // relationship between them and the board.
    const City  city      = MakeCity(20260917ull);
    const Vec3  viewFrom  = Vec3{1500.0f, 120.0f, 0.0f};
    const Vec3  viewRight = Vec3{0.0f, 0.0f, -1.0f};  // looking at the origin from due east
    const Board board     = GenerateBoard(20260917ull, city, viewFrom, viewRight);

    Check(!board.boxes.empty(), "the board generates geometry");
    Check(board.glyphHeight > 0.0f, "the glyphs have a height");

    // Spec 7.6: the board never billboards. Nothing here takes a time, so the yaw cannot follow
    // the camera — and generating it twice must give the same object.
    const Board again = GenerateBoard(20260917ull, city, viewFrom, viewRight);
    Check(again.yaw == board.yaw && again.boxes.size() == board.boxes.size(),
          "the board is a pure function of the seed, the city and the viewpoint");

    bool positive = true;
    for (const BoardBox& b : board.boxes) {
        if (b.height <= 0.0f || b.halfExtent.x <= 0.0f || b.halfExtent.y <= 0.0f) positive = false;
    }
    Check(positive, "no box is degenerate");

    // Every segment bar sits on one flat face, so its distance from the board's own origin
    // measured along the face's outward normal is the same for all of them. This is the check that
    // fails when the CPU's placement basis and the vertex shader's yaw rotation disagree: the bars
    // still land in a neat row, but the row crosses the plane it is supposed to lie in, and half
    // of them end up behind the face they belong to.
    float depthMin = 1e9f, depthMax = -1e9f;
    bool  outward  = true;
    for (const BoardBox& b : board.boxes) {
        if (b.segmentId < 0) continue;

        const Vec2  local{b.center.x - board.origin.x, b.center.y - board.origin.y};

        const Vec2  fwd   = ShaderForward(b.yaw);
        const float depth = local.x * fwd.x + local.y * fwd.y;
        if (depth <= 0.0f) outward = false;
        if (depth < depthMin) depthMin = depth;
        if (depth > depthMax) depthMax = depth;

        // And sideways it must stay within the face it is on.
        const Vec2  right = ShaderRight(b.yaw);
        const float side  = local.x * right.x + local.y * right.y;
        if (std::fabs(side) > board.width * 0.5f) outward = false;
    }
    Check(outward, "every segment bar sits on the outward side of the face, within its width");
    Check(depthMax - depthMin < 0.5f,
          "all segment bars lie in one plane (CPU basis matches the vertex shader)");

    // One face, driven by one mask, so it cannot disagree with itself between frames (spec 7.6).
    std::vector<int>   counts(kSegmentIdCount, 0);
    bool               idsInRange = true;
    bool               sameYaw    = true;
    for (const BoardBox& b : board.boxes) {
        if (std::fabs(b.yaw - board.yaw) > 1e-4f) sameYaw = false;
        if (b.segmentId < 0) continue;
        if (b.segmentId >= kSegmentIdCount) {
            idsInRange = false;
            continue;
        }
        ++counts[b.segmentId];
    }

    Check(idsInRange, "segment ids stay inside the mask");
    Check(sameYaw, "every box on the board shares the board's one yaw");

    int  used     = 0;
    bool onlyOnce = true;
    for (int id = 0; id < kSegmentIdCount; ++id) {
        if (counts[id] == 0) continue;
        ++used;
        if (counts[id] != 1) onlyOnce = false;
    }
    Check(onlyOnce, "every segment appears exactly once — there is one face, not four");

    // Six digits of seven bars and two colons of two: the colons leave five ids each unused.
    Check(used == 6 * kSegmentsPerGlyph + 2 * 2, "the used segment ids are the ones HH:MM:SS needs");

    // It stands on the desert (spec 7.6), not on a mast over the city. The band is lifted clear of
    // the sand by about three quarters of a glyph and finishes below the tallest roof, so the
    // skyline still reads over and around the numerals rather than the other way about.
    Check(board.bandBottom > board.glyphHeight * 0.5f &&
              board.bandBottom < board.glyphHeight * 1.1f,
          "the glyph band is lifted about three quarters of a glyph off the ground");
    Check(board.bandTop < city.tallest, "and finishes below the tallest roof");

    bool onTheGround = true;
    for (const BoardBox& b : board.boxes) {
        if (b.base < -0.001f) onTheGround = false;
    }
    Check(onTheGround, "nothing on the board is below ground");

    // Off the city axis and out at the edge of the built ground, so that the outermost blocks pass
    // in front of it as the camera moves. Both bounds matter: at the centre nothing can occlude
    // it, and past the edge it is a sign standing on open desert with the city behind it.
    const float standoff =
        std::sqrt(board.origin.x * board.origin.x + board.origin.y * board.origin.y);
    Check(standoff > city.params.radius * 0.75f, "the board stands at the edge of the city");
    Check(standoff < city.params.radius, "and still inside the footprint");

    // The near edge, not the far one. On the far side the whole city is between the camera and the
    // numerals and two glyphs of eight survive, which is the version this replaced.
    {
        const Vec2  toBoard{board.origin.x - viewFrom.x, board.origin.y - viewFrom.z};
        const float toBoardLength = std::sqrt(toBoard.x * toBoard.x + toBoard.y * toBoard.y);
        const float toCentre = std::sqrt(viewFrom.x * viewFrom.x + viewFrom.z * viewFrom.z);
        Check(toBoardLength < toCentre, "the board is on the near edge of the city, not the far one");

        // And off to the camera's right, so it tracks along the front of the city as the orbit
        // carries the camera past it rather than sitting in the middle of the frame.
        const float sideways = (toBoard.x * viewRight.x + toBoard.y * viewRight.z) / toBoardLength;
        Check(sideways > 0.3f, "and off to the camera's right");
    }

    // And the whole thing stays inside the city it belongs to.
    float furthest = 0.0f;
    for (const BoardBox& b : board.boxes) {
        const float reach = std::sqrt(b.center.x * b.center.x + b.center.y * b.center.y) +
                            std::sqrt(b.halfExtent.x * b.halfExtent.x +
                                      b.halfExtent.y * b.halfExtent.y);
        if (reach > furthest) furthest = reach;
    }
    Check(furthest < city.params.radius * 1.15f, "the board's footprint stays with the city");

    // Two things at once, and they pull against each other. The face is laid along the tangent of
    // the city's circle, so it belongs to the city's geometry rather than being aimed at the
    // viewer — but it still has to be the front of the board that the countdown sees, not the
    // back. A single-faced board that happened to point away would be five seconds of the cycle
    // spent looking at the back of a sign.
    {
        const Vec2  fwd = ShaderForward(board.yaw);
        const Vec2  toView{viewFrom.x - board.origin.x, viewFrom.z - board.origin.y};
        const float length = std::sqrt(toView.x * toView.x + toView.y * toView.y);
        const float facing = (toView.x * fwd.x + toView.y * fwd.y) / (length > 0.0f ? length : 1.0f);
        Check(facing > 0.0f, "the countdown sees the front of the board, not the back");

        // Tangential means the face's normal is close to radial: pointing straight out of the
        // city rather than across it.
        const float reachLength =
            std::sqrt(board.origin.x * board.origin.x + board.origin.y * board.origin.y);
        const float radial =
            (board.origin.x * fwd.x + board.origin.y * fwd.y) / (reachLength > 0.0f ? reachLength
                                                                                    : 1.0f);
        Check(radial > 0.94f, "and the face lies along the tangent of the city's circle");
    }

    // Over sixty-four seeds, because one of those numbers is drawn per cycle and a board that
    // points the wrong way one run in ten is a defect nobody reproduces.
    {
        bool everyCycleReads  = true;
        bool everyCycleStands  = true;
        bool everyCycleIsRight = true;
        bool everyCycleIsNear  = true;

        // Eight bearings, so the "right" test is not accidentally passing on one alignment.
        for (uint64_t seed = 1; seed <= 64; ++seed) {
            const City  c = MakeCity(seed * 6364136223846793005ull + 1442695040888963407ull);

            const float bearing = core::kTwoPi * static_cast<float>(seed % 8) / 8.0f;
            const float orbit   = c.params.radius * 1.9f;
            const Vec3  from{std::cos(bearing) * orbit, 90.0f, std::sin(bearing) * orbit};

            // The camera looks at the city centre from there, so its right hand is a quarter turn
            // behind its bearing. Derived here the long way round, as the caller does.
            const Vec3 forward = core::Normalize(Vec3{-from.x, -from.y * 0.5f, -from.z});
            const Vec3 rightOf = core::Normalize(core::Cross(forward, Vec3{0.0f, 1.0f, 0.0f}));

            const Board b = GenerateBoard(seed * 7919ull, c, from, rightOf);

            const Vec2  fwd = ShaderForward(b.yaw);
            const Vec2  toView{from.x - b.origin.x, from.z - b.origin.y};
            const float length = std::sqrt(toView.x * toView.x + toView.y * toView.y);
            if ((toView.x * fwd.x + toView.y * fwd.y) / (length > 0.0f ? length : 1.0f) <= 0.0f) {
                everyCycleReads = false;
            }

            const float reach = std::sqrt(b.origin.x * b.origin.x + b.origin.y * b.origin.y);
            if ((b.origin.x * fwd.x + b.origin.y * fwd.y) / (reach > 0.0f ? reach : 1.0f) < 0.94f) {
                everyCycleReads = false;
            }

            const float off = std::sqrt(b.origin.x * b.origin.x + b.origin.y * b.origin.y);
            if (off <= c.params.radius * 0.75f || off >= c.params.radius) everyCycleStands = false;

            if ((-toView.x * rightOf.x + -toView.y * rightOf.z) / length < 0.3f) {
                everyCycleIsRight = false;
            }

            if (length >= std::sqrt(from.x * from.x + from.z * from.z)) everyCycleIsNear = false;
        }
        Check(everyCycleReads, "and for sixty-four seeds on eight bearings, never the back of it");
        Check(everyCycleStands, "which all stand at the edge rather than on the axis");
        Check(everyCycleIsRight, "always on the camera's right");
        Check(everyCycleIsNear, "and always on the near edge");
    }

    // Spec 6.5: the board goes up after the last building.
    Check(board.riseStart >= city.growthEnds, "the board rises after the final building");
    Check(board.riseDuration > 0.0f, "the board takes time to rise");
}

}  // namespace selftest
