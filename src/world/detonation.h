// The shape of the detonation, as numbers rather than as pixels (spec 7.2, 7.5).
//
// The blast shell's radius and the mushroom's dimensions are needed in three places — the compute
// simulation, the building and board vertex shaders that must stop drawing what the shell has
// reached, and eventually the shell and fireball of M5 — so they live here rather than being
// worked out again in each of them. Everything is a function of the clock and the city's size, so
// a cycle's detonation is reproducible from its seed exactly as spec 4.2 requires.
#ifndef NUKE_SAVER_WORLD_DETONATION_H
#define NUKE_SAVER_WORLD_DETONATION_H

#include "core/math.h"
#include "world/phase.h"

#include <cstdint>

namespace world {

struct Detonation {
    core::Vec3 center{};  // the impact point; the city is generated around the origin

    // Spec 7.2: the shell expands fast and decelerates, and it MUST cross the whole city inside
    // phase 6. `reach` is where it ends up, past the far edge of the city and the board.
    float reach = 0.0f;

    // The mushroom of spec 7.5, in metres, at full growth.
    float stemHeight = 0.0f;
    float capHeight  = 0.0f;
    float capRadius  = 0.0f;
    float capTube    = 0.0f;

    // Spec 7.5: the cloud drifts slowly in one constant direction, drawn per cycle.
    core::Vec3 wind{};

    // Spec 7.1: the missile. One bearing and one entry point, drawn per cycle, so the approach is
    // reproducible from the seed. The run is a straight line from `missileStart` to `center`,
    // arriving exactly at the end of phase 4.
    //
    // The bearing and the run length are drawn in Create. The descent angle is not: spec 7.1
    // requires the missile to enter *just above the mountains and inside the frame*, and where
    // that is depends on the camera, on how tall the range behind it turned out, and on how wide
    // the shot is — none of which exist yet when the detonation is made. So Create leaves the
    // angle at zero and AimApproach solves it once the shot and the range are built.
    core::Vec3 missileStart{};
    float      missileBearing = 0.0f;  // radians, the compass direction the entry lies in
    float      missileDescent = 0.0f;  // radians below horizontal
    float      missileRun     = 0.0f;  // metres of ground covered, entry to impact
    float      missileLength  = 0.0f;  // metres, nose to tail
    float      missileRadius  = 0.0f;  // body radius

    // Spec 7.2: the fireball. It sits at the impact point, grows fast, rises a little, and cools
    // white -> yellow -> orange -> deep red across phases 6 and 7.
    float fireRadius = 0.0f;  // metres, at full size
    float fireRise   = 0.0f;  // metres it climbs before it dies

    // Not 9.81. The brief is explicit that this is not real physics, and at true gravity a
    // fragment thrown hard enough to clear the city is still in the air when the five seconds of
    // spec 7.4 are up. Heavier gravity buys the arc back without slowing the throw.
    float gravity = 26.0f;

    // Drawn from the seed and the city's extent.
    static Detonation Create(uint64_t seed, float cityRadius, float tallestBuilding);

    // Metres from the impact point at cycle time `t`. Zero before the flash, so "has the shell
    // reached this box" is one comparison everywhere and needs no phase test of its own.
    float ShellRadius(const Timeline& timeline, float t) const;

    // Spec 7.1. `MissileAt` is the nose position at `t`; the body runs back from it along
    // `MissileDirection`. Outside phase 4 there is no missile and `MissileVisible` says so — spec
    // 7.1 requires it to be gone from phase 5 on, and an emissive cylinder surviving the flash is
    // exactly the kind of thing nobody notices until it is on screen for eighty seconds.
    bool       MissileVisible(const Timeline& timeline, float t) const;
    core::Vec3 MissileAt(const Timeline& timeline, float t) const;
    core::Vec3 MissileDirection() const;

