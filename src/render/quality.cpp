#include "render/quality.h"

namespace render {

int QualityLevelFor(app::Quality quality) {
    switch (quality) {
        case app::Quality::High:   return 0;
        case app::Quality::Medium: return 1;
        case app::Quality::Low:    return 2;
        case app::Quality::Auto:   break;
    }
    return kQualityDefault;
}

QualityController QualityController::FromSettings(const app::Settings& settings) {
    QualityController q;
    q.automatic_ = settings.quality == app::Quality::Auto;
    q.level_     = QualityLevelFor(settings.quality);
    return q;
}

float QualityController::UpHoldFor(int level) const {
    if (level < 0 || level >= kQualityLevels) return kUpSeconds;

    // Spec 11.2's ten seconds the first time a level is tried again, then twenty, then forty. The
    // doubling starts on the *second* demotion, not the first: one demotion says nothing about
    // whether the level is affordable — the fireball arriving is enough to cause it — and making
    // the very first recovery wait twice as long as the spec asks for would be reading the
    // damping requirement as a licence to ignore the number next to it.
    //
    // Capped after two doublings, so a machine that genuinely got faster — a laptop coming off
    // battery, a game closing — still recovers inside a cycle.
    float hold = kUpSeconds;
    for (int i = 1; i < demotions_[level] && i <= 2; ++i) hold *= 2.0f;
    return hold;
}

int QualityController::Update(float frameSeconds) {
    // A stall is not a measurement. Dropped before anything else, including the average, because
    // a single two-second frame folded into an exponential average takes a long while to leave it.
    if (frameSeconds <= 0.0f || frameSeconds > kStallSeconds) return level_;

    if (!seeded_) {
        average_ = frameSeconds;
        seeded_  = true;
    } else {
        // About a third of a second of memory. Short enough to notice the fireball arriving,
        // long enough that one late frame does not start the two-second countdown to a demotion.
        const float alpha = frameSeconds / (frameSeconds + 0.33f);
        average_ += (frameSeconds - average_) * alpha;
    }

    if (warmup_ < kWarmupSeconds) {
        warmup_ += frameSeconds;
        return level_;
    }

    if (!automatic_) return level_;

    // Both the window and this frame have to agree before either timer advances.
    //
    // The average alone is not enough, and the way it fails is worth spelling out. Coming off a
    // burst of 30 FPS frames onto a steady 61, the average takes over a second to cross back
    // under a budget the frames themselves are already inside — and that is a second of `over_`
    // still counting, on top of what the burst had already banked. A machine that recovered to
    // exactly inside budget was demoted a second time for it. Requiring the current frame to
    // agree clears the timer on the first good frame, which is the honest reading of "missing
    // budget for 2 s".
    const bool slowNow = frameSeconds > kBudget;
    const bool fastNow = frameSeconds < kBudget * kMargin;

    if (average_ > kBudget && slowNow) {
        over_ += frameSeconds;
        under_ = 0.0f;
    } else if (average_ < kBudget * kMargin && fastNow) {
        under_ += frameSeconds;
        over_  = 0.0f;
    } else {
        // Between the budget and the margin: fast enough to keep, not fast enough to climb. Both
        // timers stop, which is what keeps a frame time sitting on the boundary from slowly
        // accumulating its way into a change in either direction.
        over_  = 0.0f;
        under_ = 0.0f;
    }

    if (over_ >= kDownSeconds && level_ < kQualityLevels - 1) {
        ++demotions_[level_];
        ++level_;
        ++changes_;
        over_  = 0.0f;
        under_ = 0.0f;
    } else if (level_ > 0 && under_ >= UpHoldFor(level_ - 1)) {
        --level_;
        ++changes_;
        over_  = 0.0f;
        under_ = 0.0f;
    }

    return level_;
}

}  // namespace render
