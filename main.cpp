#include <windows.h>
#include <shellapi.h>
#include <wctype.h>

static bool g_preview = false;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_MOUSEMOVE:
        if (!g_preview) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static ATOM RegisterWindowClass(HINSTANCE hInstance) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = nullptr;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"NukeSaverClass";
    return RegisterClassW(&wc);
}

static int RunMessageLoop(HINSTANCE hInstance) {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterClassW(L"NukeSaverClass", hInstance);
    return 0;
}

static int RunFullScreen(HINSTANCE hInstance) {
    if (!RegisterWindowClass(hInstance)) return 1;

    int x  = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y  = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"NukeSaverClass", L"Nuke Saver",
                                WS_POPUP, x, y, cx, cy,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    ShowCursor(FALSE);
    int rc = RunMessageLoop(hInstance);
    ShowCursor(TRUE);
    return rc;
}

static int RunPreview(HINSTANCE hInstance, HWND hparent) {
    if (!RegisterWindowClass(hInstance)) return 1;

    RECT rc;
    GetClientRect(hparent, &rc);

    HWND hwnd = CreateWindowExW(0, L"NukeSaverClass", L"Nuke Saver",
                                WS_CHILD | WS_VISIBLE,
                                0, 0, rc.right, rc.bottom,
                                hparent, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    return RunMessageLoop(hInstance);
}

static int RunConfigure(HWND hparent) {
    MessageBoxW(hparent, L"There are no settings to configure.",
                L"Nuke Saver", MB_OK | MB_ICONINFORMATION);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    (void)nCmdShow;

    int argc;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;

    bool fullScreen  = true;
    bool previewMode = false;
    HWND hparent     = nullptr;

    if (argc > 1) {
        WCHAR* arg = argv[1];
        if (*arg == L'/' || *arg == L'-') arg++;
        WCHAR lower = towlower(*arg);

        if (lower == L's') {
            fullScreen = true;
        } else if (lower == L'p') {
            fullScreen  = false;
            previewMode = true;
            arg++;
            if (*arg == L':') arg++;
            else {
                if (argc < 3) { LocalFree(argv); return 0; }
                arg = argv[2];
            }
            hparent = (HWND)(UINT_PTR)wcstoul(arg, nullptr, 10);
            if (!hparent || !IsWindow(hparent)) { LocalFree(argv); return 0; }
        } else if (lower == L'c') {
            fullScreen = false;
            arg++;
            if (*arg == L':') arg++;
            else arg = (argc < 3) ? nullptr : argv[2];
            if (arg && *arg) {
                hparent = (HWND)(UINT_PTR)wcstoul(arg, nullptr, 10);
                if (!IsWindow(hparent)) hparent = nullptr;
            }
        } else {
            LocalFree(argv);
            return 0;
        }
    }

    LocalFree(argv);

    g_preview = previewMode;

    if (fullScreen)  return RunFullScreen(hInstance);
    if (previewMode) return RunPreview(hInstance, hparent);
    return RunConfigure(hparent);
}
