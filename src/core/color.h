// The colour system (spec section 5).
//
// Spec 5.1 requires that everything be computed in linear scene-referred space, with sRGB
// encoding happening once in the tonemap pass. The palette in 5.3, though, is authored in HSV
// with values like "val 0.35-0.55" — and that is a display-referred way of describing a colour;
// nobody picks a brown by naming its linear intensity.
//
// So there is exactly one conversion, and it lives here: palette HSV is treated as sRGB-encoded,
// turned into RGB, and decoded to linear on the way out. Everything downstream of Albedo() is
// linear. Getting this wrong in one direction gives a washed-out desert and in the other a muddy
// one, and it is the gamma bug the implementation plan warns about, so it is done in one place
// where it can be read.
#ifndef NUKE_SAVER_CORE_COLOR_H
#define NUKE_SAVER_CORE_COLOR_H

#include "core/math.h"
#include "core/rng.h"

namespace core {

// Hue in degrees 0-360, saturation and value in 0-1.
struct Hsv {
    float h = 0.0f, s = 0.0f, v = 0.0f;
};

// A palette entry from spec 5.3: each channel is a range the instance draws from, plus the
// per-instance variation percentage applied on top.
struct ColorRange {
    float hueMin = 0.0f, hueMax = 0.0f;
    float satMin = 0.0f, satMax = 0.0f;
    float valMin = 0.0f, valMax = 0.0f;
    float variation = 0.10f;  // spec 5.2 default; windows use 0.15, lit segments 0.05
};

Vec3 HsvToRgb(const Hsv& hsv);
Hsv  RgbToHsv(const Vec3& rgb);

float SrgbToLinear(float c);
float LinearToSrgb(float c);
Vec3  SrgbToLinear(const Vec3& c);

// Draws this instance's colour: a base within the range, then the +/-variation offset of spec
// 5.2 applied in HSV. Returns **linear** RGB, ready to use as albedo.
//
// `channelBase` separates one palette slot from another for the same instance, so a building's
// body colour and its window colour are independent rather than locked together.
Vec3 Albedo(const ColorRange& range, uint64_t seed, uint64_t id, uint64_t channelBase = 0);

// The same draw, but returned as HSV before conversion. For the cases spec 5.3 defines
// relative to another colour - settled dust is the floor "desaturated 40%", fragment edges are
// the parent "darkened 15-30%" - which have to be expressed in HSV to mean anything.
Hsv AlbedoHsv(const ColorRange& range, uint64_t seed, uint64_t id, uint64_t channelBase = 0);

// The same draw with the six unit values supplied by the caller, in the order hue, saturation,
// value, then the three offsets. For surfaces whose variation should follow the world rather than
// the mesh: a per-vertex hash makes the *mesh* the thing being coloured, and on a radial grid seen
// at a grazing angle that paints the grid — the desert came out as a fine speckle converging on
// the camera, which is the topology, not the ground. Feeding these from a smooth spatial field
// gives the variation a scale in metres instead.
Vec3 AlbedoFrom(const ColorRange& range, const float draw[6]);
Hsv  AlbedoHsvFrom(const ColorRange& range, const float draw[6]);

// Spec 5.3 derivations, kept here so the numbers live next to the palette they modify.
Hsv Desaturate(const Hsv& c, float amount);
Hsv Darken(const Hsv& c, float amount);

// The palette of spec 5.3.
namespace palette {
extern const ColorRange kDesertFloor;
extern const ColorRange kRock;
extern const ColorRange kBuildingBody;
extern const ColorRange kBuildingWindow;
extern const ColorRange kBoardFrame;
extern const ColorRange kSegmentLit;
extern const ColorRange kMissileBody;
}  // namespace palette

}  // namespace core

#endif
