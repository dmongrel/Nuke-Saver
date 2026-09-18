// The particle systems (spec 8.3).
//
// What can be checked here is the half of the design the CPU owns: the slot budgets, and the
// emission schedule. The schedule is the part worth testing. Its whole job is to make the live
// count land where spec 8.3's table says without anything counting anything, and the ways it can
// be wrong are quiet ones — a slot holding two particles at once, a system that never fills, a
// trail that vanishes the instant the missile does. None of those show up as an error; they show
// up as a field that looks slightly thin, which is exactly the kind of thing nobody catches by
// looking at it.
//
// The particle's *motion* is closed-form GLSL and is not reachable from here. It is checked by
// capture, in the images the milestone was verified against.

#include "render/particle_data.h"
#include "selftest_check.h"
#include "world/phase.h"

#include <cmath>
#include <cstdio>

namespace selftest {
namespace {

using render::kParticleLife;
using render::kParticlePeak;
using render::kParticleSystemCount;
using render::ParticleCapacity;
using render::ParticleLiveCount;
using render::ParticleSlotAge;
using render::ParticleTotal;

// Small enough to sweep exhaustively, large enough that the scheduling arithmetic is not trivially
// exact. Nothing about the schedule depends on the count being a round number.
constexpr uint32_t kSlots = 997;

}  // namespace

void TestParticles() {
    std::printf("particles (spec 8.3)\n");

    // Spec 8.3's table, at the top quality level, exactly.
    const uint32_t expected[kParticleSystemCount] = {15000, 30000, 20000, 8000};
    bool           peaksMatch                     = true;
    for (int i = 0; i < kParticleSystemCount; ++i) {
        if (ParticleCapacity(i, 0) != expected[i]) peaksMatch = false;
    }
    Check(peaksMatch, "the four peak counts are spec 8.3's table");
    Check(ParticleTotal(0) == 73000, "73,000 slots in total at the top level");

    // Spec 11.2 gives up particle counts as quality falls, and it has to be a real ladder: a
    // level that costs the same as the one above it buys the frame-time controller nothing.
    bool descends = true;
    for (int q = 1; q < 4; ++q) {
        if (ParticleTotal(q) >= ParticleTotal(q - 1)) descends = false;
    }
    Check(descends, "every quality level costs strictly fewer particles than the one above");
    Check(ParticleTotal(3) * 4 < ParticleTotal(0),
          "the bottom level is at least four times cheaper than the top");

    // Out-of-range indices are asked for by nothing, which is why they are worth pinning: a
    // silently wrong answer here would be a silently undersized buffer.
    Check(ParticleCapacity(-1, 0) == 0 && ParticleCapacity(kParticleSystemCount, 0) == 0,
          "a system outside the four has no slots");


    const float t0   = 10.0f;
    const float t1   = 30.0f;
    const float life = 4.0f;

    // Nothing exists before the window opens. No system emits before the missile does, and the
    // phases in front of that are specified as bare desert and then a city standing on it.
    Check(ParticleLiveCount(kSlots, t0, t1, life, t0 - 0.001f) == 0,
          "no particle is alive before the emission window opens");

    // At steady state the count is the whole system: one generation of every slot, which is the
    // property the whole scheme exists to give.
    const uint32_t steady = ParticleLiveCount(kSlots, t0, t1, life, t0 + life + 3.0f);
    Check(steady == kSlots, "at steady state every slot holds exactly one particle");

    // And it never exceeds it. A slot holding two generations at once would be two particles
    // drawn from one index, which the draw has no way to express and the sort no way to order.
    bool neverOver  = true;
    bool ageInRange = true;
    for (int step = 0; step <= 400; ++step) {
        const float t = t0 - 1.0f + static_cast<float>(step) * 0.1f;
        if (ParticleLiveCount(kSlots, t0, t1, life, t) > kSlots) neverOver = false;

        for (uint32_t slot = 0; slot < kSlots; slot += 37) {
            const render::SlotBirth b = ParticleSlotAge(slot, kSlots, t0, t1, life, t);
            if (b.alive && (b.age < 0.0f || b.age >= life)) ageInRange = false;
        }
    }
    Check(neverOver, "no moment has more live particles than the system has slots");
    Check(ageInRange, "a live particle's age is always inside its lifetime");

    // A slot's age advances with the clock and resets rather than jumping: between two samples it
    // either grew by the step or the slot was reborn. An age that drifted would be a particle
    // that moved at the wrong speed, which is the failure spec 4.2 rules out.
    bool continuous = true;
    for (int step = 0; step < 2000; ++step) {
        const float dt = 0.01f;
        const float ta = t0 + 0.5f + static_cast<float>(step) * dt;
        const render::SlotBirth a = ParticleSlotAge(11, kSlots, t0, t1, life, ta);
        const render::SlotBirth b = ParticleSlotAge(11, kSlots, t0, t1, life, ta + dt);
        if (a.alive && b.alive && b.generation == a.generation &&
            std::fabs((b.age - a.age) - dt) > 1e-3f) {
            continuous = false;
        }
    }
    Check(continuous, "a slot's age advances exactly with the clock");

    // Generations increase and never go back. A slot that reused an old generation would give the
    // same particle the same random draw twice in a row, which reads as a blink rather than as a
    // new particle.
    bool monotoneGeneration = true;
    uint32_t last           = 0;
    bool     seen           = false;
    for (int step = 0; step <= 2000; ++step) {
        const float t = t0 + static_cast<float>(step) * 0.01f;
        const render::SlotBirth b = ParticleSlotAge(5, kSlots, t0, t1, life, t);
        if (!b.alive) continue;
        if (seen && b.generation < last) monotoneGeneration = false;
        last = b.generation;
        seen = true;
    }
    Check(monotoneGeneration, "a slot's generation never goes backwards");

    // The window closes, and the last particles born inside it live out their lives rather than
    // being cut off. This is spec 7.1's "smoke trail persisting a few seconds behind it": if the
    // trail died with the emitter, the missile's path would vanish at the instant of impact.
    Check(ParticleLiveCount(kSlots, t0, t1, life, t1 + life * 0.5f) > 0,
          "particles outlive the emitter that stopped");
    Check(ParticleLiveCount(kSlots, t0, t1, life, t1 + life + 0.01f) == 0,
          "and are all gone one lifetime after emission stops");

    // A window shorter than a lifetime cannot fill the system, and must not pretend to. This is
    // the missile's case: phase 4 can be as short as 2.5 s and the trail lasts 6.
    const uint32_t shortWindow = ParticleLiveCount(kSlots, 0.0f, 2.0f, 8.0f, 2.0f);
    Check(shortWindow > 0 && shortWindow < kSlots,
          "a window shorter than one lifetime fills the system only in proportion");

    // Degenerate inputs are what a phase of zero length hands this, and spec 4.1's timeline can
    // produce one for a phase it has compressed.
    Check(!ParticleSlotAge(0, kSlots, t0, t0, life, t0).alive,
          "an empty emission window emits nothing");
    Check(!ParticleSlotAge(0, 0, t0, t1, life, t0 + 1.0f).alive,
          "a system with no slots emits nothing");
    Check(!ParticleSlotAge(0, kSlots, t0, t1, 0.0f, t0 + 1.0f).alive,
          "a zero lifetime emits nothing");


    // Each system's window is a phase, and the phases are drawn per cycle. The check that matters
    // is that no system is scheduled into a window the timeline can make empty, because that is a
    // system that would silently not exist for that seed.
    bool everySystemRuns = true;
    for (uint64_t seed = 1; seed <= 64; ++seed) {
        const world::Timeline tl = world::Timeline::Create(seed * 7919u);

        const float windows[kParticleSystemCount][2] = {
            {tl.Start(world::Phase::Missile), tl.End(world::Phase::Missile)},
            {tl.Start(world::Phase::Blast), tl.Start(world::Phase::Blast) +
                                                tl.Duration(world::Phase::Blast) * 0.55f},
            {tl.Start(world::Phase::Blast) + tl.Duration(world::Phase::Blast) * 0.6f,
             tl.End(world::Phase::Disperse)},
            {tl.Start(world::Phase::Gather), tl.End(world::Phase::Disperse)},
        };

        for (int i = 0; i < kParticleSystemCount; ++i) {
            const float open = windows[i][0];
            const float shut = windows[i][1];
            if (shut <= open) {
                everySystemRuns = false;
                continue;
            }
            // Sampled in the middle of its own window, where every system must have something.
            const float mid = open + (shut - open) * 0.5f;
            if (ParticleLiveCount(64, open, shut, kParticleLife[i], mid) == 0) {
                everySystemRuns = false;
            }
        }
    }
    Check(everySystemRuns,
          "all four systems have a non-empty window and live particles in it, "
          "for sixty-four seeds");

    // The trail has to still be there when the fireball lights it, or spec 7.1's persistence buys
    // nothing: the one frame it matters in is the one just after impact.
    bool trailSurvivesImpact = true;
    for (uint64_t seed = 1; seed <= 64; ++seed) {
        const world::Timeline tl      = world::Timeline::Create(seed * 104729u);
        const float           open    = tl.Start(world::Phase::Missile);
        const float           shut    = tl.End(world::Phase::Missile);
        const float           atFlash = tl.End(world::Phase::Flash);
        if (ParticleLiveCount(256, open, shut, kParticleLife[0], atFlash) == 0) {
            trailSurvivesImpact = false;
        }
    }
    Check(trailSurvivesImpact,
          "the missile's trail is still in the air when the flash ends (spec 7.1)");
}

}  // namespace selftest
