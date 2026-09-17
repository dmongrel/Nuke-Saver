#include "app/windows.h"

#include <windowsx.h>

#include "app/capture.h"
#include "app/input_watcher.h"
#include "app/log.h"
#include "app/settings.h"
#include "render/renderer.h"

#include <memory>
#include <vector>

namespace app {
namespace {

const wchar_t* kClassName = L"NukeSaverFullScreen";

// Spec 4.2: a stall must not be handed to the simulation as a single enormous step.
constexpr double kMaxFrameDelta = 0.100;

// Input is watched twice over: through the window procedure below, and by polling the keyboard
// and cursor each frame. See app/input_watcher.h for why the polling path exists — briefly, the
// window procedure only sees input while one of our windows holds focus, and focus can be
// refused or stolen, which would otherwise leave the saver impossible to dismiss.
struct HostState {
    std::unique_ptr<render::Renderer> renderer;
    std::vector<HWND>                 windows;
    InputWatcher                      input;

    bool   running        = true;
    bool   sawFirstMove   = false;
    double elapsed        = 0.0;
    POINT  referenceMouse = {0, 0};
};

HostState* g_host = nullptr;

void RequestExit() {
    if (!g_host) return;
    g_host->running = false;
    PostQuitMessage(0);
}

// Every window routes here, and any of them can end the run. Spec 9.2 is explicit that input
// tears down the whole process rather than the one window that received it: on a multi-monitor
// desktop, dismissing one screen and leaving the others black would be worse than useless.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            // Same grace period the poller uses: the keystroke that launched the saver must
            // not immediately dismiss it.
            if (g_host && g_host->elapsed >= InputWatcher::kGraceSeconds) RequestExit();
            return 0;

        case WM_MOUSEMOVE: {
            if (!g_host) return 0;
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &p);

            if (!g_host->sawFirstMove) {
                g_host->sawFirstMove   = true;
                g_host->referenceMouse = p;
                return 0;  // swallowed (spec 9.2)
            }
            if (g_host->elapsed < InputWatcher::kGraceSeconds) return 0;
            const int dx = p.x - g_host->referenceMouse.x;
            const int dy = p.y - g_host->referenceMouse.y;
            if (dx * dx + dy * dy > InputWatcher::kDeadZonePixels * InputWatcher::kDeadZonePixels) RequestExit();
            return 0;
        }

        case WM_SETCURSOR:
            SetCursor(nullptr);  // spec 9.2: the cursor is hidden for the duration
            return TRUE;

        case WM_ERASEBKGND:
            return 1;  // the renderer owns every pixel; never let GDI flash the background

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(hwnd, &ps);
            FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            if (g_host && g_host->renderer) {
                g_host->renderer->ResizeWindow(hwnd, LOWORD(lParam), HIWORD(lParam));
            }
            return 0;

        case WM_DISPLAYCHANGE:
            // A monitor came or went. Spec A12 only requires that the remaining displays keep
            // rendering, so the cheapest correct answer is to let the affected window close and
            // carry on with the rest.
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_host) {
                if (g_host->renderer) g_host->renderer->DetachWindow(hwnd);
                for (auto it = g_host->windows.begin(); it != g_host->windows.end(); ++it) {
                    if (*it == hwnd) {
                        g_host->windows.erase(it);
                        break;
                    }
                }
                if (g_host->windows.empty()) RequestExit();
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Windows refuses SetForegroundWindow to a process that is not already in the foreground. The
// system grants that right when it starts a screen saver itself, but not when one is launched
// from the settings dialog's Preview button or by hand — and without focus, WM_KEYDOWN never
// arrives and the saver cannot be dismissed from the keyboard. Attaching to the current
// foreground thread's input queue lifts the restriction for the duration of the call.
void ForceForeground(HWND hwnd) {
    const DWORD foreignThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD ownThread     = GetCurrentThreadId();
    const bool  attach        = foreignThread != 0 && foreignThread != ownThread;

    if (attach) AttachThreadInput(foreignThread, ownThread, TRUE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (attach) AttachThreadInput(foreignThread, ownThread, FALSE);
}

BOOL CALLBACK MonitorProc(HMONITOR, HDC, LPRECT rect, LPARAM lParam) {
    auto* rects = reinterpret_cast<std::vector<RECT>*>(lParam);
    rects->push_back(*rect);
    return TRUE;
}

std::vector<RECT> EnumerateMonitors() {
    std::vector<RECT> rects;
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&rects));

    // A desktop with no enumerable monitor should still produce something rather than nothing.
    if (rects.empty()) {
        RECT r{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        rects.push_back(r);
    }
    return rects;
}

}  // namespace

