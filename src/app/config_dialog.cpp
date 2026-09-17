#include "app/config_dialog.h"

#include "app/settings.h"
#include "resource.h"

namespace app {
namespace {

// Spec section 10: three settings, nothing else, a plain Win32 dialog resource, no custom
// controls and no theming work. The order of each list matches the DWORD values stored in the
// registry, so the index is the value.

const wchar_t* const kQuality[]   = {L"Automatic", L"Low", L"Medium", L"High"};
const wchar_t* const kTimeOfDay[] = {L"Random each cycle", L"Morning", L"Noon", L"Twilight",
                                     L"Night"};
const wchar_t* const kCamera[]    = {L"Random each cycle", L"Distant ridge", L"Low approach",
                                     L"High oblique",      L"Street level"};

void FillCombo(HWND dlg, int id, const wchar_t* const* items, int count, unsigned selected) {
    HWND combo = GetDlgItem(dlg, id);
    if (!combo) return;
    for (int i = 0; i < count; ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i]));
    }
    SendMessageW(combo, CB_SETCURSEL, selected < static_cast<unsigned>(count) ? selected : 0, 0);
}

unsigned ComboValue(HWND dlg, int id, unsigned fallback) {
    HWND    combo = GetDlgItem(dlg, id);
    LRESULT sel   = combo ? SendMessageW(combo, CB_GETCURSEL, 0, 0) : CB_ERR;
    return sel == CB_ERR ? fallback : static_cast<unsigned>(sel);
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            const Settings s = Load();
            FillCombo(dlg, IDC_QUALITY, kQuality, 4, static_cast<unsigned>(s.quality));
            FillCombo(dlg, IDC_TIMEOFDAY, kTimeOfDay, 5, static_cast<unsigned>(s.timeOfDay));
            FillCombo(dlg, IDC_CAMERA, kCamera, 5, static_cast<unsigned>(s.camera));
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    Settings s;
                    s.quality =
                        static_cast<Quality>(ComboValue(dlg, IDC_QUALITY, 0));
                    s.timeOfDay =
                        static_cast<TimeOfDay>(ComboValue(dlg, IDC_TIMEOFDAY, 3));
                    s.camera =
                        static_cast<CameraMode>(ComboValue(dlg, IDC_CAMERA, 0));
                    Save(s);
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            return FALSE;

        case WM_CLOSE:
            EndDialog(dlg, IDCANCEL);
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

}  // namespace

int RunConfigure(HINSTANCE instance, HWND parent) {
    if (parent && !IsWindow(parent)) parent = nullptr;
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), parent, DialogProc, 0);
    return 0;
}

}  // namespace app
