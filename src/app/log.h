// Debug tracing.
//
// The shipped .scr is a Windows-subsystem binary with no console, and spec section 12 forbids
// showing the user a dialog when something goes wrong — so when behaviour needs investigating
// there is nowhere for it to say anything. This writes to a file, and only when the environment
// variable NUKE_SAVER_LOG is set, so a normal run costs one getenv at startup and nothing after.
#ifndef NUKE_SAVER_LOG_H
#define NUKE_SAVER_LOG_H

namespace app {

// Reads NUKE_SAVER_LOG. If set, its value is the log path ("1" means %TEMP%\nuke-saver.log).
void LogInit();

// printf-style. A no-op unless logging was enabled.
void Log(const char* fmt, ...);

bool LogEnabled();

}  // namespace app

#endif
