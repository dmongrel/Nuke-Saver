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
    // reproducible from the seed. The run is a straight line at constant speed from `missileStart`
    // to `center`, arriving exactly at the end of phase 4.
    core::Vec3 missileStart{};
    float      missileLength = 0.0f;  // metres, nose to tail
    float      missileRadius = 0.0f;  // body radius

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
