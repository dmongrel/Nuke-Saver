#include "world/detonation.h"

#include "core/rng.h"

#include <cmath>

namespace world {

Detonation Detonation::Create(uint64_t seed, float cityRadius, float tallestBuilding) {
    core::Rng rng = core::Rng(seed).Fork(0xB1A57ull);

    Detonation det;
    // An airburst, a little under half the height of the tallest building. At ground level the
    // last second of the missile's run was inside the skyline and the impact happened behind a row
    // of buildings — spec 7.1 spends a phase getting the missile there, so the arrival has to be
    // visible. It also lifts the fireball clear of the near rooftops, which is what lets it light
    // the far side of the city instead of only the block underneath it.
    det.center = core::Vec3{0.0f, tallestBuilding * 0.45f, 0.0f};

    // Past the far edge of the city, so nothing is left standing when phase 6 ends. The board is
    // inside the city's radius by construction, so one number covers both.
    det.reach = cityRadius * 1.35f;

    // Proportioned against the city rather than fixed, for the same reason the board is: the
    // cloud has to read as having come from this city, and a 2 km cap over an 800 m town reads as
    // weather. These are the dimensions at full growth.
    // These are the dimensions at full growth, and they are a compromise the framing forces.
    // Spec 11.1 makes the cloud the binding constraint on the orbit radius, so every metre of
    // cloud pushes the camera back and shrinks the city for the half of the cycle before the
    // cloud exists. A cloud about as tall as the city is wide is as far as that trade goes.
    det.stemHeight = cityRadius * rng.Range(0.62f, 0.80f);
    det.capRadius  = cityRadius * rng.Range(0.42f, 0.55f);
    det.capTube    = det.capRadius * rng.Range(0.50f, 0.62f);
    det.capHeight  = det.stemHeight + det.capTube * 0.55f;

    const float bearing = rng.Range(0.0f, core::kPi * 2.0f);
    const float speed   = rng.Range(5.0f, 11.0f);
    det.wind = core::Vec3{std::cos(bearing) * speed, 0.0f, std::sin(bearing) * speed};

    // Spec 7.1: a random compass bearing, and a descent steep enough that the missile comes down
    // out of the sky rather than sliding in across the rooftops. The old approach ran 1.7 city
    // radii at 20 degrees, which put the entry point at about 500 m — below the mountain
    // silhouette from every shot that stands higher than a truck, so the missile appeared against
    // the range instead of over it. Coming over the mountains is a statement about the entry's
    // *elevation as seen from the camera*, and the camera does not exist yet, so all Create fixes
    // here is the bearing, the angle, and a run long enough to be going somewhere. AimOverRidge
    // stretches it once the shot is solved.
    det.missileBearing = rng.Range(0.0f, core::kPi * 2.0f);
    det.missileDescent = rng.Range(core::Radians(30.0f), core::Radians(44.0f));
    det.SetMissileRun(cityRadius * 3.2f);

    // Big enough to read at this distance and no bigger. A missile drawn at true scale against a
    // 1.5 km city is two pixels, and at a fifteenth of the city radius it was still lost against
    // the countdown board it flies past: spec 7.1 spends a whole phase on this object, so it is
    // sized to be seen rather than to be right.
    det.missileLength = cityRadius * 0.260f;
    det.missileRadius = det.missileLength * 0.085f;

    // The fireball of spec 7.2, which has to swallow the middle of the city to be the dominant
    // light and not so much that it hides the shell crossing the outskirts.
    det.fireRadius = cityRadius * rng.Range(0.26f, 0.34f);
    det.fireRise   = det.stemHeight * 0.35f;

    return det;
}

float Detonation::ShellRadius(const Timeline& timeline, float t) const {
    const float start = timeline.Start(Phase::Blast);
    if (t < start) return 0.0f;

    const float span = timeline.Duration(Phase::Blast);
    if (span <= 0.0f) return reach;

    // Fast then decelerating (spec 7.2). The exponential reaches 98% of `reach` by the end of the
    // phase, and the remaining 2% is why `reach` is past the city rather than at its edge: the
    // requirement is that the shell crosses the city inside the phase, not that it stops there.
    // The rate is what decides whether the scatter has time to land. Spec 7.2 only requires the
    // shell to cross the city inside phase 6, but a shell that takes the whole phase leaves the
    // outskirts shattering with two seconds left of phase 7 — and spec 7.4 wants the frame at the
    // end of phase 7 to read as flat debris, which those fragments are still a long way from.
    // At this rate the shell is across the city by two fifths of the phase.
    const float u = (t - start) / span;
    return reach * (1.0f - std::exp(-6.5f * u));
}

float Detonation::CloudGrow(const Timeline& timeline, float t) const {
    const float start = timeline.Start(Phase::Gather);
    if (t < start) return 0.0f;

    const float span = timeline.Duration(Phase::Gather);
    if (span <= 0.0f) return 1.0f;

    // Spec 7.5: the cap radius and stem height grow over the phase, and convergence should take
    // most of it. A cloud that reached full size in the first second would have the fragments
    // chasing a target that stopped moving.
    const float u = core::Saturate((t - start) / span);
    return core::Saturate(0.12f + 0.88f * core::EaseOutCubic(u));
}

bool Detonation::MissileVisible(const Timeline& timeline, float t) const {
    return timeline.Active(Phase::Missile, t);
}

core::Vec3 Detonation::MissileAt(const Timeline& timeline, float t) const {
    // Progress is clamped at both ends, so a caller that asks outside the phase gets an endpoint
    // rather than a missile somewhere past the city.
    const float u = timeline.Progress(Phase::Missile, t);
    return core::Lerp(missileStart, center, MissileEase(u));
}

core::Vec3 Detonation::MissileWithin(float distance) const {
    const core::Vec3 back = missileStart - center;
    const float      len  = core::Length(back);
    if (len <= 1e-3f) return center;
    return center + back * (core::Saturate(distance / len));
}

core::Vec3 Detonation::MissileDirection() const {
    return core::Normalize(center - missileStart);
}

float Detonation::MissileEntryElevation(const core::Vec3& eye) const {
    const core::Vec3 d{missileStart.x - eye.x, 0.0f, missileStart.z - eye.z};
    return std::atan2(missileStart.y - eye.y, std::fmax(core::Length(d), 1.0f));
}

void Detonation::SetMissileRun(float run) {
    missileRun   = run;
    missileStart = center + core::Vec3{std::cos(missileBearing) * run,
                                       run * std::tan(missileDescent),
                                       std::sin(missileBearing) * run};
}

void Detonation::AimOverRidge(const core::Vec3& eye, float ridgeElevation, float margin,
                              float maxRun) {
    const float want = ridgeElevation + margin;

    // Walked rather than solved. The elevation is not monotonic in the run length in closed form —
    // the entry moves away from the camera as well as up, and by different amounts depending on
    // whether it is on the camera's side of the city or the far one — so this asks the real
    // question at a series of run lengths and stops at the first that answers yes. Same shape as
    // GenerateClosedRange in horizon.cpp, and for the same reason: the check is the requirement.
    const float start = missileRun;
    for (int step = 0; step <= 48; ++step) {
        const float run = core::Lerp(start, maxRun, static_cast<float>(step) / 48.0f);
        SetMissileRun(run);
        if (MissileEntryElevation(eye) >= want) return;
    }

    // Nothing inside the cap cleared it. Keep the longest run: it is the highest entry available,
    // which is the closest this cycle gets to what spec 7.1 asks for.
    SetMissileRun(maxRun);
}

float Detonation::FlashIntensity(const Timeline& timeline, float t) const {
    const float start = timeline.Start(Phase::Flash);
    const float end   = timeline.End(Phase::Flash);
    if (t < start || t >= end) return 0.0f;

    // Spec 7.2: full white over ~120 ms, then held for the rest of the phase. Held, not decayed:
    // the recovery is the exposure adaptation of spec 8.2 coming back up afterwards, and writing
    // a fade here as well would be the scripted version the spec explicitly rules out.
    const float ramp = core::Saturate((t - start) / 0.120f);

    // Spec 5.1 puts the peak at 8,000 to 15,000 linear.
    return 12000.0f * ramp * ramp;
}

namespace {

// The fireball exists from the flash to the end of the scatter, and its curves are measured
// against phase 6 rather than against that whole window.
//
// That distinction matters because spec 5.1 anchors the brightness at two points — "phase 6
// start" and "phase 6 end" — and phase 6 is four to six seconds while the window is a fixed six
// and a bit. Driving the decay from the window put the phase 6 end anywhere between 5 and 78
// linear depending on the seed, which is either side of the 20 to 60 the spec asks for. Driving it
// from phase 6 hits both anchors for every seed, and the tail simply runs on into phase 7.
float FireProgress(const Timeline& timeline, float t) {
    const float span = timeline.Duration(Phase::Blast);
    if (span <= 0.0f) return 1.0f;
    return (t - timeline.Start(Phase::Blast)) / span;
}

bool FireExists(const Timeline& timeline, float t) {
    return t >= timeline.Start(Phase::Flash) && t < timeline.End(Phase::Scatter);
}

}  // namespace

float Detonation::FireRadius(const Timeline& timeline, float t) const {
    if (!FireExists(timeline, t)) return 0.0f;

    const float v = FireProgress(timeline, t);

    // Out to full size almost at once — the flash and the fireball are the same event — then a
    // slow swell as it rises and cools, then a collapse over the last of phase 7 as the stem it
    // leaves behind is taken over by the fragments.
    const float grow  = 1.0f - std::exp(-16.0f * core::Saturate(v + 0.06f));
    const float swell = 1.0f + 0.22f * core::Saturate(v);

    // Measured against the window rather than against phase 6, because what it has to be gone by
    // is the end of the scatter.
    const float u   = core::Saturate((t - timeline.Start(Phase::Flash)) /
                                   (timeline.End(Phase::Scatter) - timeline.Start(Phase::Flash)));
    const float die = 1.0f - core::Saturate((u - 0.72f) / 0.28f);

    return fireRadius * grow * swell * die * die;
}

core::Vec3 Detonation::FireCenter(const Timeline& timeline, float t) const {
    const float u = core::Saturate((t - timeline.Start(Phase::Flash)) /
                                   (timeline.End(Phase::Scatter) - timeline.Start(Phase::Flash)));

    // It rises, but only a little: this is the fireball, not the cloud. The cloud is made of the
    // city and arrives in phase 8.
    return center + core::Vec3{0.0f, fireRise * core::EaseOutCubic(u), 0.0f};
}

core::Vec3 Detonation::FireColor(const Timeline& timeline, float t) const {
    const float v = core::Clamp(FireProgress(timeline, t), 0.0f, 2.0f);

    // Spec 7.2's cooling curve, as four stops. Chromaticity first, magnitude second, because the
    // two move at very different rates: the colour is most of the way to orange while the
    // intensity is still in the thousands, and multiplying a single ramp would have made it red
    // and dim together.
    core::Vec3 hue;
    if (v < 0.08f) {
        hue = core::Lerp(core::Vec3{1.00f, 0.98f, 0.94f}, core::Vec3{1.00f, 0.86f, 0.46f},
                         v / 0.08f);
    } else if (v < 0.35f) {
        hue = core::Lerp(core::Vec3{1.00f, 0.86f, 0.46f}, core::Vec3{1.00f, 0.44f, 0.12f},
                         (v - 0.08f) / 0.27f);
    } else {
        hue = core::Lerp(core::Vec3{1.00f, 0.44f, 0.12f}, core::Vec3{0.74f, 0.09f, 0.02f},
                         core::Saturate((v - 0.35f) / 0.85f));
    }

    // Spec 5.1: 2,000-4,000 at the start of phase 6 and 20-60 at its end. Those two anchors fix
    // the constant and the rate between them; nothing here is free.
    const float magnitude = 3000.0f * std::exp(-4.317f * v);
    return hue * magnitude;
}

}  // namespace world
