// Preview mode host (spec 9.3).
#ifndef NUKE_SAVER_PREVIEW_H
#define NUKE_SAVER_PREVIEW_H

#include <windows.h>

namespace app {

// Draws into the Screen Saver Settings dialog's preview window. Always returns 0.
int RunPreview(HINSTANCE instance, HWND parent);

}  // namespace app

#endif
