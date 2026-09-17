// Selftests for the missile, the flash and the fireball (spec 7.1, 7.2, 5.1).
//
// These are the parts of M5 with numbers in the spec rather than adjectives, and every one of them
// is a number that is wrong silently. A missile that arrives half a second early still looks like
// a missile. A fireball at 5 linear where the spec says 20 to 60 still looks like a fireball, in
// isolation — what it does not do is white out the frame, and by the time that is noticed the
// cause could be anywhere in the exposure chain.

#include "app/settings.h"
#include "core/math.h"
#include "selftest_check.h"
#include "world/camera.h"
#include "world/detonation.h"
#include "world/horizon.h"
#include "world/missile.h"
#include "world/phase.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>

namespace selftest {

using namespace world;

namespace {

// The brightest channel, which is what spec 5.1's table means by an emissive magnitude.
float Magnitude(const core::Vec3& c) {
    return c.x > c.y ? (c.x > c.z ? c.x : c.z) : (c.y > c.z ? c.y : c.z);
}

}  // namespace

void TestMissile() {
    std::printf("missile (spec 7.1)\n");

    bool arrives     = true;
    bool starts      = true;
    bool slowsIn     = true;
    bool neverStalls = true;
    bool descends    = true;
    bool goneAfter   = true;
    bool onlyPhase4  = true;

    for (uint64_t s = 1; s <= 200; ++s) {
        const uint64_t   seed = s * 6364136223846793005ull + 1442695040888963407ull;
        const Timeline   tl   = Timeline::Create(seed);
        const float      radius = 600.0f + static_cast<float>(s % 400);
        const Detonation det    = Detonation::Create(seed, radius, 180.0f);

        // Spec 7.1: it reaches the city centre exactly at the end of the phase.
        if (core::Length(det.MissileAt(tl, tl.End(Phase::Missile)) - det.center) > 0.5f) arrives = false;
        if (core::Length(det.MissileAt(tl, tl.Start(Phase::Missile)) - det.missileStart) > 0.5f) {
            starts = false;
        }

        // Spec 7.1: the missile sheds speed on the way in, monotonically, and never stops. The
        // deceleration is what keeps the final stretch readable now that the run starts kilometres
        // out; a missile that came to a halt on the way would be a worse defect than one that
        // arrived too fast, so both halves are checked.
        const float span  = tl.Duration(Phase::Missile);
        float       prior = -1.0f;
        for (int i = 0; i < 32; ++i) {
            const float t0 = tl.Start(Phase::Missile) + span * (static_cast<float>(i) / 32.0f);
            const float t1 = tl.Start(Phase::Missile) + span * (static_cast<float>(i + 1) / 32.0f);
            const float step = core::Length(det.MissileAt(tl, t1) - det.MissileAt(tl, t0));

            if (prior >= 0.0f && step > prior + 1e-3f) slowsIn = false;
            if (step <= core::Length(det.missileStart - det.center) * 0.008f) neverStalls = false;
            prior = step;
        }

        // A descent, not a drop and not a slide: it covers more ground than height, and enough
        // height to be coming down out of the sky rather than in across the rooftops.
        const core::Vec3 run  = det.center - det.missileStart;
        const float      flat = std::sqrt(run.x * run.x + run.z * run.z);
        if (-run.y >= flat * 1.05f || -run.y <= flat * 0.50f) descends = false;

        // Spec 7.1: destroyed at impact, and MUST NOT be visible in phase 5 or later.
        if (det.MissileVisible(tl, tl.Start(Phase::Flash))) goneAfter = false;
        if (det.MissileVisible(tl, tl.Start(Phase::Blast))) goneAfter = false;
        if (det.MissileVisible(tl, tl.Start(Phase::Gather))) goneAfter = false;
        if (det.MissileVisible(tl, tl.End(Phase::Countdown) - 0.01f)) onlyPhase4 = false;
        if (!det.MissileVisible(tl, tl.Start(Phase::Missile) + span * 0.5f)) onlyPhase4 = false;
    }

    Check(starts, "the missile begins its run off at its entry point");
    Check(arrives, "the missile reaches the city centre exactly at the end of phase 4");
    Check(slowsIn, "the missile sheds speed all the way in and never gains it back");
    Check(neverStalls, "and never stalls short of the impact point");
    Check(descends, "the approach comes down out of the sky without falling vertically");
    Check(goneAfter, "the missile is not visible in phase 5 or later (spec 7.1)");
    Check(onlyPhase4, "the missile is visible during phase 4 and not before it");

    // Spec 7.1 makes the framing a requirement: "Camera framing MUST guarantee the missile is on
    // screen for at least the last 2 s of its run." The world's solver takes a window for it; this
    // asks the solved camera the same question the solver was given.
    bool framed = true;
    for (uint64_t s = 1; s <= 60; ++s) {
        const uint64_t   seed   = s * 0xA24BAULL + 12345ull;
        const Timeline   tl     = Timeline::Create(seed);
        const float      radius = 700.0f + static_cast<float>(s % 300);
        const Detonation det    = Detonation::Create(seed, radius, 180.0f);

        OrbitCamera cam = OrbitCamera::Create(seed, PickShot(seed), radius, tl.total());

        const float      from = tl.End(Phase::Missile) - 2.0f;
        const core::Vec3 at   = det.MissileAt(tl, from);

        Subject missile;
        missile.radius = std::sqrt(at.x * at.x + at.z * at.z);
        missile.height = at.y;

        Subject city;
        city.radius = radius;
        city.height = 180.0f;

        const FramingWindow windows[2] = {{city, 0.0f, tl.End(Phase::Settle)},
                                          {missile, from, tl.End(Phase::Missile)}};
        cam.FrameOn(windows, 2);

        for (int i = 0; i <= 16; ++i) {
            const float t = from + 2.0f * (static_cast<float>(i) / 16.0f);
            if (cam.FramingFill(missile, t) > 1.0f) framed = false;
        }
    }
    Check(framed, "the solved camera keeps the missile in frame for the last 2 s of its run");

    // The mesh. Spec 7.1 asks for a cone nose, a cylinder body and four fins, plus an emissive
    // exhaust; what can be checked here is that it is a mesh rather than a bag of degenerate
    // triangles, and that the emissive channel is on the plume and only on the plume.
    const Mesh mesh = BuildMissileMesh(20260917ull, 56.0f, 4.8f);

    Check(!mesh.empty(), "the missile mesh has geometry");
    Check(mesh.indices.size() % 3 == 0, "the missile mesh is made of whole triangles");

    bool  inRange    = true;
    bool  nonDegenerate = true;
    bool  unitNormals = true;
    float longest    = 0.0f;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size()) {
            inRange = false;
            break;
        }
        const core::Vec3 ab = mesh.vertices[b].position - mesh.vertices[a].position;
        const core::Vec3 ac = mesh.vertices[c].position - mesh.vertices[a].position;
        if (core::Length(core::Cross(ab, ac)) < 1e-6f) nonDegenerate = false;
    }
    for (const Vertex& v : mesh.vertices) {
        if (std::fabs(core::Length(v.normal) - 1.0f) > 1e-3f) unitNormals = false;
        const float reach = core::Length(v.position);
        if (reach > longest) longest = reach;
    }

    Check(inRange, "every missile index names a vertex that exists");
    Check(nonDegenerate, "no missile triangle is degenerate");
    Check(unitNormals, "every missile normal is a unit vector");

    // Nose at the origin, tail at -length, plume behind that: the whole thing lives within a body
    // length or so of the origin, which is what makes the model matrix a rotation and a
    // translation with no offset of its own.
    Check(longest < 56.0f * 2.4f, "the missile mesh stays local to its own nose");

    int emissive = 0, plain = 0;
    for (const Vertex& v : mesh.vertices) {
        if (v.rockiness > 0.0f) {
            ++emissive;
        } else {
            ++plain;
        }
    }
    Check(emissive > 0 && plain > 0, "the plume carries emission and the airframe does not");

    // The transform. It has to be orthonormal or the normals it rotates stop being unit vectors,
    // and it has to put local +Z on the flight path or the missile flies sideways.
    const core::Vec3 direction = core::Normalize(core::Vec3{0.6f, -0.3f, 0.74f});
    const core::Vec3 position{120.0f, 80.0f, -40.0f};
    const core::Mat4 m = MissileTransform(position, direction);

    const core::Vec4 origin = m * core::Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    const core::Vec4 nose   = m * core::Vec4{0.0f, 0.0f, 1.0f, 0.0f};

    Check(core::Length(core::Vec3{origin.x, origin.y, origin.z} - position) < 1e-3f,
          "the missile transform puts the nose at the flight position");
    Check(core::Length(core::Vec3{nose.x, nose.y, nose.z} - direction) < 1e-3f,
          "the missile transform points local +Z along the flight path");

    bool orthonormal = true;
    for (int c = 0; c < 3; ++c) {
        const core::Vec3 axis{m.m[c][0], m.m[c][1], m.m[c][2]};
        if (std::fabs(core::Length(axis) - 1.0f) > 1e-3f) orthonormal = false;
        for (int d = c + 1; d < 3; ++d) {
            const core::Vec3 other{m.m[d][0], m.m[d][1], m.m[d][2]};
            if (std::fabs(core::Dot(axis, other)) > 1e-3f) orthonormal = false;
        }
    }
    Check(orthonormal, "the missile transform is orthonormal, so it rotates normals correctly");

    // Spec 7.1: the missile MUST enter above the mountains, and MUST be in shot doing it.
    // AimApproach is the mechanism, so it is asked the question directly, at every shot height the
    // library reaches and on both sides of the city -- an entry on the camera's own side clears a
    // ridge trivially, and one on the far side has to climb past the whole basin to do it.
    bool clears  = true;
    bool inFrame = true;
    bool banded  = true;
    bool keptAim = true;

    for (uint64_t s = 1; s <= 120; ++s) {
        const uint64_t   seed   = s * 0x9E3779B97F4A7C15ull + 7ull;
        const float      radius = 600.0f + static_cast<float>(s % 400);
        Detonation       det    = Detonation::Create(seed, radius, 180.0f);

        const float bearing = det.missileBearing;
        const float run     = det.missileRun;
        const float ridge   = core::Radians(4.0f + static_cast<float>(s % 7));
        const float margin  = core::Radians(2.0f);
        const float ceiling = core::Radians(9.0f + static_cast<float>(s % 5));

        // Eight camera positions round the orbit, at four heights spanning the shot library.
        const float around = core::kTwoPi * static_cast<float>(s % 8) / 8.0f;
        const float height = radius * (0.02f + 0.83f * static_cast<float>(s % 4));
        const core::Vec3 eye{std::cos(around) * radius * 2.4f, height,
                             std::sin(around) * radius * 2.4f};

        const float got  = det.AimApproach(eye, ridge, margin, ceiling);
        const float want = std::fmin(ridge + margin, ceiling);

        // It hit the target elevation, unless the angle that would have taken it there is outside
        // the band a missile descends at -- in which case it must be sitting *on* a bound, not
        // somewhere convenient in the middle of the band.
        const bool clamped = std::fabs(det.missileDescent - Detonation::kDescentMin) < 1e-5f ||
                             std::fabs(det.missileDescent - Detonation::kDescentMax) < 1e-5f;
        if (std::fabs(got - want) > core::Radians(0.05f) && !clamped) clears = false;

        // Under the top of the frame, unless the descent band would not reach that low. The
        // second is not a nicety: at a steep enough descent the entry sits above everything the
        // camera can see, and the whole approach happens off screen.
        if (got > ceiling + core::Radians(0.05f) && !clamped) inFrame = false;
        if (det.missileDescent < Detonation::kDescentMin - 1e-5f ||
            det.missileDescent > Detonation::kDescentMax + 1e-5f) {
            banded = false;
        }

        // Aiming turns the approach; it does not move it. If it changed the bearing or the run,
        // the arrival the framing was solved against would be somewhere else.
        if (std::fabs(det.missileBearing - bearing) > 1e-5f) keptAim = false;
        if (std::fabs(det.missileRun - run) > 1e-3f) keptAim = false;

        const core::Vec3 line = det.missileStart - det.center;
        const float      flat = std::sqrt(line.x * line.x + line.z * line.z);
        if (std::fabs(std::atan2(line.y, flat) - det.missileDescent) > 1e-3f) keptAim = false;
    }

    Check(clears, "the missile is aimed to enter just clear of the mountains");
    Check(inFrame, "and below the top of the frame, so the approach is on screen");
    Check(banded, "at a descent angle a missile could plausibly hold");
    Check(keptAim, "aiming turns the approach without moving its bearing or its length");

    // And the same question of whole worlds, which is where the ordering can go wrong: the aim is
    // taken against the camera and the range that this cycle actually built, and both of those are
    // decided after the detonation is. Eight is enough to catch a mis-ordering; it is not a survey.
    bool worldsClear  = true;
    bool worldsFramed = true;
    bool worldsAhead  = true;
    for (uint64_t s = 1; s <= 40; ++s) {
        app::Settings settings;
        settings.camera = static_cast<app::CameraMode>(s % 4 == 0 ? 1 : (s % 4) + 1);

        const world::World w = world::Generate(settings, s * 0xA24BAull + 991ull);

        // The peaks this cycle shipped: GenerateClosedRange left its widened parameters in
        // world.horizon, and the generator is seeded, so this is the same range.
        const std::vector<Peak> peaks = GeneratePeaks(w.horizon);

        const CameraState view = w.camera.Evaluate(w.timeline.Start(Phase::Missile));
        const float ridge = SilhouetteElevation(peaks, view.eye, w.detonation.missileBearing);
        const float got   = w.detonation.MissileEntryElevation(view.eye);

        // Derived here the way world.cpp derives it, which is the point of asking: the frame
        // the entry was aimed into has to be this camera's rather than a constant that happened
        // to suit the shots in the loop.
        const core::Vec3 toTarget = view.target - view.eye;
        const float pitch = std::atan2(
            toTarget.y, std::fmax(std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z),
                                  1.0f));
        const float top = pitch + view.fovY * 0.5f;

        // Whether this shot has room for a missile above the range at all. The steep shots look
        // down into the basin and carry no sky; a few carry a sliver, with the summits sitting in
        // it. Neither can show an entry over the mountains, and spec 7.1 says being on screen wins
        // in that case -- so the requirement asked of them below is the other one.
        const bool room = ridge + core::Radians(2.0f) <= pitch + view.fovY * 0.44f;

        // Over the range and inside the frame -- except on a shot whose frame does not reach the
        // range at all, which is the one case where the two cannot both hold and spec 7.1 says
        // the frame wins. Written as a branch rather than as a skip, so a shot that quietly
        // stopped showing the horizon would not quietly stop being checked: it would move to the
        // other arm, where being on screen is still required of it.
        if (room) {
            if (got <= ridge) worldsClear = false;
            if (got > top) worldsFramed = false;
        } else if (got > top) {
            // No room: the entry is above the frame and comes down into it. Acceptable only when
            // nothing could have been done about it -- that is, when the descent is already as
            // shallow as a missile is allowed to hold. Anything else is an aim that gave up early.
            if (std::fabs(w.detonation.missileDescent - Detonation::kDescentMin) > 1e-5f) {
                worldsFramed = false;
            }
        }

        // And on screen sideways as well as vertically. A bearing is drawn at random, and a random
        // bearing is sometimes the one the camera has its back to; the aim walks the bearing round
        // until the entry is in shot, and this is the question that walk exists to answer.
        const float halfWide = std::atan(std::tan(view.fovY * 0.5f) * 16.0f / 9.0f);
        if (w.detonation.MissileEntryOffAxis(view.eye, core::Normalize(toTarget)) > halfWide) {
            worldsAhead = false;
        }
    }
    Check(worldsClear, "and in a built world the missile enters above the range behind it");
    Check(worldsFramed, "and inside the frame the camera will be showing when it does");
    Check(worldsAhead, "and ahead of the camera rather than off the side of the screen");
}

