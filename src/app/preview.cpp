// Preview mode, /p <hwnd> (spec 9.3).
//
// Deliberately does not touch Vulkan. A preview window is a few hundred pixels across and the
// Screen Saver Settings dialog can destroy it at any moment; standing up a device for that is
// neither worth the wait nor safe against the parent vanishing mid-initialisation.
//
// It paints one still, baked from a real run and embedded (see preview_image.h). Not a filmstrip
// and not a cross-fade: the pane is small, the dialog may close within a second or two, and a
// recognisable frame of the cloud answers "what is this screen saver" faster than any animation
// of it could.

#include "app/preview.h"

#include "app/preview_image.h"

namespace app {
namespace {

const wchar_t* kClassName = L"NukeSaverPreview";

// The parent is owned by another process's dialog and can go away without telling us. Poll for
// it rather than trusting a notification that may never arrive.
constexpr UINT_PTR kParentWatchTimer = 1;
constexpr UINT     kParentWatchMs    = 250;

HWND g_parent = nullptr;

// The embedded still, or nothing if the blob is too short to hold what its header claims. Checked
// rather than trusted: this is generated at build time from a file on disk, and a preview that
// paints a stripe of whatever followed the buffer is worse than one that paints black.
struct Thumbnail {
    const unsigned char* pixels = nullptr;
    LONG                 width  = 0;
    LONG                 height = 0;
};

Thumbnail EmbeddedThumbnail() {
    Thumbnail t;
    if (!g_preview_thumb || g_preview_thumb_size < 8) return t;

    const unsigned char* p = g_preview_thumb;
    const unsigned w = static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8) |
                       (static_cast<unsigned>(p[2]) << 16) | (static_cast<unsigned>(p[3]) << 24);
    const unsigned h = static_cast<unsigned>(p[4]) | (static_cast<unsigned>(p[5]) << 8) |
                       (static_cast<unsigned>(p[6]) << 16) | (static_cast<unsigned>(p[7]) << 24);
    if (w == 0 || h == 0 || w > 4096u || h > 4096u) return t;

    // The multiply is done in the wider type on purpose: 4096 * 4096 * 4 overflows unsigned, and
    // the bound above is the only reason it cannot here.
    const unsigned long long need = 8ull + static_cast<unsigned long long>(w) * h * 4ull;
    if (need > g_preview_thumb_size) return t;

    t.pixels = p + 8;
    t.width  = static_cast<LONG>(w);
    t.height = static_cast<LONG>(h);
    return t;
}

// The largest rect of the image's aspect that fits inside `into`, centred. Letterboxed rather than
// stretched to fill: the pane's aspect is the dialog's business and not always the image's, and a
// mushroom cloud squashed sideways reads as a mistake rather than as a screen saver.
RECT FitPreserving(const RECT& into, LONG imageW, LONG imageH) {
    const LONG boxW = into.right - into.left;
    const LONG boxH = into.bottom - into.top;
    if (boxW <= 0 || boxH <= 0 || imageW <= 0 || imageH <= 0) return into;

    LONG w = boxW;
    LONG h = static_cast<LONG>((static_cast<long long>(boxW) * imageH) / imageW);
    if (h > boxH) {
        h = boxH;
        w = static_cast<LONG>((static_cast<long long>(boxH) * imageW) / imageH);
    }

    const LONG x = into.left + (boxW - w) / 2;
    const LONG y = into.top + (boxH - h) / 2;
    return RECT{x, y, x + w, y + h};
}

void PaintThumbnail(HDC dc, const RECT& client) {
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    const Thumbnail t = EmbeddedThumbnail();
    if (!t.pixels) return;  // black, which is what this did before there was a still to draw

    const RECT dst = FitPreserving(client, t.width, t.height);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize     = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth    = t.width;
    bi.bmiHeader.biHeight   = -t.height;  // negative: top row first, as the blob stores it
    bi.bmiHeader.biPlanes   = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // The still is larger than the pane at every sane DPI, so this is a downscale and the mode
    // matters: the default would drop rows and columns and turn the cap into a comb.
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);

    StretchDIBits(dc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top, 0, 0, t.width,
                  t.height, t.pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(hwnd, &ps);

            // Painted against the whole client rect rather than ps.rcPaint: the image is placed
            // by where the window is, not by which part of it was invalidated, and clipping to
            // the damaged region is what the DC already does.
            RECT client{};
            GetClientRect(hwnd, &client);
            PaintThumbnail(dc, client);

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
