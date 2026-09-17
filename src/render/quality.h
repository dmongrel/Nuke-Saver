// The auto-quality controller (spec 11.2).
//
// "The renderer MUST measure its own frame time over a rolling window and, in `auto`, step down
// after missing budget for 2 s and back up after beating it with margin for 10 s, damped so it
// does not oscillate."
//
// The damping is the part with teeth. A controller that steps down when it is slow and up when it
// is fast, with nothing else, will sit on the boundary between two levels and swap between them
// forever — and a quality change is visible, so that is worse than simply running at the lower
// level. So a level remembers how many times it has been demoted from, and each demotion doubles
// how long the frame time has to be comfortable before that level is tried again.
//
// Kept out of the renderer so that it can be driven by a list of frame times in a test rather than
// by a GPU. Nothing in here knows what a quality level costs; it only knows which way is cheaper.
#ifndef NUKE_SAVER_RENDER_QUALITY_H
#define NUKE_SAVER_RENDER_QUALITY_H

#include "app/settings.h"

namespace render {

// Spec 11.2's ladder, best to worst. Four rungs, because the fragment budget it names has four:
// 300, 200, 120 and 60 triangles a building.
constexpr int kQualityLevels = 4;

// Where `auto` starts. Spec 11.2 sets the target as 60 FPS at native resolution at medium, so
// medium is what it opens at rather than the top: starting high means the first two seconds of
// every run on a modest machine are the two seconds that look worst.
constexpr int kQualityDefault = 1;

class QualityController {
public:
    // `settings.quality` of Auto gives a controller that moves; anything else pins the level and
    // Update becomes a no-op that still measures, so the average is available for logging.
    static QualityController FromSettings(const app::Settings& settings);

    // Call once a frame with that frame's duration in seconds. Returns the level to render at,
    // 0 being the best.
    int Update(float frameSeconds);

    int   level() const { return level_; }
    bool  automatic() const { return automatic_; }
    float average() const { return average_; }

    // How many times the controller has changed level. Only the log and the tests want this; it
    // is the number that says whether the damping is working.
    int changes() const { return changes_; }

private:
    // 60 FPS at native resolution, which spec 11.2 names as the target.
    static constexpr float kBudget = 1.0f / 60.0f;

    // How far under budget counts as "with margin". A level is only worth climbing back to if
    // there is room for it to cost more, and a frame at 16.0 ms has none.
    static constexpr float kMargin = 0.75f;

    // Spec 11.2's two dwell times.
    static constexpr float kDownSeconds = 2.0f;
    static constexpr float kUpSeconds   = 10.0f;

    // Anything longer than this is not a slow frame, it is a stall: the cycle reset of spec 4.1
    // regenerates the world and waits for the device, and demoting the whole run because of the
    // one frame that happens on would be a controller that ratchets itself to the bottom.
    static constexpr float kStallSeconds = 0.25f;

    // Nothing is decided before this. The first frames of a run include shader compilation, the
    // first upload of every buffer and the swapchain coming up.
    static constexpr float kWarmupSeconds = 3.0f;

    bool  automatic_ = true;
    int   level_     = kQualityDefault;
    int   changes_   = 0;

    // Exponential moving average of the frame time. A rolling window over a fixed number of
    // frames would cover a different amount of time at 20 FPS than at 200, which is the wrong way
    // round: the spec's windows are in seconds.
    float average_ = kBudget;
    bool  seeded_  = false;

    float warmup_ = 0.0f;
    float over_   = 0.0f;  // seconds the average has been continuously over budget
    float under_  = 0.0f;  // seconds it has been continuously under budget with margin

    // How many times each level has been demoted from. Indexed by level, and the reason the
    // controller settles instead of hunting.
    int demotions_[kQualityLevels]{};

    float UpHoldFor(int level) const;
};

// The fixed levels of spec section 10's `Quality` value, mapped onto the ladder above. Three names
// for four rungs: the bottom one exists for the controller to fall to and is not offered in the
// settings, because someone choosing a quality level by hand is not choosing that one.
int QualityLevelFor(app::Quality quality);

}  // namespace render

#endif
