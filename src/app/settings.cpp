#include "app/settings.h"

#include <windows.h>

namespace app {
namespace {

const wchar_t* kKey = L"Software\\nuke-saver";

// Reads one DWORD, clamping to [0, maxValid]. An absent or out-of-range value leaves the
// caller's default untouched, which is what makes an empty registry key correct rather than
// merely tolerated.
unsigned ReadDword(HKEY key, const wchar_t* name, unsigned fallback, unsigned maxValid) {
    DWORD value = 0;
    DWORD size  = sizeof(value);
    DWORD type  = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) !=
        ERROR_SUCCESS) {
        return fallback;
    }
    if (type != REG_DWORD || size != sizeof(value)) return fallback;
    if (value > maxValid) return fallback;
    return value;
}

}  // namespace

Settings Load() {
    Settings s;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return s;  // no key yet: defaults, including twilight
    }

    s.quality = static_cast<Quality>(
        ReadDword(key, L"Quality", static_cast<unsigned>(s.quality), 3));
    s.timeOfDay = static_cast<TimeOfDay>(
        ReadDword(key, L"TimeOfDay", static_cast<unsigned>(s.timeOfDay), 4));
    s.camera = static_cast<CameraMode>(
        ReadDword(key, L"CameraMode", static_cast<unsigned>(s.camera), 4));

    RegCloseKey(key);
    return s;
}

bool Save(const Settings& s) {
    HKEY  key        = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return false;
    }

    auto write = [key](const wchar_t* name, unsigned v) {
        DWORD value = v;
        return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                              sizeof(value)) == ERROR_SUCCESS;
    };

    bool ok = write(L"Quality", static_cast<unsigned>(s.quality));
    ok      = write(L"TimeOfDay", static_cast<unsigned>(s.timeOfDay)) && ok;
    ok      = write(L"CameraMode", static_cast<unsigned>(s.camera)) && ok;

    RegCloseKey(key);
    return ok;
}

}  // namespace app
