// Deterministic randomness (implementation plan, M3).
//
// Spec 5.2 requires that every per-instance random be derived from the instance ID and the cycle
// seed, "so it is stable for the whole cycle and reproducible across runs". That rules out a
// sequential generator for anything per-instance: draw order would then decide the values, and
// adding one building would reshuffle the colour of every building after it.
//
// So there are two things here, and the distinction matters:
//   - Rng, a sequential stream, for generation that is genuinely a sequence (choosing road
//     angles, subdividing lots).
//   - Hash*, stateless functions from (seed, id, channel) to a value, for anything an instance
//     must be able to recompute on its own — including in a shader, which has no stream.
#ifndef NUKE_SAVER_CORE_RNG_H
#define NUKE_SAVER_CORE_RNG_H

#include "core/math.h"

#include <cstdint>

namespace core {

// SplitMix64. Chosen because it is a mixing function as much as a generator: it scrambles a
// counter well enough to be used statelessly, which is exactly what per-instance values need.
inline uint64_t SplitMix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// A stable value for (seed, id, channel). `channel` separates independent draws for one
// instance — hue, saturation and value must not be correlated, and spec 5.2 asks for them
// "each drawn independently".
inline uint64_t HashCombine(uint64_t seed, uint64_t id, uint64_t channel) {
    return SplitMix64(SplitMix64(seed ^ (id * 0xD6E8FEB86659FD93ull)) ^
                      (channel * 0xA24BAED4963EE407ull));
}

// Uniform in [0,1). 24 bits of mantissa is all a float can hold, so taking the top 24 bits
// avoids the rounding-to-1.0 that dividing a full 32-bit draw by 2^32 can produce.
inline float ToUnitFloat(uint64_t bits) {
    return static_cast<float>((bits >> 40) & 0xFFFFFFull) * (1.0f / 16777216.0f);
}

inline float HashFloat(uint64_t seed, uint64_t id, uint64_t channel) {
    return ToUnitFloat(HashCombine(seed, id, channel));
}

// Uniform in [lo,hi).
inline float HashRange(uint64_t seed, uint64_t id, uint64_t channel, float lo, float hi) {
    return lo + (hi - lo) * HashFloat(seed, id, channel);
}

// Uniform in [-1,1]. The shape spec 5.2 wants for a +/-x% offset.
inline float HashSigned(uint64_t seed, uint64_t id, uint64_t channel) {
    return HashFloat(seed, id, channel) * 2.0f - 1.0f;
}

class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed ? seed : 0x123456789ABCDEFull) {}

    uint64_t NextU64() {
        state_ += 0x9E3779B97F4A7C15ull;
        uint64_t z = state_;
        z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z          = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    float Unit() { return ToUnitFloat(NextU64()); }
    float Range(float lo, float hi) { return lo + (hi - lo) * Unit(); }
    float Signed() { return Unit() * 2.0f - 1.0f; }

    // Uniform in [0,count). Modulo bias is irrelevant at the magnitudes here (counts in the
    // hundreds against a 64-bit draw) and not worth a rejection loop.
    uint32_t Index(uint32_t count) {
        return count ? static_cast<uint32_t>(NextU64() % count) : 0u;
    }

    bool Chance(float p) { return Unit() < p; }

    // A direction on the unit sphere, correctly area-weighted. Sampling Euler angles uniformly
    // clusters at the poles, which shows up as a seam in anything using this for scatter.
    Vec3 UnitVector() {
        const float z     = Signed();
        const float theta = Unit() * kTwoPi;
        const float r     = std::sqrt(Clamp(1.0f - z * z, 0.0f, 1.0f));
        return {r * std::cos(theta), r * std::sin(theta), z};
    }

    // Derives an independent stream. Lets one generation step hand a sub-generator to another
    // without the two consuming from the same sequence, which is what makes generation order
    // stop mattering.
    Rng Fork(uint64_t tag) const { return Rng(SplitMix64(state_ ^ (tag * 0x9E3779B97F4A7C15ull))); }

private:
    uint64_t state_;
};

}  // namespace core

#endif
