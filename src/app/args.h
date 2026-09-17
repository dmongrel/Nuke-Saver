// Command-line parsing for the screen-saver host (spec 9.1).
//
// Semantics match ghost-saver exactly, which the spec makes authoritative: a leading '/' or '-'
// is stripped, the first remaining character selects the mode case-insensitively, and anything
// unrecognised exits 0.
//
// Parsing is kept free of Win32 calls so it can be exercised by the selftest harness without
// creating a window. Validating that a parent HWND still exists is the caller's job.
#ifndef NUKE_SAVER_ARGS_H
#define NUKE_SAVER_ARGS_H

namespace app {

enum class Mode {
    FullScreen,  // no arguments, or /s
    Preview,     // /p <hwnd>, /p:<hwnd>
    Configure,   // /c, /c:<hwnd>
    Exit,        // anything else: leave immediately, quietly, with code 0
};

struct Args {
    Mode mode = Mode::FullScreen;

    // The window handle as written on the command line, or 0 when none was given.
    // Preview REQUIRES one; Configure treats it as an optional owner for the dialog.
    unsigned long long parentHandle = 0;
    bool               hasParent    = false;
};

// argv follows the usual convention: argv[0] is the executable.
Args Parse(int argc, const wchar_t* const* argv);

}  // namespace app

#endif
