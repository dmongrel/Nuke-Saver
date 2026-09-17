#include "world/phase.h"

#include "app/log.h"
#include "core/math.h"
#include "core/rng.h"

namespace world {
namespace {

// How long after the blast front starts expanding the first triangles are already moving. Spec 4.1
// requires the two to overlap; this is how much.
constexpr float kBlastScatterOverlap = 0.8f;

// Spec 4.1 puts the cycle at 80 to 115 seconds.
constexpr float kCycleMin = 80.0f;
constexpr float kCycleMax = 115.0f;

}  // namespace

const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::EmptyLand: return "empty-land";
        case Phase::Growth:    return "growth";
        case Phase::Settle:    return "settle";
        case Phase::Countdown: return "countdown";
        case Phase::Missile:   return "missile";
        case Phase::Flash:     return "flash";
        case Phase::Blast:     return "blast";
        case Phase::Scatter:   return "scatter";
        case Phase::Gather:    return "gather";
        case Phase::Disperse:  return "disperse";
        case Phase::Fade:      return "fade";
        case Phase::Count:     break;
    }
    return "?";
}

Timeline Timeline::Create(uint64_t seed) {
    core::Rng rng = core::Rng(seed).Fork(0x7114E11Eull);

    // Spec 4.1, in order. The two fixed ones are fixed for a reason: phase 3 must run exactly
    // 5.000 s of wall clock or the countdown drifts against its own digits, and phase 7 is the
    // beat the scatter is choreographed around.
    float duration[kPhaseCount];
    duration[static_cast<int>(Phase::EmptyLand)] = rng.Range(5.0f, 8.0f);
    duration[static_cast<int>(Phase::Growth)]    = rng.Range(5.0f, 10.0f);
    duration[static_cast<int>(Phase::Settle)]    = rng.Range(2.0f, 3.0f);
    duration[static_cast<int>(Phase::Countdown)] = 5.0f;
    // Longer than the 2.5-4 s this used to be. Spec 7.1 now has the missile entering over the
    // mountains, kilometres further out than the old approach began, and at the old duration the
    // final stretch of the run -- the part the phase exists to show -- went past in well under a
    // second. It also moves the cycle length toward the 80-115 s spec 4.1 asks for rather than
    // away from it.
    duration[static_cast<int>(Phase::Missile)]   = rng.Range(4.0f, 6.0f);
    duration[static_cast<int>(Phase::Flash)]     = 0.3f;
    duration[static_cast<int>(Phase::Blast)]     = rng.Range(4.0f, 6.0f);
    duration[static_cast<int>(Phase::Scatter)]   = 5.0f;
    duration[static_cast<int>(Phase::Gather)]    = rng.Range(25.0f, 35.0f);
    duration[static_cast<int>(Phase::Disperse)]  = rng.Range(15.0f, 25.0f);
    duration[static_cast<int>(Phase::Fade)]      = 8.0f;

    // The cycle length the durations above actually produce runs about 76 to 106 seconds, which
    // does not reach the 80 to 115 spec 4.1 states — the overlap between phases 6 and 7 removes
    // four to six seconds that a straight sum of the table would include. Rather than quietly
    // shipping a short cycle or quietly editing the table, the two long phases are stretched to
    // bring the total into range: they are the ones with the widest stated ranges, and they are
    // where a few extra seconds are least noticeable.
    const auto totalFor = [&]() {
        float t = 0.0f;
        for (int i = 0; i < kPhaseCount; ++i) {
            if (i == static_cast<int>(Phase::Scatter)) continue;  // overlapped, counted below
            t += duration[i];
        }
        // The blast and scatter pair occupies whichever of the two finishes last.
        t -= duration[static_cast<int>(Phase::Blast)];
        t += std::fmax(duration[static_cast<int>(Phase::Blast)],
                       kBlastScatterOverlap + duration[static_cast<int>(Phase::Scatter)]);
        return t;
    };

    const float raw = totalFor();
    if (raw < kCycleMin || raw > kCycleMax) {
        const float target  = core::Clamp(raw, kCycleMin, kCycleMax);
        const float slack   = duration[static_cast<int>(Phase::Gather)] +
                            duration[static_cast<int>(Phase::Disperse)];
        const float scale = (slack + (target - raw)) / slack;

        duration[static_cast<int>(Phase::Gather)] *= scale;
        duration[static_cast<int>(Phase::Disperse)] *= scale;
    }

    Timeline line;

    float cursor = 0.0f;
    for (int i = 0; i < kPhaseCount; ++i) {
        const Phase p = static_cast<Phase>(i);

        if (p == Phase::Scatter) {
            // Spec 4.1: phase 6 MUST overlap phase 7. Scatter begins shortly after the shell
            // starts expanding, not after it finishes, so the nearest triangles are already
            // flying while the front is still crossing the city.
            line.start_[i] = line.start_[static_cast<int>(Phase::Blast)] + kBlastScatterOverlap;
            line.end_[i]   = line.start_[i] + duration[i];

            // Whatever comes next waits for both of them.
            cursor = std::fmax(line.end_[i], line.end_[static_cast<int>(Phase::Blast)]);
            continue;
        }

        line.start_[i] = cursor;
        line.end_[i]   = cursor + duration[i];
        cursor         = line.end_[i];
    }

    line.total_ = cursor;

    // Every boundary, not a selection of them. Half the questions asked of this file so far have
    // been "which phase was t in", and a summary that names five of eleven cannot answer them.
    app::Log("timeline: %.1fs total", line.total_);
    for (int i = 0; i < kPhaseCount; ++i) {
        const Phase p = static_cast<Phase>(i);
        app::Log("  phase %d %-10s %6.1f - %6.1f", i, PhaseName(p), line.Start(p), line.End(p));
    }

    return line;
}

