// Selftests for the pure-CPU core: math, deterministic randomness, noise, colour.
//
// All of this is generation logic, which makes it exactly the part of the project that can be
// checked without a GPU or a desktop — and, given that live verification on this machine has
// already been blocked once by the input desktop, the part worth checking properly.

#include "core/color.h"
#include "core/math.h"
#include "core/noise.h"
#include "core/rng.h"
#include "selftest_check.h"

#include <cmath>
#include <cstdio>

namespace selftest {

using namespace core;

void TestMath() {
    std::printf("math (implementation plan M3)\n");

    Check(Length(Normalize(Vec3{3.0f, 4.0f, 0.0f})) > 0.999f, "normalize gives unit length");
    // A zero vector must not produce NaN: directions here come from random data, and one
    // degenerate sample should not poison a frame.
    Check(Length(Normalize(Vec3{0.0f, 0.0f, 0.0f})) == 0.0f, "normalize of zero is zero, not NaN");

    Check(Cross(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}).z > 0.999f,
          "cross is right-handed (x cross y = +z)");

    const Vec4 v = Mat4::Identity() * Vec4{1.0f, 2.0f, 3.0f, 1.0f};
    Check(v.x == 1.0f && v.y == 2.0f && v.z == 3.0f, "identity leaves a vector alone");

    // Translation must land in the column the GPU reads it from. A transposed convention is
    // invisible until geometry turns up in the wrong place.
    const Vec4 t = Translate(Vec3{5.0f, 6.0f, 7.0f}) * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    Check(t.x == 5.0f && t.y == 6.0f && t.z == 7.0f, "translate is column-major");