void TestFireAndFlash() {
    std::printf("flash and fireball (spec 7.2, 5.1, 8.2)\n");

    bool flashOnlyInPhase5 = true;
    bool flashPeak         = true;
    bool flashHolds        = true;
    bool fireWindow        = true;
    bool fireStart         = true;
    bool fireEnd           = true;
    bool fireCools         = true;
    bool fireRises         = true;
    bool fireBigEnough     = true;

    for (uint64_t s = 1; s <= 200; ++s) {
        const uint64_t   seed   = s * 6364136223846793005ull + 1442695040888963407ull;
        const Timeline   tl     = Timeline::Create(seed);
        const float      radius = 600.0f + static_cast<float>(s % 400);
        const Detonation det    = Detonation::Create(seed, radius, 180.0f);

        // Spec 7.2: white over ~120 ms, held for the rest of phase 5, nothing outside it.
        if (det.FlashIntensity(tl, tl.Start(Phase::Flash) - 0.01f) != 0.0f) {
            flashOnlyInPhase5 = false;
        }
        if (det.FlashIntensity(tl, tl.End(Phase::Flash)) != 0.0f) flashOnlyInPhase5 = false;

        const float peak = det.FlashIntensity(tl, tl.Start(Phase::Flash) + 0.12f);
        if (peak < 8000.0f || peak > 15000.0f) flashPeak = false;  // spec 5.1

        const float late = det.FlashIntensity(tl, tl.End(Phase::Flash) - 0.001f);
        if (late < peak * 0.99f) flashHolds = false;

        // The fireball exists from the flash to the end of the scatter and nowhere else.
        if (det.FireRadius(tl, tl.Start(Phase::Flash) - 0.01f) != 0.0f) fireWindow = false;
        if (det.FireRadius(tl, tl.End(Phase::Scatter)) != 0.0f) fireWindow = false;
        if (det.FireRadius(tl, tl.Start(Phase::Blast)) <= 0.0f) fireWindow = false;

        // Spec 5.1: 2,000-4,000 at the start of phase 6, 20-60 at its end.
        const float begin = Magnitude(det.FireColor(tl, tl.Start(Phase::Blast)));
        if (begin < 2000.0f || begin > 4000.0f) fireStart = false;

        const float finish = Magnitude(det.FireColor(tl, tl.End(Phase::Blast)));
        if (finish < 20.0f || finish > 60.0f) fireEnd = false;

        // It only ever cools, and it only ever rises.
        float previousMagnitude = 1e9f;
        float previousHeight    = -1e9f;
        for (int i = 0; i <= 64; ++i) {
            const float t = tl.Start(Phase::Flash) +
                            (tl.End(Phase::Scatter) - tl.Start(Phase::Flash)) *
                                (static_cast<float>(i) / 64.0f);

            const float m = Magnitude(det.FireColor(tl, t));
            if (m > previousMagnitude + 1e-3f) fireCools = false;
            previousMagnitude = m;

            const float h = det.FireCenter(tl, t).y;
            if (h < previousHeight - 1e-3f) fireRises = false;
            previousHeight = h;
        }

        // Spec 7.2 makes it the dominant light of phases 6 and 7, which it cannot be if it is a
        // speck: it has to cover the middle of the city.
        float widest = 0.0f;
        for (int i = 0; i <= 64; ++i) {
            const float t = tl.Start(Phase::Flash) +
                            (tl.End(Phase::Scatter) - tl.Start(Phase::Flash)) *
                                (static_cast<float>(i) / 64.0f);
            const float r = det.FireRadius(tl, t);
            if (r > widest) widest = r;
        }
        if (widest < radius * 0.2f) fireBigEnough = false;
    }

    Check(flashOnlyInPhase5, "the flash exists only during phase 5");
    Check(flashPeak, "the flash peaks at 8,000 to 15,000 linear (spec 5.1)");
    Check(flashHolds, "the flash holds at its peak for the rest of phase 5 (spec 7.2)");
    Check(fireWindow, "the fireball exists from the flash to the end of the scatter and no longer");
    Check(fireStart, "the fireball starts phase 6 at 2,000 to 4,000 linear (spec 5.1)");
    Check(fireEnd, "the fireball ends phase 6 at 20 to 60 linear (spec 5.1)");
    Check(fireCools, "the fireball only ever cools");
    Check(fireRises, "the fireball only ever rises");
    Check(fireBigEnough, "the fireball covers the middle of the city, as a dominant light must");
}

}  // namespace selftest