float Timeline::Progress(Phase p, float t) const {
    const float length = Duration(p);
    if (length <= 0.0f) return t >= Start(p) ? 1.0f : 0.0f;
    return core::Saturate((t - Start(p)) / length);
}

float Timeline::Elapsed(Phase p, float t) const {
    return core::Clamp(t - Start(p), 0.0f, Duration(p));
}

Phase Timeline::Primary(float t) const {
    Phase latest = Phase::EmptyLand;
    for (int i = 0; i < kPhaseCount; ++i) {
        if (t >= start_[i]) latest = static_cast<Phase>(i);
    }
    return latest;
}

int Timeline::BoardSeconds(float t) const {
    // Lit from the moment it exists, holding at five. The board rises at the end of phase 1 and
    // the countdown does not start until phase 3, and a board that stood dark across all of phase
    // 2 read as a piece of scenery rather than as the thing about to happen. It shows 00:00:05
    // from the frame it appears, which is also the only reading that makes the first digit change
    // an event rather than the display switching on.
    //
    // Nothing needs a phase test for "has it risen yet": the renderer already declines to draw a
    // board whose rise is zero, so this being lit before then is a value nobody reads.
    if (t < Start(Phase::Countdown)) return 5;

    // Dark again from the flash (spec 7.6).
    if (t >= Start(Phase::Flash)) return -1;

    if (t < End(Phase::Countdown)) {
        // Five whole seconds hold five digits: 5, 4, 3, 2, 1, one second each. Zero is not one of
        // them. A five-second phase cannot give six digits a second apiece, and taking the missing
        // second off the front — opening on 4 — is the one reading that makes the board wrong on
        // the frame the viewer first looks at it. Zero belongs to the missile run below, which is
        // what spec 7.6 means by holding at zero: the count reaches zero and the thing arrives.
        const float elapsed = t - Start(Phase::Countdown);
        return core::Clamp(5 - static_cast<int>(std::floor(elapsed)), 0, 5);
    }

    // Holds at zero through the missile (spec 7.6).
    return 0;
}

}  // namespace world
