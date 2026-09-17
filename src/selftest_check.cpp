#include "selftest_check.h"

#include <cmath>
#include <cstdio>

namespace selftest {
namespace {
int g_checks   = 0;
int g_failures = 0;
}  // namespace

void Check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void CheckNear(float got, float want, float tol, const char* what) {
    ++g_checks;
    // Written as a negated <= so a NaN fails rather than silently passing, which is the whole
    // reason several of these checks exist.
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failures;
        std::printf("  FAIL  %s (got %.6f, want %.6f +/- %.6f)\n", what, got, want, tol);
    }
}

int Checks() { return g_checks; }
int Failures() { return g_failures; }

}  // namespace selftest
