// The CPU half of the particle systems (spec 8.3).
//
// There is deliberately very little of it. A particle has no state: shaders/particle_common.glsl
// evaluates its position, size, colour and opacity as a closed-form function of its index and the
// cycle clock, so nothing is uploaded, nothing is stepped and nothing here owns a particle.
//
// What is here is the two things the CPU genuinely decides — how many slots each system gets at a
// given quality level, and the emission schedule those slots follow — and they are here rather
// than inside the renderer so that they can be tested. The schedule in particular is the subtle
// part of the design: it is what makes the live count settle where it should without anything
// counting anything, and it is worth a test that can fail.
//
// `ParticleSlotAge` mirrors `SlotAge` in shaders/particle_common.glsl. The two describe the same
// arithmetic and MUST be changed together; there is no way to share the code, exactly as with
// SceneUniforms and its GLSL twin.
#ifndef NUKE_SAVER_RENDER_PARTICLE_DATA_H
#define NUKE_SAVER_RENDER_PARTICLE_DATA_H

#include <cstdint>

namespace render {

constexpr int kParticleSystemCount = 4;

// Spec 8.3's peak live counts, in the order of its table: missile exhaust and trail, ground
// collar dust, settled dust, embers.
//
// There is nothing for phase 1. An earlier version kicked dust up under the rising city, which
// made the growth read as construction rather than as the city simply being there -- and dust is
// what the back half of the cycle is made of, so spending it here cost the detonation its one
// visual idea.
constexpr uint32_t kParticlePeak[kParticleSystemCount] = {15000, 30000, 20000, 8000};

// Each system's particle lifetime in seconds, mirroring shaders/particle_common.glsl. System 0
// carries two populations with different lifetimes; the figure here is the trail's, because the
// trail is the one whose persistence spec 7.1 has an opinion about.
constexpr float kParticleLife[kParticleSystemCount] = {6.0f, 6.0f, 30.0f, 5.0f};

// Depth buckets for the back-to-front sort. Mirrors kSortBuckets in the shader.
constexpr uint32_t kSortBuckets = 256;

// Spec 11.2 sacrifices particle counts one rung below fragments per building. The buffers are
// always sized for level 0, so a change of quality is a different push constant and nothing else.
uint32_t ParticleCapacity(int system, int quality);
uint32_t ParticleTotal(int quality);

// Where one slot is in its cycle. `alive` false means this slot has no particle at `t`: either it
// has not had its first birth yet, or the emission window closed and its last one has expired.
struct SlotBirth {
    bool     alive      = false;
    float    age        = 0.0f;  // seconds since birth, always in [0, life)
    uint32_t generation = 0;     // how many times this slot has been reused
};

SlotBirth ParticleSlotAge(uint32_t slot, uint32_t slots, float t0, float t1, float life, float t);

// How many of a system's slots hold a live particle at `t`. Nothing in the renderer calls this —
// the GPU never needs the number — but it is the quantity the schedule exists to control, so it
// is what the tests measure.
uint32_t ParticleLiveCount(uint32_t slots, float t0, float t1, float life, float t);

}  // namespace render

#endif
