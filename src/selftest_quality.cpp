// The auto-quality controller (spec 11.2).
//
// This is a control loop, and a control loop is exactly the sort of thing that looks right in a
// capture and is wrong in a run: every failure it has takes tens of seconds to show up. So it is
// driven here by lists of frame times, at frame rates chosen to sit on each side of every
// threshold it has, and the property that matters most — that it settles rather than hunting — is
// checked by counting how many times it changes its mind.

#include "app/settings.h"
#include "render/quality.h"
#include "selftest_check.h"

#include <cstdio>

namespace selftest {
namespace {

using render::kQualityDefault;
using render::kQualityLevels;
using render::QualityController;

app::Settings AutoSettings() {
    app::Settings s;
    s.quality = app::Quality::Auto;
    return s;
}

// Runs the controller for `seconds` at a steady frame time and gives back the level it ends on.
int RunSteady(QualityController* q, float frameSeconds, float seconds) {
    int level = q->level();
    for (float t = 0.0f; t < seconds; t += frameSeconds) level = q->Update(frameSeconds);
    return level;
}

constexpr float kFast  = 1.0f / 144.0f;  // comfortably inside the margin
constexpr float kOnPar = 1.0f / 61.0f;   // under budget, but with no room to climb
constexpr float kSlow  = 1.0f / 30.0f;   // missing budget badly

}  // namespace

void TestQuality() {
    std::printf("auto quality (spec 11.2)\n");

    // The fixed levels of spec section 10 map onto the ladder, and `auto` starts at medium
    // because that is the level spec 11.2 sets the 60 FPS target at.
    Check(render::QualityLevelFor(app::Quality::High) == 0, "high is the top of the ladder");
    Check(render::QualityLevelFor(app::Quality::Medium) == 1, "medium is one rung down");
    Check(render::QualityLevelFor(app::Quality::Low) == 2, "low is two rungs down");
    Check(render::QualityLevelFor(app::Quality::Auto) == kQualityDefault,
          "auto opens at medium rather than at the top");

    // A pinned level never moves, however badly the frames go. Someone who chose a quality level
    // chose it.
    {
        app::Settings fixed;
        fixed.quality        = app::Quality::High;
        QualityController q  = QualityController::FromSettings(fixed);
        RunSteady(&q, kSlow, 60.0f);
        Check(q.level() == 0 && q.changes() == 0, "a pinned level survives a minute of slow frames");
        Check(q.average() > 0.02f, "a pinned controller still measures the frame time");
    }

    // Warm-up. The first frames of a run are shader compilation and the first upload of every
    // buffer, and demoting on those would mean every run starts by getting worse.
    {
        QualityController q = QualityController::FromSettings(AutoSettings());
        RunSteady(&q, kSlow, 2.5f);
        Check(q.level() == kQualityDefault, "nothing is decided during the warm-up");
    }

    // Slow frames step down, and spec 11.2 says after 2 s. Checked from both sides: not before
    // the warm-up plus two seconds, and definitely by a little after.
    {
        QualityController q = QualityController::FromSettings(AutoSettings());
        RunSteady(&q, kSlow, 3.0f + 1.5f);
        Check(q.level() == kQualityDefault, "two seconds of slow frames have not elapsed yet");
        RunSteady(&q, kSlow, 1.0f);
        Check(q.level() == kQualityDefault + 1, "and then it steps down exactly one rung");
    }

    // It keeps stepping down while the frames stay slow, and stops at the bottom rather than
    // walking off the end of the ladder.
    {
        QualityController q = QualityController::FromSettings(AutoSettings());
        RunSteady(&q, kSlow, 120.0f);
        Check(q.level() == kQualityLevels - 1, "sustained slow frames reach the bottom rung");
        const int settled = q.changes();
        RunSteady(&q, kSlow, 120.0f);
        Check(q.changes() == settled, "and it stops there rather than counting past it");
    }

    // Fast frames climb back, after spec 11.2's ten seconds and not before.
    {
        QualityController q = QualityController::FromSettings(AutoSettings());
        RunSteady(&q, kSlow, 6.0f);
        Check(q.level() == kQualityDefault + 1, "demoted once");

        RunSteady(&q, kFast, 8.0f);
        Check(q.level() == kQualityDefault + 1, "eight seconds of fast frames is not yet enough");
        RunSteady(&q, kFast, 4.0f);
        Check(q.level() == kQualityDefault, "twelve is");
    }

    // Beating the budget is not the same as beating it with margin. A frame time just under
    // 16.7 ms has no room for the level above to cost anything more, and climbing into it is how
    // a controller ends up oscillating.
    {
        QualityController q = QualityController::FromSettings(AutoSettings());
        RunSteady(&q, kSlow, 6.0f);
        const int demoted = q.level();
        RunSteady(&q, kOnPar, 120.0f);
        Check(q.level() == demoted, "frames merely inside budget never climb back");
    }

    // The damping, which is the whole reason this is a class rather than two if statements. A
    // machine that can just barely not afford a level will alternate: fast for a while at the
    // lower level, slow as soon as it climbs. Undamped that is a change every twelve seconds
    // forever; damped, the dwell time doubles each time and it settles.
    {
        QualityController q = QualityController::FromSettings(AutoSettings());

        int changesInFirstMinute = 0;
        int changesInLastMinute  = 0;
        for (int minute = 0; minute < 5; ++minute) {
            const int before = q.changes();
            for (int burst = 0; burst < 4; ++burst) {
                // Fast while it is at the cheaper level, slow the moment it climbs.
                const int start = q.level();
                RunSteady(&q, kFast, 15.0f);
                if (q.level() < start) RunSteady(&q, kSlow, 3.0f);
            }
            if (minute == 0) changesInFirstMinute = q.changes() - before;
            if (minute == 4) changesInLastMinute = q.changes() - before;
        }

        Check(changesInFirstMinute > 0, "the controller does react to a machine on the boundary");
        Check(changesInLastMinute < changesInFirstMinute,
              "and changes level less often the longer the boundary persists");
    }

    // A stall is not a slow frame. The cycle reset of spec 4.1 regenerates the world and waits
    // for the device; a controller that treated that as evidence would ratchet to the bottom over
    // a night's run, one rung per cycle, on a machine that was never slow.
    {
        QualityController q = QualityController::FromSettings(AutoSettings());
        RunSteady(&q, kFast, 30.0f);
        for (int cycle = 0; cycle < 40; ++cycle) {
            q.Update(2.0f);  // the reset
            RunSteady(&q, kFast, 20.0f);
        }
        Check(q.level() == 0, "a machine that is always fast climbs to the top and stays");
        Check(q.changes() == 1, "forty cycle resets cause no change of their own");
        Check(q.average() < kOnPar, "and the average is not dragged up by them either");
    }
}

}  // namespace selftest
