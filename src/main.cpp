// nuke-saver entry point.
//
// Parses the screen-saver arguments and dispatches. Everything else lives behind app/ and
// render/; this file exists to keep that dispatch in one readable place.
//
// Spec 9.1 governs the argument grammar, and its last rule matters most: anything unrecognised
// exits 0 immediately and silently.

#include <windows.h>
#include <shellapi.h>

#include "app/args.h"
#include "app/log.h"
#include "app/config_dialog.h"
#include "app/preview.h"
#include "app/windows.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    app::LogInit();

    int     argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;

    const app::Args args = app::Parse(argc, argv);
    LocalFree(argv);
    app::Log("argc=%d mode=%d parent=%llu", argc, static_cast<int>(args.mode),
             args.parentHandle);

    HWND parent = nullptr;
    if (args.hasParent) {
        parent = reinterpret_cast<HWND>(static_cast<UINT_PTR>(args.parentHandle));
        if (!IsWindow(parent)) parent = nullptr;
    }

    switch (args.mode) {
        case app::Mode::FullScreen:
            return app::RunFullScreen(instance);

        case app::Mode::Preview:
            // A preview with no live parent has nothing to draw into.
            if (!parent) return 0;
            return app::RunPreview(instance, parent);

        case app::Mode::Configure:
            return app::RunConfigure(instance, parent);

        case app::Mode::Exit:
        default:
            return 0;
    }
}
