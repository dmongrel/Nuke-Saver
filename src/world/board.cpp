#include "world/board.h"

#include "app/log.h"
#include "core/color.h"
#include "core/rng.h"

namespace world {
namespace {

using core::Vec2;
using core::Vec3;

// Proportions, all as fractions of the glyph height, so the board scales with the city and a small
// city does not get numerals twice its width (spec 7.6).
constexpr float kDigitWidth = 0.56f;
constexpr float kColonWidth = 0.22f;
constexpr float kGapWidth   = 0.22f;
constexpr float kBarThick   = 0.105f;
constexpr float kBarDepth   = 0.055f;  // how far a bar stands proud of the face
constexpr float kPanelDepth = 0.06f;
// The recess is padded upwards and downwards only. Padding it sideways as well eats the gap
// between glyphs, and the row of separate plates becomes one continuous band — which is the
// billboard spec 7.6 rules out, arrived at from the other direction. The bars already reach the
// edges of their cell exactly, so a recess the width of the cell backs all of them.
constexpr float kRecessPad  = 0.055f;
constexpr float kRailThick  = 0.018f;  // the open lattice that carries the glyph row

// The glyphs of HH:MM:SS. Positions 2 and 5 are colons.
bool IsColon(int glyph) { return glyph == 2 || glyph == 5; }

float GlyphWidth(int glyph, float h) { return (IsColon(glyph) ? kColonWidth : kDigitWidth) * h; }

// One segment bar, in the face's own 2D frame: u across the face from its left edge, v up from the
// bottom of the glyph band. Returned as a half-extent and a centre, ready to become a box.
struct Bar {
    float u = 0.0f, v = 0.0f;         // centre
    float halfU = 0.0f, halfV = 0.0f; // half size
};

// Segment a..g for a digit cell whose left edge is at u0 and whose height is h.
//
// Generated from the proportions rather than tabulated, so changing the bar thickness moves every
// bar consistently instead of needing 56 numbers re-derived.
Bar DigitBar(int segment, float u0, float h) {
    const float w  = kDigitWidth * h;
    const float th = kBarThick * h;

    // Horizontal bars stop short of the verticals so the corners read as mitres rather than as a
    // solid block. The verticals are mitred at the top and bottom only: they MUST overlap at the
    // middle, because a zero has no middle segment and two verticals that merely reach towards
    // each other leave the digit split into two stacked rectangles.
    const float halfH = (w - th * 1.7f) * 0.5f;

    const float mitre = th * 0.35f;
    const float join  = th * 0.15f;  // how far each vertical crosses the middle line
    const float vLow  = mitre;
    const float vHigh = h - mitre;
    const float halfV = (h * 0.5f + join - mitre) * 0.5f;
    const float upperV = (h * 0.5f - join + vHigh) * 0.5f;
    const float lowerV = (vLow + h * 0.5f + join) * 0.5f;

    Bar bar;
    switch (segment) {
        case 0:  // a, top
            bar = {u0 + w * 0.5f, h - th * 0.5f, halfH, th * 0.5f};
            break;
        case 1:  // b, top right
            bar = {u0 + w - th * 0.5f, upperV, th * 0.5f, halfV};
            break;
        case 2:  // c, bottom right
            bar = {u0 + w - th * 0.5f, lowerV, th * 0.5f, halfV};
            break;
        case 3:  // d, bottom
            bar = {u0 + w * 0.5f, th * 0.5f, halfH, th * 0.5f};
            break;
        case 4:  // e, bottom left
            bar = {u0 + th * 0.5f, lowerV, th * 0.5f, halfV};
            break;
        case 5:  // f, top left
            bar = {u0 + th * 0.5f, upperV, th * 0.5f, halfV};
            break;
        default:  // g, middle
            bar = {u0 + w * 0.5f, h * 0.5f, halfH, th * 0.5f};
            break;
    }
    return bar;
}

// A colon uses two of its seven ids and leaves the rest unused. Wasteful by five bits per colon,
// and worth it: every glyph then addresses the same way, so the mask, the instance ids and the
// shader all index by glyph * 7 + segment with no special case anywhere.
Bar ColonBar(int segment, float u0, float h) {
    const float w  = kColonWidth * h;
    const float th = kBarThick * h;

    Bar bar;
    bar.u     = u0 + w * 0.5f;
    bar.v     = segment == 0 ? h * 0.30f : h * 0.70f;
    bar.halfU = th * 0.5f;
    bar.halfV = th * 0.5f;
    return bar;
}

}  // namespace

uint8_t DigitSegments(int digit) {
    // a=1, b=2, c=4, d=8, e=16, f=32, g=64.
    static const uint8_t kPatterns[10] = {
        0x3F,  // 0: a b c d e f
        0x06,  // 1: b c
        0x5B,  // 2: a b g e d
        0x4F,  // 3: a b g c d
        0x66,  // 4: f g b c
        0x6D,  // 5: a f g c d
        0x7D,  // 6: a f g e c d
        0x07,  // 7: a b c
        0x7F,  // 8: all
        0x6F,  // 9: a b c d f g
    };
    return digit >= 0 && digit <= 9 ? kPatterns[digit] : 0;
}

uint64_t BoardMask(int seconds) {
    if (seconds < 0) return 0;  // dark

    // HH:MM:SS, counting whole seconds. The countdown never exceeds 5 (spec 7.6), so the hours and
    // minutes are zero throughout — but the digits are laid out and driven as a full clock anyway,
    // because the board is a clock and a display that only worked below ten would be a display
    // with a bug waiting in it.
    const int hours   = (seconds / 3600) % 100;
    const int minutes = (seconds / 60) % 60;
    const int secs    = seconds % 60;

    const int digits[6] = {hours / 10,   hours % 10, minutes / 10,
                           minutes % 10, secs / 10,  secs % 10};

    static const int kDigitGlyph[6] = {0, 1, 3, 4, 6, 7};

    uint64_t mask = 0;
    for (int i = 0; i < 6; ++i) {
        const uint8_t pattern = DigitSegments(digits[i]);
        for (int s = 0; s < kSegmentsPerGlyph; ++s) {
            if (pattern & (1u << s)) {
                mask |= 1ull << (kDigitGlyph[i] * kSegmentsPerGlyph + s);
            }
        }
    }

    // The colons are lit whenever the board is.
    for (int glyph : {2, 5}) {
        mask |= 1ull << (glyph * kSegmentsPerGlyph + 0);
        mask |= 1ull << (glyph * kSegmentsPerGlyph + 1);
    }

    return mask;
}

Board GenerateBoard(uint64_t seed, const City& city) {
    core::Rng rng = core::Rng(seed).Fork(0xB0A2Dull);

    Board board;

    // Yaw is fixed at cycle start and held (spec 7.6). Offset per cycle so the camera does not
    // always meet a face head-on at the same moment.
    board.yaw = rng.Range(0.0f, core::kPi * 0.5f);

    // Sized by width first, then the glyph height falls out of it. The other way round — picking a
    // glyph height comparable to the tallest building and multiplying by eight glyphs — gives a
    // board wider than the city it stands in.
    const float faceWidth = city.params.radius * 0.80f;

    float widthInHeights = 0.0f;
    for (int g = 0; g < kGlyphCount; ++g) {
        widthInHeights += IsColon(g) ? kColonWidth : kDigitWidth;
        if (g + 1 < kGlyphCount) widthInHeights += kGapWidth;
    }

    board.glyphHeight = faceWidth / widthInHeights;
    board.width       = faceWidth;

    // The band straddles the skyline: the digits start below the tallest roofs and finish above
    // them, so the city passes in front of them as the camera orbits rather than standing clear.
    board.bandBottom = city.tallest * 0.55f;
    board.bandTop    = board.bandBottom + board.glyphHeight;

    board.faceColor = core::Albedo(core::palette::kBoardFrame, seed, 0, 0);

    // An unlit segment must read as a dark recessed bar, not vanish (spec 7.6). Darkened from the
    // frame colour rather than drawn separately, so the bar is visibly a bar against the face
    // behind it whatever the frame happened to draw.
    board.barColor = core::SrgbToLinear(core::HsvToRgb(
        core::Darken(core::AlbedoHsv(core::palette::kBoardFrame, seed, 1, 1), 0.45f)));

    board.litColor = core::Albedo(core::palette::kSegmentLit, seed, 2, 0);

    // Spec 6.5: after the final building, so the eye is left on it going into phase 2.
    board.riseDuration = rng.Range(0.9f, 1.4f);
    board.riseStart    = city.growthEnds;

    const float half = faceWidth * 0.5f;

    const float pad        = board.glyphHeight * kRecessPad;
    const float panelHalfD = board.glyphHeight * kPanelDepth * 0.5f;
    const float barHalfD   = board.glyphHeight * kBarDepth * 0.5f;
    const float railThick  = board.glyphHeight * kRailThick;

    // Four identical faces on a square mast, one per compass quadrant (spec 7.6). Each glyph gets
    // its own dark recess rather than the whole face getting one panel: spec 7.6 forbids a heavy
    // frame, and a single slab 580 m across is a billboard with numbers on it, which is the one
    // thing the board must not look like.
    for (int face = 0; face < 4; ++face) {
        const float yaw = board.yaw + core::kPi * 0.5f * static_cast<float>(face);
        const float c   = std::cos(yaw);
        const float s   = std::sin(yaw);

        // The box's own frame, exactly as the vertex shader rotates it: local +x lands on (c, s)
        // and local +z on (-s, c). Placement here MUST use that same basis. Mirroring it — the
        // obvious (c, -s) / (s, c) pair — leaves each face's panels pointing one way while its
        // bars march off along a different line, half of them ending up behind the face.
        const Vec2 across{c, s};
        const Vec2 outward{-s, c};

        // The lattice the glyphs hang from: two slender rails, one under the band and one over it.
        for (int rail = 0; rail < 2; ++rail) {
            BoardBox bar;
            bar.center     = Vec2{outward.x * half, outward.y * half};
            bar.base       = rail == 0 ? board.bandBottom - pad - railThick : board.bandTop + pad;
            bar.height     = railThick;
            bar.halfExtent = Vec2{half, panelHalfD};
            bar.yaw        = yaw;
            board.boxes.push_back(bar);
        }

        // The glyphs. Laid out in the face's own frame and then rotated out, so the layout code
        // never has to think about which way the face points.
        float u = 0.0f;
        for (int g = 0; g < kGlyphCount; ++g) {
            const float glyphWidth = GlyphWidth(g, board.glyphHeight);

            // The recess this glyph's bars sit in. Without it an unlit segment has nothing to be
            // dark against, and the numeral becomes whatever the sky behind it happens to be.
            {
                const float offset = u + glyphWidth * 0.5f - faceWidth * 0.5f;

                BoardBox recess;
                recess.center     = Vec2{across.x * offset + outward.x * half,
                                         across.y * offset + outward.y * half};
                recess.base       = board.bandBottom - pad;
                recess.height     = board.glyphHeight + pad * 2.0f;
                recess.halfExtent = Vec2{glyphWidth * 0.5f, panelHalfD};
                recess.yaw        = yaw;
                board.boxes.push_back(recess);
            }

            const int barCount = IsColon(g) ? 2 : kSegmentsPerGlyph;

            for (int sIdx = 0; sIdx < barCount; ++sIdx) {
                const Bar bar = IsColon(g) ? ColonBar(sIdx, u, board.glyphHeight)
                                           : DigitBar(sIdx, u, board.glyphHeight);
                if (bar.halfU <= 0.0f || bar.halfV <= 0.0f) continue;

                // u runs from the left edge of the face; convert to an offset from its centre.
                const float offset = bar.u - faceWidth * 0.5f;
                const float depth  = half + panelHalfD + barHalfD;

                BoardBox box;
                box.center = Vec2{across.x * offset + outward.x * depth,
                                  across.y * offset + outward.y * depth};
                box.base       = board.bandBottom + bar.v - bar.halfV;
                box.height     = bar.halfV * 2.0f;
                box.halfExtent = Vec2{bar.halfU, barHalfD};
                box.yaw        = yaw;
                box.segmentId  = g * kSegmentsPerGlyph + sIdx;
                board.boxes.push_back(box);
            }

            u += glyphWidth + kGapWidth * board.glyphHeight;
        }
    }

    // The mast. Four thin uprights at the corners of the square, from the ground to the top rail.
    // Spec 7.6 wants the structure minimal: a heavy frame turns monumental typography back into a
    // billboard, so this is all there is holding the board up.
    {
        const float c = std::cos(board.yaw);
        const float s = std::sin(board.yaw);

        const Vec2 right{c, s};
        const Vec2 fwd{-s, c};

        const float legHalf = board.glyphHeight * 0.055f;

        for (int i = 0; i < 4; ++i) {
            const float su = (i & 1) ? 1.0f : -1.0f;
            const float sv = (i & 2) ? 1.0f : -1.0f;

            BoardBox leg;
            leg.center     = Vec2{right.x * half * su + fwd.x * half * sv,
                                  right.y * half * su + fwd.y * half * sv};
            leg.base       = 0.0f;
            leg.height     = board.bandTop + pad + railThick;
            leg.halfExtent = Vec2{legHalf, legHalf};
            leg.yaw        = board.yaw;
            board.boxes.push_back(leg);
        }
    }

    app::Log("board: glyph %.0fm, face %.0fm wide, band %.0f-%.0fm, %zu boxes, rises at %.1fs",
             board.glyphHeight, board.width, board.bandBottom, board.bandTop, board.boxes.size(),
             board.riseStart);

    return board;
}

}  // namespace world
