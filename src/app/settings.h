// Persisted settings (spec section 10).
//
// Stored under HKCU\Software\nuke-saver. Every value has a working default, and a fresh install
// against an empty registry key MUST run correctly — so reads never fail, they fall back.
#ifndef NUKE_SAVER_SETTINGS_H
#define NUKE_SAVER_SETTINGS_H

namespace app {

enum class Quality : unsigned {
    Auto = 0,
    Low,
    Medium,
    High,
};

enum class TimeOfDay : unsigned {
    Random = 0,
    Morning,
    Noon,
    Twilight,  // the default (spec 5.4)
    Night,
};

enum class CameraMode : unsigned {
    Random = 0,
    DistantRidge,
    LowApproach,
    HighOblique,
    StreetLevel,
};

struct Settings {
    Quality    quality   = Quality::Auto;
    TimeOfDay  timeOfDay = TimeOfDay::Twilight;
    CameraMode camera    = CameraMode::Random;
};

// Never fails. Missing, malformed or out-of-range values fall back to the defaults above.
Settings Load();

// Best effort. A failure to write is not worth surfacing to someone configuring a screen saver.
bool Save(const Settings& s);

}  // namespace app

#endif