    // How far along the path the missile is at phase progress `u`, 0 to 1. Not the identity: a
    // re-entering warhead sheds a great deal of speed in the last few thousand metres of air, and
    // once AimOverRidge has stretched the run out over the mountains a constant pace would cover
    // the final approach — the part the whole phase exists to show — in under a second. This eases
    // the last stretch to a little under half the average pace without ever stopping it.
    //
    // Mirrored by MissileNoseAt in shaders/particle_common.glsl, which places the contrail where
    // the missile actually was rather than where a straight lerp would have put it.
    static float MissileEase(float u) {
        const float slowed = 1.0f - (1.0f - u) * (1.0f - u);
        return core::Lerp(u, slowed, 0.55f);
    }

    // Moves the entry point to `run` metres of ground from the impact point, along the bearing
    // and descent angle currently set. The one place missileStart is written, so the numbers that
    // describe the approach cannot drift apart from the point they describe.
    void SetMissileRun(float run);

    // Swings the whole approach round to a new compass bearing, keeping its length and angle.
    void SetMissileBearing(float bearing);

    // The horizontal angle between the camera's forward direction and the entry point, in radians.
    // Zero means the missile enters dead ahead; anything past the camera's half field of view is a
    // missile entering off the side of the screen, or behind the viewer entirely.
    float MissileEntryOffAxis(const core::Vec3& eye, const core::Vec3& forward) const;

    // Where the missile is when it still has `distance` metres of its path left to fly. The
    // framing solver of spec 11.1 sizes its missile subject from this rather than from a clock
    // time, so that the framing does not move when AimOverRidge changes the run length: what has
    // to be in frame is the arrival, and the arrival is a distance from the impact point.
    core::Vec3 MissileWithin(float distance) const;

    // The elevation, in radians, of the entry point as seen from `eye`. Negative means the
    // missile appears below the viewer, which is the version of phase 4 where it slides in across
    // the rooftops instead of coming down out of the sky.
    float MissileEntryElevation(const core::Vec3& eye) const;

    // Spec 7.1: sets the descent angle so the entry point sits `margin` radians above
    // `ridgeElevation` as seen from `eye` — over the mountains rather than in front of them — but
    // never above `ceiling`, the highest elevation still comfortably inside the frame. Both bounds
    // are the requirement: an entry below the ridge is a missile that slid in off the peaks, and
    // one above the frame is a missile the viewer never sees arrive.
    //
    // The two cannot always both be met. The steep shots look down into the basin and carry no sky
    // at all — their ceiling is below the horizon, let alone below the range — and there the
    // ceiling wins: a missile dropping into frame from above is still a missile arriving, and one
    // aimed over mountains the shot does not contain is not.
    //
    // The bearing and the run are left alone. Because they are, the horizontal distance from the
    // eye to the entry point does not depend on the angle being solved for, which makes this one
    // line of trigonometry rather than a search.
    //
    // Returns the elevation it actually achieved, which differs from the target only when the
    // angle had to be clamped to the band a missile can plausibly descend at.
    float AimApproach(const core::Vec3& eye, float ridgeElevation, float margin, float ceiling);

    // The band the solved descent angle is held to. Below the floor the approach is the shallow
    // slide across the rooftops this replaced; above the ceiling it is a drop, and the contrail
    // behind it is a vertical stroke that says nothing about where the missile came from.
    static constexpr float kDescentMin = 0.1745f;  // 10 degrees
    static constexpr float kDescentMax = 0.7854f;  // 45 degrees

    // Spec 5.1: the flash peaks at 8,000-15,000 linear. Zero outside phase 5, ramping over the
    // first ~120 ms and holding for the rest of it (spec 7.2).
    float FlashIntensity(const Timeline& timeline, float t) const;

    // Spec 7.2 and 5.1: the fireball's radius in metres, its centre (it rises), and its emissive
    // colour, which carries the 2,000-to-20 cooling in its magnitude. Radius is zero outside
    // phases 5 to 7, which is how every caller knows whether to draw it.
    float      FireRadius(const Timeline& timeline, float t) const;
    core::Vec3 FireCenter(const Timeline& timeline, float t) const;
    core::Vec3 FireColor(const Timeline& timeline, float t) const;

    // How far the cloud has grown, 0 to 1, across the gather phase (spec 7.5). Zero before it, so
    // the simulation can use it as the "is there a cloud to be drawn toward" flag as well.
    float CloudGrow(const Timeline& timeline, float t) const;
};

}  // namespace world

#endif
