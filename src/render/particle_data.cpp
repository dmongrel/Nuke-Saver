#include "render/particle_data.h"

#include <cmath>

namespace render {
namespace {

// Spec 11.2's ladder for particle counts. Four levels, matching the four fragment budgets, and
// the same order: high, medium, low, minimum.
constexpr float kQualityScale[4] = {1.0f, 0.65f, 0.40f, 0.22f};

int ClampQuality(int quality) { return quality < 0 ? 0 : (quality > 3 ? 3 : quality); }

}  // namespace

uint32_t ParticleCapacity(int system, int quality) {
    if (system < 0 || system >= kParticleSystemCount) return 0;
    const float scale = kQualityScale[ClampQuality(quality)];
    return static_cast<uint32_t>(static_cast<float>(kParticlePeak[system]) * scale);
}

uint32_t ParticleTotal(int quality) {
    uint32_t total = 0;
    for (int i = 0; i < kParticleSystemCount; ++i) total += ParticleCapacity(i, quality);
    return total;
}

SlotBirth ParticleSlotAge(uint32_t slot, uint32_t slots, float t0, float t1, float life, float t) {
    SlotBirth out;
    if (slots == 0 || t1 <= t0 || life <= 0.0f) return out;

    // Births per second across the whole system, chosen so that at steady state exactly one
    // generation of each slot is alive: a slot comes back `life` seconds after it was last born.
    const float rate  = static_cast<float>(slots) / life;
    const float count = static_cast<float>(slots);
    const float j     = static_cast<float>(slot);

    // The last generation of this slot whose birth still falls inside the emission window. Past
    // it the slot is finished, but the particle it last produced lives out its whole life — which
    // is what makes the missile's trail persist after the missile has gone (spec 7.1).
    const float lastGen = std::floor(((t1 - t0) * rate - j) / count);
    if (lastGen < 0.0f) return out;

    const float generation = std::fmin(std::floor(((t - t0) * rate - j) / count), lastGen);
    if (generation < 0.0f) return out;

    const float birth = t0 + (j + generation * count) / rate;
    const float age   = t - birth;
    if (age < 0.0f || age >= life) return out;

    out.alive      = true;
    out.age        = age;
    out.generation = static_cast<uint32_t>(generation);
    return out;
}

uint32_t ParticleLiveCount(uint32_t slots, float t0, float t1, float life, float t) {
    uint32_t live = 0;
    for (uint32_t i = 0; i < slots; ++i) {
        if (ParticleSlotAge(i, slots, t0, t1, life, t).alive) ++live;
    }
    return live;
}

}  // namespace render