int RunFullScreen(HINSTANCE instance) {
    HostState host;
    g_host = &host;

    Log("RunFullScreen: entry");

    WNDCLASSW wc{};
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = instance;
    wc.hCursor       = nullptr;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    if (!RegisterClassW(&wc)) {
        g_host = nullptr;
        return 0;
    }

    const Settings settings = Load();
    host.renderer           = render::CreateRenderer(settings);
    Log("renderer: %s", host.renderer ? host.renderer->Name() : "none");

    for (const RECT& r : EnumerateMonitors()) {
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        HWND      hwnd =
            CreateWindowExW(WS_EX_TOPMOST, kClassName, L"Nuke Saver", WS_POPUP, r.left,
                            r.top, w, h, nullptr, nullptr, instance, nullptr);
        if (!hwnd) continue;

        if (host.renderer && !host.renderer->AttachWindow(hwnd, w, h)) {
            // This backend cannot present here. Rather than run with a hole in the desktop,
            // drop to the fallback for every window and start the attachments again.
            host.renderer->WaitIdle();
            host.renderer = render::CreateGdiFallbackRenderer();
            for (HWND existing : host.windows) {
                RECT er{};
                GetClientRect(existing, &er);
                host.renderer->AttachWindow(existing, er.right, er.bottom);
            }
            host.renderer->AttachWindow(hwnd, w, h);
        }

        host.windows.push_back(hwnd);
        ShowWindow(hwnd, SW_SHOW);
        Log("window %p at %ld,%ld %dx%d", (void*)hwnd, r.left, r.top, w, h);
    }

    if (host.windows.empty()) {
        UnregisterClassW(kClassName, instance);
        g_host = nullptr;
        return 0;
    }

    ForceForeground(host.windows.front());

    // Capture the mouse as well. Spec 9.2 requires that a move anywhere ends the saver, and a
    // pointer that has wandered onto a second monitor's window would otherwise report to that
    // window instead of this one.
    SetCapture(host.windows.front());

    ShowCursor(FALSE);
    Log("entering render loop with %zu window(s)", host.windows.size());

    LARGE_INTEGER freq{}, start{}, prev{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    prev = start;

    // A capture run advances the clock by a fixed step instead of by the wall clock, so "frame
    // 1700" names one moment in the cycle rather than one moment on this machine at this frame
    // rate. Without it a capture is a lottery: the same request lands at a different second on a
    // faster GPU, and two shots meant to be compared are of two different scenes.
    const double fixedStep = CaptureRequestFromEnvironment().enabled ? 1.0 / 60.0 : 0.0;
    if (fixedStep > 0.0) Log("capture run: fixed %.4fs per frame", fixedStep);

    unsigned long long frame = 0;
    MSG                msg{};
    while (host.running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                host.running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!host.running) break;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        double elapsed =
            static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
        double delta =
            static_cast<double>(now.QuadPart - prev.QuadPart) / static_cast<double>(freq.QuadPart);
        prev = now;
        if (delta > kMaxFrameDelta) delta = kMaxFrameDelta;  // spec 4.2
        if (fixedStep > 0.0) {
            elapsed = static_cast<double>(frame) * fixedStep;
            delta   = fixedStep;
        }
        host.elapsed = elapsed;

        if (host.input.Consider(elapsed, SampleNow())) {
            Log("input ended the run at t=%.3f", elapsed);
            host.running = false;
            break;
        }

        if (LogEnabled() && frame % 600 == 0) Log("frame %llu t=%.3f", frame, elapsed);
        ++frame;

        if (host.renderer) host.renderer->RenderFrame(elapsed, delta);

        // Spec 11.2 forbids busy-waiting to pace frames. A FIFO swapchain already blocks in
        // present, and sleeping on top of it costs vblanks rather than power: Windows' default
        // timer granularity is 15.6 ms, so a Sleep(8) here pinned the loop to 64 fps on a
        // 3440x1440 display that was perfectly capable of more. Only an unpaced backend needs
        // the explicit yield.
        if (!host.renderer || !host.renderer->PacesItself()) Sleep(8);
    }

    Log("loop exited after %llu frames", frame);
    if (host.renderer) host.renderer->WaitIdle();

    ReleaseCapture();
    ShowCursor(TRUE);
    for (HWND hwnd : std::vector<HWND>(host.windows)) DestroyWindow(hwnd);

    host.renderer.reset();
    UnregisterClassW(kClassName, instance);
    g_host = nullptr;
    Log("RunFullScreen: clean exit");
    return 0;
}

}  // namespace app
