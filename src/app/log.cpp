#include "app/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace app {
namespace {

FILE* g_file    = nullptr;
bool  g_enabled = false;

}  // namespace

void LogInit() {
    char* value = nullptr;
    size_t len  = 0;
    if (_dupenv_s(&value, &len, "NUKE_SAVER_LOG") != 0 || !value || !*value) {
        if (value) free(value);
        return;
    }

    char path[MAX_PATH * 2];
    if (std::strcmp(value, "1") == 0) {
        char temp[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, temp);
        if (n == 0 || n >= MAX_PATH) {
            free(value);
            return;
        }
        std::snprintf(path, sizeof(path), "%snuke-saver.log", temp);
    } else {
        std::snprintf(path, sizeof(path), "%s", value);
    }
    free(value);

    g_file = std::fopen(path, "w");
    if (!g_file) return;
    g_enabled = true;

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    Log("nuke-saver log opened (qpc freq %lld)", static_cast<long long>(freq.QuadPart));
}

bool LogEnabled() { return g_enabled; }

void Log(const char* fmt, ...) {
    if (!g_enabled || !g_file) return;

    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double t = freq.QuadPart ? static_cast<double>(now.QuadPart) / freq.QuadPart : 0.0;

    std::fprintf(g_file, "[%10.3f] ", t);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

}  // namespace app
