#include "core/color.h"

namespace core {

Vec3 HsvToRgb(const Hsv& hsv) {
    float h = std::fmod(hsv.h, 360.0f);
    if (h < 0.0f) h += 360.0f;

    const float s = Saturate(hsv.s);
    const float v = Saturate(hsv.v);

    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;

    Vec3 rgb;
    if (h < 60.0f)        rgb = {c, x, 0.0f};
    else if (h < 120.0f)  rgb = {x, c, 0.0f};
    else if (h < 180.0f)  rgb = {0.0f, c, x};
    else if (h < 240.0f)  rgb = {0.0f, x, c};
    else if (h < 300.0f)  rgb = {x, 0.0f, c};
    else                  rgb = {c, 0.0f, x};

    return {rgb.x + m, rgb.y + m, rgb.z + m};
}

Hsv RgbToHsv(const Vec3& rgb) {
    const float maxC = std::fmax(rgb.x, std::fmax(rgb.y, rgb.z));
    const float minC = std::fmin(rgb.x, std::fmin(rgb.y, rgb.z));
    const float d    = maxC - minC;

    Hsv out;
    out.v = maxC;
    out.s = maxC > 1e-6f ? d / maxC : 0.0f;

    if (d < 1e-6f) {
        out.h = 0.0f;  // achromatic: hue is meaningless, not zero-degrees-red
        return out;
    }

    if (maxC == rgb.x)      out.h = 60.0f * std::fmod((rgb.y - rgb.z) / d, 6.0f);
    else if (maxC == rgb.y) out.h = 60.0f * ((rgb.z - rgb.x) / d + 2.0f);
    else                    out.h = 60.0f * ((rgb.x - rgb.y) / d + 4.0f);

    if (out.h < 0.0f) out.h += 360.0f;
    return out;
}

float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

Vec3 SrgbToLinear(const Vec3& c) {
    return {SrgbToLinear(c.x), SrgbToLinear(c.y), SrgbToLinear(c.z)};
}

Hsv AlbedoHsvFrom(const ColorRange& r, const float draw[6]) {
    // Base draw within the range, then the offset. Six independent channels, because spec 5.2
    // asks for hue, saturation and value "each drawn independently" - and the base draw must be
    // independent of the offset too, or the two correlate and the spread collapses.
    float h = Lerp(r.hueMin, r.hueMax, draw[0]);
    float s = Lerp(r.satMin, r.satMax, draw[1]);
    float v = Lerp(r.valMin, r.valMax, draw[2]);

    // Hue varies by a fraction of the stated hue *range*, per spec 5.3: +/-10% of a hue value
    // would be meaningless (10% of 200 degrees is a different colour, 10% of 5 degrees is
    // nothing), whereas a fraction of the authored range means the same thing everywhere.
    // A degenerate range - the frame colour has no hue span - falls back to a small absolute
    // span so a grey still carries a trace of variation rather than being flat.
    const float hueSpan = (r.hueMax - r.hueMin) > 1e-3f ? (r.hueMax - r.hueMin) : 12.0f;
    h += hueSpan * r.variation * (draw[3] * 2.0f - 1.0f);

    // Saturation and value vary proportionally, which is what "+/-10%" means for a scalar.
    s *= 1.0f + r.variation * (draw[4] * 2.0f - 1.0f);
    v *= 1.0f + r.variation * (draw[5] * 2.0f - 1.0f);

    return {h, Saturate(s), Saturate(v)};
}

Vec3 AlbedoFrom(const ColorRange& r, const float draw[6]) {
    return SrgbToLinear(HsvToRgb(AlbedoHsvFrom(r, draw)));
}

Hsv AlbedoHsv(const ColorRange& r, uint64_t seed, uint64_t id, uint64_t channelBase) {
    const uint64_t ch      = channelBase * 8ull;
    const float    draw[6] = {HashFloat(seed, id, ch + 0), HashFloat(seed, id, ch + 1),
                              HashFloat(seed, id, ch + 2), HashFloat(seed, id, ch + 3),
                              HashFloat(seed, id, ch + 4), HashFloat(seed, id, ch + 5)};
    return AlbedoHsvFrom(r, draw);
}

Vec3 Albedo(const ColorRange& r, uint64_t seed, uint64_t id, uint64_t channelBase) {
    return SrgbToLinear(HsvToRgb(AlbedoHsv(r, seed, id, channelBase)));
}

Hsv Desaturate(const Hsv& c, float amount) {
    return {c.h, Saturate(c.s * (1.0f - Saturate(amount))), c.v};
}

Hsv Darken(const Hsv& c, float amount) {
    return {c.h, c.s, Saturate(c.v * (1.0f - Saturate(amount)))};
}

namespace palette {

// Spec 5.3, transcribed. Values here are display-referred HSV; Albedo() decodes to linear.
const ColorRange kDesertFloor{25.0f, 35.0f, 0.35f, 0.50f, 0.35f, 0.55f, 0.10f};
const ColorRange kRock{20.0f, 30.0f, 0.15f, 0.30f, 0.30f, 0.45f, 0.10f};
const ColorRange kBuildingBody{20.0f, 40.0f, 0.02f, 0.12f, 0.35f, 0.70f, 0.10f};
const ColorRange kBuildingWindow{195.0f, 215.0f, 0.10f, 0.30f, 0.25f, 0.60f, 0.15f};
const ColorRange kBoardFrame{0.0f, 0.0f, 0.00f, 0.08f, 0.10f, 0.20f, 0.10f};
// Tightened to +/-5%: eight glyphs of one display visibly mismatching reads as a bug.
const ColorRange kSegmentLit{5.0f, 20.0f, 0.85f, 1.00f, 0.90f, 1.00f, 0.05f};
const ColorRange kMissileBody{0.0f, 0.0f, 0.00f, 0.05f, 0.70f, 0.85f, 0.10f};

}  // namespace palette

}  // namespace core
