// Console harness for the toolchain probe and the host's window-free logic.
//
// The shipped .scr is a Windows-subsystem binary with no stdout and, per spec 9.1, exits 0 on
// any argument it does not recognise — so it is the wrong place to put a diagnostic mode. This
// builds as a separate console executable (`make selftest`) that links the same objects.
//
// Argument parsing is checked here rather than by launching the screen saver, because launching
// it to test `/s` blanks the developer's display, and launching a `.scr` through the shell fires
// the file type's default verb instead of passing the arguments through at all.

#include "app/args.h"
#include "app/input_watcher.h"
#include "app/settings.h"
#include "render/vk_probe.h"
#include "selftest_check.h"

#include <cstdio>
#include <cwchar>
#include <vector>

namespace {

using selftest::Check;

app::Args ParseLine(std::initializer_list<const wchar_t*> tail) {
    std::vector<const wchar_t*> argv{L"nuke-saver.scr"};
    for (const wchar_t* t : tail) argv.push_back(t);
    return app::Parse(static_cast<int>(argv.size()), argv.data());
}

void TestArgs() {
    std::printf("argument parsing (spec 9.1)\n");

    Check(ParseLine({}).mode == app::Mode::FullScreen, "no arguments -> full screen");
    Check(ParseLine({L"/s"}).mode == app::Mode::FullScreen, "/s -> full screen");
    Check(ParseLine({L"-s"}).mode == app::Mode::FullScreen, "-s -> full screen");
    Check(ParseLine({L"/S"}).mode == app::Mode::FullScreen, "/S -> full screen (case)");

    Check(ParseLine({L"/c"}).mode == app::Mode::Configure, "/c -> configure");
    Check(ParseLine({L"/C"}).mode == app::Mode::Configure, "/C -> configure (case)");
    Check(ParseLine({L"/c:1234"}).mode == app::Mode::Configure, "/c:hwnd -> configure");
    Check(ParseLine({L"/c:1234"}).parentHandle == 1234, "/c:1234 parses the handle");
    Check(ParseLine({L"/c", L"5678"}).parentHandle == 5678, "/c hwnd parses the handle");

    Check(ParseLine({L"/p:4321"}).mode == app::Mode::Preview, "/p:hwnd -> preview");
    Check(ParseLine({L"/p:4321"}).parentHandle == 4321, "/p:4321 parses the handle");
    Check(ParseLine({L"/p", L"999"}).parentHandle == 999, "/p hwnd parses the handle");
    Check(ParseLine({L"/p"}).mode == app::Mode::Exit, "/p with no handle -> exit");
    Check(ParseLine({L"/p:0"}).mode == app::Mode::Exit, "/p:0 -> exit");

    Check(ParseLine({L"/bogus"}).mode == app::Mode::Exit, "/bogus -> exit");
    Check(ParseLine({L"-zzz"}).mode == app::Mode::Exit, "-zzz -> exit");
    Check(ParseLine({L"/q"}).mode == app::Mode::Exit, "/q -> exit");
    Check(ParseLine({L""}).mode == app::Mode::Exit, "empty argument -> exit");

    // Documented consequence of matching ghost-saver exactly: the prefix is optional and only
    // the first character is examined, so a bare word beginning with c, p or s selects a mode.
    // Screen-saver hosts never pass one, and the spec makes ghost-saver authoritative here.
    Check(ParseLine({L"config"}).mode == app::Mode::Configure,
          "bare 'config' -> configure (ghost-saver compatibility)");
}

// The exit path is the most important logic in the program: if it breaks, the user cannot get
// their desktop back. It also cannot be exercised against the real OS from a test harness,
// because only a process on the input desktop can read the keyboard or the cursor. So it is
// tested here against synthetic samples instead.
void TestInputWatcher() {
    std::printf("input exit policy (spec 9.2)\n");

    const double after = app::InputWatcher::kGraceSeconds + 0.1;

    auto sampleAt = [](int x, int y) {
        app::InputSample s;
        s.cursorValid = true;
        s.cursor.x    = x;
        s.cursor.y    = y;
        return s;
    };
    auto sampleKey = [](int vk) {
        app::InputSample s;
        s.cursorValid  = true;
        s.keyDown[vk]  = true;
        return s;
    };

    {   // Nothing during the grace period counts, however loud it is.
        app::InputWatcher w;
        Check(!w.Consider(0.0, sampleKey(VK_SPACE)), "key during grace is ignored");
        Check(!w.Consider(0.4, sampleKey(VK_RETURN)), "key at 0.4s is ignored");
        Check(!w.primed(), "no priming before the grace period ends");
    }

    {   // A key already held when the saver starts must not dismiss it.
        app::InputWatcher w;
        Check(!w.Consider(after, sampleKey(VK_RETURN)), "held key primes rather than exits");
        Check(w.primed(), "primed after first post-grace sample");
        Check(!w.Consider(after + 0.1, sampleKey(VK_RETURN)), "still-held key does not exit");
        Check(!w.Consider(after + 0.2, app::InputSample{}), "release does not exit");
        Check(w.Consider(after + 0.3, sampleKey(VK_RETURN)), "press after release exits");
    }

    {   // A fresh key press exits.
        app::InputWatcher w;
        Check(!w.Consider(after, app::InputSample{}), "quiet first sample primes");
        Check(w.Consider(after + 0.1, sampleKey(VK_SPACE)), "new key press exits");
    }

    {   // Mouse: jitter inside the dead zone is not input; a real move is.
        app::InputWatcher w;
        Check(!w.Consider(after, sampleAt(500, 500)), "cursor reference primed");
        Check(!w.Consider(after + 0.1, sampleAt(504, 503)), "jitter inside dead zone ignored");
        Check(!w.Consider(after + 0.2, sampleAt(500, 500)), "return to origin ignored");
        Check(w.Consider(after + 0.3, sampleAt(560, 540)), "move beyond dead zone exits");
    }

    {   // An unreadable cursor is an unknown, never a reason to exit, and the first readable
        // position afterwards becomes the reference rather than looking like a jump.
        app::InputWatcher w;
        app::InputSample blind;
        blind.cursorValid = false;
        Check(!w.Consider(after, blind), "unreadable cursor primes without exiting");
        Check(!w.Consider(after + 0.1, blind), "still unreadable, still no exit");
        Check(!w.Consider(after + 0.2, sampleAt(900, 900)), "first readable position is adopted");
        Check(!w.Consider(after + 0.3, sampleAt(902, 901)), "jitter after adoption ignored");
        Check(w.Consider(after + 0.4, sampleAt(980, 980)), "real move after adoption exits");
    }

    {   // Mouse buttons arrive as virtual keys and must behave like any other press.
        app::InputWatcher w;
        Check(!w.Consider(after, app::InputSample{}), "quiet prime");
        Check(w.Consider(after + 0.1, sampleKey(VK_LBUTTON)), "left button exits");
    }
}

void TestSettings() {
    std::printf("settings (spec section 10)\n");

    app::Settings defaults;
    Check(defaults.timeOfDay == app::TimeOfDay::Twilight, "default time of day is twilight");
    Check(defaults.quality == app::Quality::Auto, "default quality is automatic");
    Check(defaults.camera == app::CameraMode::Random, "default camera is random");

    // Load() must never fail, whatever is or is not in the registry.
    app::Settings loaded = app::Load();
    Check(static_cast<unsigned>(loaded.quality) <= 3, "loaded quality in range");
    Check(static_cast<unsigned>(loaded.timeOfDay) <= 4, "loaded time of day in range");
    Check(static_cast<unsigned>(loaded.camera) <= 4, "loaded camera in range");
}

void TestProbe() {
    std::printf("vulkan probe (implementation plan M0)\n");

    probe::Result r = probe::Run();
    std::printf("  shaders embedded : %u blob(s), %u bytes\n", r.shaderCount, r.shaderBytes);
    std::printf("  stage reached    : %s\n", r.stage.c_str());
    std::printf("  physical devices : %u\n", r.deviceCount);
    if (!r.apiVersion.empty()) std::printf("  device api       : %s\n", r.apiVersion.c_str());
    std::printf("  detail           : %s\n", r.detail.c_str());

    Check(r.shaderCount > 0, "at least one shader blob is linked in");
    Check(r.ok, "probe completed");
}

}  // namespace

// Suites living in their own translation units.
namespace selftest {
void TestMath();
void TestRng();
void TestNoise();
void TestColor();
void TestSky();
void TestCamera();
void TestTerrain();
void TestHorizon();
void TestCity();
void TestUnitCube();
void TestFraming();
}  // namespace selftest

int main() {
    std::printf("nuke-saver selftest\n");
    std::printf("-------------------\n");

    selftest::TestMath();
    selftest::TestRng();
    selftest::TestNoise();
    selftest::TestColor();
    selftest::TestSky();
    selftest::TestCamera();
    selftest::TestTerrain();
    selftest::TestHorizon();
    selftest::TestCity();
    selftest::TestUnitCube();
    selftest::TestFraming();
    TestArgs();
    TestInputWatcher();
    TestSettings();
    TestProbe();

    std::printf("-------------------\n");
    std::printf("%d checks, %d failure(s): %s\n", selftest::Checks(),
                selftest::Failures(), selftest::Failures() == 0 ? "PASS" : "FAIL");
    return selftest::Failures() == 0 ? 0 : 1;
}
