// Full-screen screen-saver host (spec 9.2).
//
// One borderless top-most window per monitor, all sharing one renderer, one simulation clock,
// and one exit path.
#ifndef NUKE_SAVER_WINDOWS_H
#define NUKE_SAVER_WINDOWS_H

#include <windows.h>

namespace app {

// Runs until input arrives or every window is gone. Always returns 0 — a screen saver that
// reports failure to the shell has nothing useful to say (spec section 12).
int RunFullScreen(HINSTANCE instance);

}  // namespace app

#endif
