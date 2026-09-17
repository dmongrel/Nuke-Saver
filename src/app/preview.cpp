// Preview mode, /p <hwnd> (spec 9.3).
//
// Deliberately does not touch Vulkan. A preview window is a few hundred pixels across and the
// Screen Saver Settings dialog can destroy it at any moment; standing up a device for that is
// neither worth the wait nor safe against the parent vanishing mid-initialisation.
//
// Until M6 can bake stills from a working renderer, preview paints black. That is the specified
// behavior, not a stub — so everything else here is finished and does not need revisiting.

#include "app/preview.h"

namespace app {
namespace {

const wchar_t* kClassName = L"NukeSaverPreview";

// The parent is owned by another process's dialog and can go away without telling us. Poll for
// it rather than trusting a notification that may never arrive.
constexpr UINT_PTR kParentWatchTimer = 1;
constexpr UINT     kParentWatchMs    = 250;

HWND g_parent = nullptr;

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(hwnd, &ps);
            FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kParentWatchTimer && (!g_parent || !IsWindow(g_parent))) {
                PostQuitMessage(0);
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kParentWatchTimer);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int RunPreview(HINSTANCE instance, HWND parent) {
    if (!parent || !IsWindow(parent)) return 0;
    g_parent = parent;

    WNDCLASSW wc{};
    wc.lpfnWndProc   = PreviewProc;
    wc.hInstance     = instance;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    if (!RegisterClassW(&wc)) return 0;

    RECT rc{};
    if (!GetClientRect(parent, &rc)) {
        UnregisterClassW(kClassName, instance);
        return 0;
    }

    HWND hwnd = CreateWindowExW(0, kClassName, L"Nuke Saver", WS_CHILD | WS_VISIBLE, 0, 0,
                                rc.right, rc.bottom, parent, nullptr, instance, nullptr);
    if (!hwnd) {
        UnregisterClassW(kClassName, instance);
        return 0;
    }

    SetTimer(hwnd, kParentWatchTimer, kParentWatchMs, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // The parent can die between messages just as easily as during one.
        if (!IsWindow(parent)) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    UnregisterClassW(kClassName, instance);
    g_parent = nullptr;
    return 0;
}

}  // namespace app
