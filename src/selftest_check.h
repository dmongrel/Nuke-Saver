// Shared check helpers for the console selftest (`make selftest`).
//
// Split out of selftest_main.cpp so the suites can live in their own translation units. The
// Makefile keeps every src/selftest_*.cpp out of the shipped .scr.
#ifndef NUKE_SAVER_SELFTEST_CHECK_H
#define NUKE_SAVER_SELFTEST_CHECK_H

namespace selftest {

void Check(bool condition, const char* what);
void CheckNear(float got, float want, float tol, const char* what);

int Checks();
int Failures();

}  // namespace selftest

#endif
