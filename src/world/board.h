// The countdown board (spec 7.6).
//
// A physical object in the world, not an overlay: it is built with the city, it lights the city,
// and the blast takes it apart with everything else. The reference is diegetic film typography —
// numerals standing among the buildings at the scale of the buildings, lit by the scene's own
// light, with the city passing in front of them as the camera moves. Partial occlusion is the
// effect, not a defect.
//
// Everything here is a box. The masts, the four dark face panels and all the segment bars use the
// same unit cube the city does, which means the whole board is one instanced draw and the segment
// geometry is geometry rather than a texture, as spec 7.6 requires. Segments are generated from
// the digit value — there is no font, no atlas and no glyph table beyond the seven-bit patterns,
// which is the same constraint spec 6.1 puts on everything else.
#ifndef NUKE_SAVER_WORLD_BOARD_H
#define NUKE_SAVER_WORLD_BOARD_H

#include "core/math.h"
#include "world/city.h"

#include <cstdint>
#include <vector>

namespace world {

// Eight glyph positions reading HH:MM:SS. Positions 2 and 5 are the colons; the other six are
// digits. Nothing else appears on the board — no name, no marking (spec 7.6).
constexpr int kGlyphCount = 8;

// Seven segments per glyph, so a segment is addressed by glyph * 7 + index and the whole board's
// lit state fits in 56 bits. That is what lets the four faces be driven from one push constant and
// therefore update in the same frame, which spec 7.6 requires.
constexpr int kSegmentsPerGlyph = 7;
constexpr int kSegmentIdCount   = kGlyphCount * kSegmentsPerGlyph;

static_assert(kSegmentIdCount <= 64, "the lit mask must fit in 64 bits");

// Segment order is the conventional one: a top, b top-right, c bottom-right, d bottom, e
// bottom-left, f top-left, g middle.
uint8_t DigitSegments(int digit);

// Which segments are lit for a whole-second countdown value, as a mask over segment ids. `seconds`
// below zero means the board is dark. The colons are lit whenever the board is.
uint64_t BoardMask(int seconds);

struct BoardBox {
    core::Vec2 center{};      // world XZ of the box's footprint centre
    float      base   = 0.0f;  // world Y of its underside
    float      height = 0.0f;
    core::Vec2 halfExtent{};   // in the box's own frame, before yaw

    float yaw = 0.0f;

    // Which segment this box is, or -1 for structure that never lights. Structure and segments
    // share the instance stream so the whole board is one draw and cannot tear between them.
    int segmentId = -1;
};

struct Board {
    float yaw = 0.0f;  // fixed at cycle start and held; the board never billboards (spec 7.6)

    float glyphHeight = 0.0f;
    float width       = 0.0f;  // across one face
    float bandBottom  = 0.0f;  // world Y of the bottom of the glyph band
    float bandTop     = 0.0f;

    // Spec 6.5: the board rises last, after the final building.
    float riseStart    = 0.0f;  // seconds into the growth phase
    float riseDuration = 0.0f;

    core::Vec3 faceColor{};  // the dark recess the bars sit in
    core::Vec3 barColor{};   // an unlit bar: visible, not invisible (spec 7.6)
    core::Vec3 litColor{};   // linear, before the 30-60 emissive scale of spec 5.1

    std::vector<BoardBox> boxes;
};

Board GenerateBoard(uint64_t seed, const City& city);

}  // namespace world

#endif
