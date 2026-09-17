#include "core/noise.h"

namespace core {
namespace {

// Quintic fade. Perlin's original cubic has a discontinuous second derivative, which shows up as
// creasing once the field drives a normal — and terrain here is normal-mapped.
inline float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// A unit gradient for an integer lattice point. Derived by hashing rather than indexed from a
// permutation table: no table to carry, and the seed genuinely changes the field.
inline Vec2 LatticeGradient(int ix, int iy, uint64_t seed) {
    const uint64_t h = HashCombine(seed, (static_cast<uint64_t>(static_cast<uint32_t>(ix)) << 32) ^
                                             static_cast<uint32_t>(iy),
                                   0x6772);
    const float angle = ToUnitFloat(h) * kTwoPi;
    return {std::cos(angle), std::sin(angle)};
}

inline float DotGrid(int ix, int iy, float dx, float dy, uint64_t seed) {
    const Vec2 g = LatticeGradient(ix, iy, seed);
    return g.x * dx + g.y * dy;
}

}  // namespace

float GradientNoise2(float x, float y, uint64_t seed) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int   ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    const float dx = x - fx, dy = y - fy;

    const float u = Fade(dx), v = Fade(dy);

    const float n00 = DotGrid(ix, iy, dx, dy, seed);
    const float n10 = DotGrid(ix + 1, iy, dx - 1.0f, dy, seed);
    const float n01 = DotGrid(ix, iy + 1, dx, dy - 1.0f, seed);
    const float n11 = DotGrid(ix + 1, iy + 1, dx - 1.0f, dy - 1.0f, seed);

    const float a = Lerp(n00, n10, u);
    const float b = Lerp(n01, n11, u);

    // The sqrt(2) scale brings the theoretical range of 2D gradient noise to about [-1,1].
    return Lerp(a, b, v) * 1.41421356f;
}

float Fbm2(float x, float y, uint64_t seed, const FbmParams& p) {
    float sum = 0.0f, norm = 0.0f;
    float freq = p.frequency, amp = p.amplitude;

    for (int i = 0; i < p.octaves; ++i) {
        // Each octave gets its own gradient field. Sharing one field across octaves correlates
        // them, which flattens the result and wastes the octaves.
        sum += GradientNoise2(x * freq, y * freq, seed + static_cast<uint64_t>(i) * 0x9E37ull) * amp;
        norm += amp;
        freq *= p.lacunarity;
        amp *= p.gain;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

float Ridged2(float x, float y, uint64_t seed, const FbmParams& p) {
    float sum = 0.0f, norm = 0.0f;
    float freq = p.frequency, amp = p.amplitude;

    for (int i = 0; i < p.octaves; ++i) {
        float n = GradientNoise2(x * freq, y * freq, seed + static_cast<uint64_t>(i) * 0x9E37ull);
        n       = 1.0f - std::fabs(n);
        n *= n;  // sharpens the crest and flattens the valley floor

        sum += n * amp;
        norm += amp;
        freq *= p.lacunarity;
        amp *= p.gain;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

float FbmMaxFrequency(const FbmParams& p) {
    float f = p.frequency;
    for (int i = 1; i < p.octaves; ++i) f *= p.lacunarity;
    return f > 0.0f ? f : 1.0f;
}

float Curl2Step(const FbmParams& p) { return 0.25f / FbmMaxFrequency(p); }

Vec2 Curl2(float x, float y, uint64_t seed, const FbmParams& p, float step) {
    // Too small and the difference drowns in float cancellation; too large and the curl smooths
    // away the detail it exists for. A quarter-wavelength of the finest octave is the balance.
    const float e = step > 0.0f ? step : Curl2Step(p);

    // The perpendicular of the gradient. This, not the numerics, is what makes the field
    // divergence-free: div(curl) is identically zero for any scalar potential.
    const float dFdy = (Fbm2(x, y + e, seed, p) - Fbm2(x, y - e, seed, p)) / (2.0f * e);
    const float dFdx = (Fbm2(x + e, y, seed, p) - Fbm2(x - e, y, seed, p)) / (2.0f * e);

    return {dFdy, -dFdx};
}

}  // namespace core
