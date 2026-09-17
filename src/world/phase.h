// The run cycle (spec 4).
//
// Eleven phases, drawn once per cycle from the seed, and after that a pure function of elapsed
// seconds. Nothing here accumulates: spec 4.2 requires every animated quantity to be a function of
// the clock rather than of frames, so this answers questions about a time rather than being
// stepped forward.
//
// Phases 6 and 7 overlap by design — the blast front is still expanding while the nearest
// triangles are already flying — so this cannot be a single "which phase is it" lookup. Each phase
// has its own interval, and callers ask whether the one they care about is running.
#ifndef NUKE_SAVER_WORLD_PHASE_H
#define NUKE_SAVER_WORLD_PHASE_H

#include <cstdint>

namespace world {

enum class Phase {
    EmptyLand,  // 0: bare desert, camera already moving
    Growth,     // 1: 500 buildings rise, board last
    Settle,     // 2: the finished city, still, board dark
    Countdown,  // 3: exactly 5 s, board lit, 00:00:05 to 00:00:00
    Missile,    // 4: missile runs in, board holds at zero
    Flash,      // 5: white
    Blast,      // 6: refractive shell expands, overlaps Scatter
    Scatter,    // 7: triangles fly out and settle
    Gather,     // 8: triangles drawn back and lifted into the cloud
    Disperse,   // 9: the cloud lets go
    Fade,       // 10: to black over the debris
    Count
};

constexpr int kPhaseCount = static_cast<int>(Phase::Count);

class Timeline {
public:
    // Drawn from the seed, so a cycle is reproducible (spec 4.2).
    static Timeline Create(uint64_t seed);

    float Start(Phase p) const { return start_[static_cast<int>(p)]; }
    float End(Phase p) const { return end_[static_cast<int>(p)]; }
    float Duration(Phase p) const { return End(p) - Start(p); }

    bool Active(Phase p, float t) const { return t >= Start(p) && t < End(p); }

    // 0 before the phase, 1 after it, linear within. Every animation in the project is driven
    // from one of these rather than from a timer of its own, which is what keeps the whole cycle
    // reproducible from a seed and a clock reading.
    float Progress(Phase p, float t) const;

    // Seconds into the phase, clamped to its length. Used where the absolute count matters rather
    // than the fraction — the countdown steps on whole seconds.
    float Elapsed(Phase p, float t) const;

    // The latest phase that has started at `t`. For display and logging; anything that has to
    // behave correctly across the blast overlap must use Active() instead.
    Phase Primary(float t) const;

    float total() const { return total_; }

    // The digits the board shows at `t`, as whole seconds remaining on the countdown: 5 down to 0
    // through phase 3, then held at 0 until the flash. Negative means the board is dark.
    int BoardSeconds(float t) const;

private:
    float start_[kPhaseCount]{};
    float end_[kPhaseCount]{};
    float total_ = 0.0f;
};

const char* PhaseName(Phase p);

}  // namespace world

#endif
