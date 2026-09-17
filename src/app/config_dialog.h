// Settings dialog, /c (spec section 10).
#ifndef NUKE_SAVER_CONFIG_DIALOG_H
#define NUKE_SAVER_CONFIG_DIALOG_H

#include <windows.h>

namespace app {

// Shows the three settings from spec section 10 and persists them. Always returns 0.
int RunConfigure(HINSTANCE instance, HWND parent);

}  // namespace app

#endif