    // A camera at +Z looking at the origin puts the target down -Z in view space.
    const Mat4 view =
        LookAt(Vec3{0.0f, 0.0f, 10.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    const Vec4 originInView = view * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    CheckNear(originInView.z, -10.0f, 1e-4f, "lookAt puts the target 10m down -Z");
    CheckNear(originInView.x, 0.0f, 1e-4f, "lookAt centres the target in x");

    // Looking straight down with world up as the hint is degenerate. It must still produce a
    // usable basis: the high oblique shot in the camera library gets close to this.
    const Mat4 down =
        LookAt(Vec3{0.0f, 10.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    const Vec4 dv = down * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    Check(!std::isnan(dv.x) && !std::isnan(dv.y) && !std::isnan(dv.z),
          "lookAt straight down is not NaN");

    // Vulkan clip space: near maps to z=0, far to z=1, Y already flipped so nothing downstream
    // repeats the correction.
    const Mat4 proj  = Perspective(Radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    const Vec4 nearP = proj * Vec4{0.0f, 0.0f, -0.1f, 1.0f};
    const Vec4 farP  = proj * Vec4{0.0f, 0.0f, -100.0f, 1.0f};
    CheckNear(nearP.z / nearP.w, 0.0f, 1e-4f, "perspective maps the near plane to z=0");
    CheckNear(farP.z / farP.w, 1.0f, 1e-4f, "perspective maps the far plane to z=1");
    Check(proj.m[1][1] < 0.0f, "perspective flips Y for Vulkan NDC");

    // Growth (spec 6.5) must decelerate into place and must not overshoot or bounce.
    CheckNear(EaseOutCubic(0.0f), 0.0f, 1e-6f, "ease-out starts at 0");
    CheckNear(EaseOutCubic(1.0f), 1.0f, 1e-6f, "ease-out ends at 1");
    Check(EaseOutCubic(0.5f) > 0.5f, "ease-out is fast early");

    bool overshoot = false;
    for (int i = 0; i <= 100; ++i) {
        const float e = EaseOutCubic(static_cast<float>(i) / 100.0f);
        if (e > 1.0f + 1e-6f || e < -1e-6f) overshoot = true;
    }
    Check(!overshoot, "ease-out never overshoots (spec 6.5)");
}

void TestRng() {
    std::printf("deterministic randomness (spec 5.2)\n");

    // The point of hashing rather than streaming: a value depends on (seed, id, channel) and
    // nothing else, so adding one building cannot reshuffle the colour of its neighbours.
    Check(HashFloat(99, 7, 0) == HashFloat(99, 7, 0), "hash is stable for the same inputs");
    Check(HashFloat(99, 7, 0) != HashFloat(99, 8, 0), "different ids differ");
    Check(HashFloat(99, 7, 0) != HashFloat(100, 7, 0), "different seeds differ");
    Check(HashFloat(99, 7, 0) != HashFloat(99, 7, 1), "different channels differ");

    bool      inRange = true, sawLow = false, sawHigh = false;
    double    sum = 0.0;
    const int kN  = 20000;
    for (int i = 0; i < kN; ++i) {
        const float f = HashFloat(1234, static_cast<uint64_t>(i), 0);
        if (f < 0.0f || f >= 1.0f) inRange = false;
        if (f < 0.1f) sawLow = true;
        if (f > 0.9f) sawHigh = true;
        sum += f;
    }
    Check(inRange, "hash floats stay in [0,1)");
    Check(sawLow && sawHigh, "hash floats cover the range");
    CheckNear(static_cast<float>(sum / kN), 0.5f, 0.02f, "hash floats are uniform on average");

    // Signed draws carry the per-instance variation of spec 5.2, so a biased one would tilt
    // every colour in the scene the same way.
    double signedSum = 0.0;
    for (int i = 0; i < kN; ++i) signedSum += HashSigned(777, static_cast<uint64_t>(i), 3);
    CheckNear(static_cast<float>(signedSum / kN), 0.0f, 0.03f, "signed draws are centred on 0");

    Rng a(42), b(42);
    Check(a.NextU64() == b.NextU64(), "same seed gives the same stream");

    Rng forked = Rng(42).Fork(1);
    Rng other  = Rng(42).Fork(2);
    Check(forked.NextU64() != other.NextU64(), "different fork tags give different streams");

    // Uniform on the sphere, not on Euler angles: sampling angles clusters at the poles, which
    // shows up as a seam in fragment scatter.
    Rng   dirs(5150);
    float meanY = 0.0f;
    bool  unit  = true;
    for (int i = 0; i < 5000; ++i) {
        const Vec3 d = dirs.UnitVector();
        if (std::fabs(Length(d) - 1.0f) > 1e-3f) unit = false;
        meanY += d.y;
    }
    Check(unit, "UnitVector returns unit length");
    CheckNear(meanY / 5000.0f, 0.0f, 0.05f, "UnitVector is not pole-biased");
}

void TestNoise() {
    std::printf("noise (spec 6.2)\n");

    Check(GradientNoise2(1.5f, 2.5f, 7) == GradientNoise2(1.5f, 2.5f, 7), "noise is deterministic");
    Check(GradientNoise2(1.5f, 2.5f, 7) != GradientNoise2(1.5f, 2.5f, 8), "seed changes the field");

    // Gradient noise is zero at the lattice points by construction. A non-zero value here means
    // the gradients are not being dotted against the offset correctly.
    CheckNear(GradientNoise2(3.0f, 4.0f, 11), 0.0f, 1e-5f, "gradient noise is 0 at lattice points");

    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 4000; ++i) {
        const float n =
            GradientNoise2(static_cast<float>(i) * 0.037f, static_cast<float>(i) * 0.021f, 3);
        lo = std::fmin(lo, n);
        hi = std::fmax(hi, n);
    }
    Check(lo > -1.05f && hi < 1.05f, "gradient noise stays near [-1,1]");
    Check(lo < -0.3f && hi > 0.3f, "gradient noise actually varies");

    // Continuity across lattice boundaries. A field that jumps between adjacent samples creases
    // the terrain normal, and terrain here is normal-mapped.
    float maxJump = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const float x  = static_cast<float>(i) * 0.001f;
        const float n0 = GradientNoise2(x, 0.3f, 21);
        const float n1 = GradientNoise2(x + 0.001f, 0.3f, 21);
        maxJump        = std::fmax(maxJump, std::fabs(n1 - n0));
    }
    Check(maxJump < 0.05f, "gradient noise is continuous across lattice boundaries");

    FbmParams p;
    p.octaves   = 6;
    p.frequency = 0.5f;

    float flo = 1e9f, fhi = -1e9f;
    for (int i = 0; i < 4000; ++i) {
        const float n = Fbm2(static_cast<float>(i) * 0.013f, static_cast<float>(i) * 0.029f, 5, p);
        flo           = std::fmin(flo, n);
        fhi           = std::fmax(fhi, n);
    }
    Check(flo > -1.05f && fhi < 1.05f, "fbm stays normalised");
    Check(fhi - flo > 0.5f, "fbm actually varies");

    // Ridged noise exists to make crests. It has to be one-sided, or it is just fbm again.
    bool ridgedPositive = true, ridgedPeaks = false;
    for (int i = 0; i < 4000; ++i) {
        const float n = Ridged2(static_cast<float>(i) * 0.017f, static_cast<float>(i) * 0.011f, 9, p);
        if (n < -1e-5f) ridgedPositive = false;
        if (n > 0.75f) ridgedPeaks = true;
    }
    Check(ridgedPositive, "ridged noise is non-negative");
    Check(ridgedPeaks, "ridged noise produces crests");

    // Curl is used because it is divergence-free, which is what stops particles piling into
    // sinks. What makes that true is structural - the field is the perpendicular of a gradient,
    // and div(curl) is identically zero for any scalar potential - so what is worth checking is
    // that the structure is actually what the code builds. A sign error, or returning the
    // gradient itself, is the failure this catches.
    //
    // It is measured at the field's own differencing step. Measuring at some other step does not
    // test the field: the two mixed partials would then be taken over different rectangles, and
    // the residual is discretisation error on a field carrying content up to frequency 16, not
    // divergence. Probing that mistake is what turned up the step being wrong in the first place.
    const float step   = Curl2Step(p);
    float       maxDiv = 0.0f;
    for (int i = 1; i < 200; ++i) {
        const float x = static_cast<float>(i) * 0.05f, y = static_cast<float>(i) * 0.03f;
        const Vec2  cx0 = Curl2(x - step, y, 4, p), cx1 = Curl2(x + step, y, 4, p);
        const Vec2  cy0 = Curl2(x, y - step, 4, p), cy1 = Curl2(x, y + step, 4, p);
        const float div = (cx1.x - cx0.x) / (2.0f * step) + (cy1.y - cy0.y) / (2.0f * step);
        maxDiv          = std::fmax(maxDiv, std::fabs(div));
    }
    Check(maxDiv < 1e-3f, "curl field is divergence-free at its own step");

    // ...and it must be a real field, not a constant one that trivially satisfies the above.
    float speedLo = 1e9f, speedHi = -1e9f;
    for (int i = 0; i < 500; ++i) {
        const Vec2 c = Curl2(static_cast<float>(i) * 0.021f, static_cast<float>(i) * 0.013f, 4, p);
        const float speed = Length(c);
        speedLo = std::fmin(speedLo, speed);
        speedHi = std::fmax(speedHi, speed);
    }
    Check(speedHi > 0.1f, "curl field is non-trivial");
    Check(speedHi - speedLo > 0.05f, "curl field varies in magnitude");

    // The step must resolve the finest octave. A coarser one returns the curl of a smoother
    // field than the caller asked for, which is exactly the bug this value was tuned to avoid.
    Check(Curl2Step(p) < 0.5f / FbmMaxFrequency(p), "curl step resolves the finest octave");
}

void TestColor() {
    std::printf("colour system (spec section 5)\n");

    const Vec3 rgb  = HsvToRgb(Hsv{210.0f, 0.5f, 0.8f});
    const Hsv  back = RgbToHsv(rgb);
    CheckNear(back.h, 210.0f, 0.5f, "HSV round trip preserves hue");
    CheckNear(back.s, 0.5f, 0.01f, "HSV round trip preserves saturation");
    CheckNear(back.v, 0.8f, 0.01f, "HSV round trip preserves value");

    const Vec3 grey = HsvToRgb(Hsv{123.0f, 0.0f, 0.5f});
    CheckNear(grey.x, grey.y, 1e-6f, "zero saturation is neutral");
    CheckNear(grey.y, grey.z, 1e-6f, "zero saturation is neutral in blue too");

    // The sRGB transfer function. 0.5 is the value that tells a correct decode from a 2.2 power
    // approximation, and getting it wrong is the gamma bug the plan warns about.
    CheckNear(SrgbToLinear(0.0f), 0.0f, 1e-6f, "sRGB decode maps 0 to 0");
    CheckNear(SrgbToLinear(1.0f), 1.0f, 1e-6f, "sRGB decode maps 1 to 1");
    CheckNear(SrgbToLinear(0.5f), 0.2140f, 1e-3f, "sRGB decode of 0.5 is 0.214");
    CheckNear(LinearToSrgb(SrgbToLinear(0.37f)), 0.37f, 1e-5f, "sRGB transfer round trips");

    // Per-instance variation (spec 5.2): stable within a cycle, different between instances.
    const Vec3 c0 = Albedo(palette::kBuildingBody, 1234, 17);
    const Vec3 c1 = Albedo(palette::kBuildingBody, 1234, 17);
    Check(c0.x == c1.x && c0.y == c1.y && c0.z == c1.z, "instance colour is stable within a cycle");

    const Vec3 c2 = Albedo(palette::kBuildingBody, 1234, 18);
    Check(c0.x != c2.x || c0.y != c2.y || c0.z != c2.z, "neighbouring instances differ");

    const Vec3 c3 = Albedo(palette::kBuildingBody, 9999, 17);
    Check(c0.x != c3.x || c0.y != c3.y || c0.z != c3.z, "a new cycle seed gives a new city");

    // A building's body and window slots must be independent, or every building's windows track
    // its walls and the city reads as tinted rather than varied.
    const Vec3 body = Albedo(palette::kBuildingBody, 55, 3, 0);
    const Vec3 win  = Albedo(palette::kBuildingWindow, 55, 3, 1);
    Check(body.x != win.x || body.z != win.z, "palette slots are independent for one instance");

    // Spread. "500 boxes in one grey" is the failure spec 5.2 exists to prevent.
    float vlo = 1e9f, vhi = -1e9f, hlo = 1e9f, hhi = -1e9f;
    for (uint64_t i = 0; i < 500; ++i) {
        const Hsv h = AlbedoHsv(palette::kBuildingBody, 2026, i);
        vlo         = std::fmin(vlo, h.v);
        vhi         = std::fmax(vhi, h.v);
        hlo         = std::fmin(hlo, h.h);
        hhi         = std::fmax(hhi, h.h);
    }
    Check(vhi - vlo > 0.25f, "500 buildings span a real value range");
    Check(hhi - hlo > 10.0f, "500 buildings span a real hue range");

    // ...and stay inside the authored bounds plus the stated tolerance, so "vary" never becomes
    // "escape the palette". Body value is 0.35 to 0.70 at 10 percent.
    Check(vlo >= 0.35f * 0.9f - 1e-4f && vhi <= 0.70f * 1.1f + 1e-4f,
          "variation stays within the palette range plus its tolerance");

    // A degenerate hue range - the board frame is grey - must still vary rather than divide by
    // zero or collapse to one hue.
    float flo = 1e9f, fhi = -1e9f;
    bool  hueFinite = true;
    for (uint64_t i = 0; i < 200; ++i) {
        const Hsv h = AlbedoHsv(palette::kBoardFrame, 11, i);
        if (std::isnan(h.h)) hueFinite = false;
        flo = std::fmin(flo, h.v);
        fhi = std::fmax(fhi, h.v);
    }
    Check(hueFinite, "a degenerate hue range is not NaN");
    Check(fhi - flo > 0.02f, "a grey palette slot still varies in value");

    // Lit segments are tightened precisely because mismatched glyphs read as a bug rather than
    // as variety.
    float slo = 1e9f, shi = -1e9f;
    for (uint64_t i = 0; i < 8; ++i) {
        const Hsv h = AlbedoHsv(palette::kSegmentLit, 3, i);
        slo         = std::fmin(slo, h.v);
        shi         = std::fmax(shi, h.v);
    }
    Check(shi - slo < 0.25f, "the eight glyphs stay visually matched (spec 5.3)");

    CheckNear(Desaturate(Hsv{30.0f, 0.5f, 0.4f}, 0.4f).s, 0.3f, 1e-5f, "desaturate 40 percent");
    CheckNear(Darken(Hsv{30.0f, 0.5f, 0.4f}, 0.4f).v, 0.24f, 1e-5f, "darken 40 percent");
}

}  // namespace selftest
