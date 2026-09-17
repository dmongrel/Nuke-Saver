// The far-field mountain range (spec 6.3).
//
// The basin is 8 km across and the camera orbits it, so something has to close the horizon. These
// peaks are silhouette, not geography: a few faces each, no displacement, no LOD, never simulated
// and never fragmented by the blast.
//
// The requirement with teeth is coverage. Spec 6.3: the ring MUST completely close the horizon
// from every point on the camera orbit, at every shot height, with no gap where sky meets flat
// ground. That is not something to eyeball on a few frames — it fails on one bearing at one
// height, in one cycle out of fifty. So the silhouette is computable here, and the generator
// checks its own work before returning.
#ifndef NUKE_SAVER_WORLD_HORIZON_H
#define NUKE_SAVER_WORLD_HORIZON_H

#include "world/mesh.h"

#include <cstdint>
#include <vector>

namespace world {

struct Peak {
    core::Vec2 center{};        // on the ground plane
    float      height = 0.0f;   // apex above the ground
    float      radius = 0.0f;   // half-width of the base
};

struct HorizonParams {
    uint64_t seed = 0;

    // Rows at different distances, overlapping, so the range reads as having depth rather than as
    // a fence (spec 6.3).
    float innerRadius = 14000.0f;
    float outerRadius = 26000.0f;
    int   rows        = 3;

    // Tall enough to clear the highest camera in the shot library, which is what "close the
    // horizon" actually requires: a peak shorter than the viewer cannot rise above the horizon
    // line at all, however wide it is.
    float minHeight = 2200.0f;
    float maxHeight = 3600.0f;

    int peaksPerRow = 72;

    // Base half-width as a multiple of the spacing between neighbours in the same row. Above 1
    // the bases overlap, which is the whole mechanism: the ring closes because adjacent peaks
    // intersect and leave a saddle, not because any one peak is enormous. At 1.0 they merely
    // touch, and the silhouette dips to the ground between every pair.
    //
    // 2.7 because that is what it measures out at, not because it reads well: over 200 seeds it
    // closes the horizon on the first roll 197 times. At 1.9 it closed once in 200, so almost
    // every cycle went round the widening loop and came out 30% wider and 15% taller than the
    // numbers written here — the loop was doing the tuning, and these values described nothing.
    float widthFactor = 2.7f;

    // The highest and lowest the camera gets. Coverage is verified against these.
    float maxCameraHeight = 2600.0f;
    float orbitRadius     = 3400.0f;
};

// Sizes a range for a given orbit. The heights are derived from the highest point the camera
// reaches, because a peak shorter than the viewer cannot rise above the horizon line however wide
// it is — which is why this can only be answered once the camera for the cycle exists. The
// defaults on HorizonParams above are placeholders that no shot should be built from; this is the
// rule, and it lives here so the coverage check and the parameters it checks stay together.
HorizonParams SizeHorizon(uint64_t seed, float maxCameraHeight, float orbitRadius);

std::vector<Peak> GeneratePeaks(const HorizonParams& params);

// The elevation angle, in radians, of the highest thing the range puts at this bearing as seen
// from `eye`. Negative means the sky reaches down to the ground plane at that bearing, which is
// exactly the gap spec 6.3 forbids.
float SilhouetteElevation(const std::vector<Peak>& peaks, const core::Vec3& eye, float bearing);

// True when no bearing shows a gap, from any point on the orbit at any height up to
// maxCameraHeight. This is spec 6.3's coverage requirement, stated as a function.
bool ClosesHorizon(const std::vector<Peak>& peaks, const HorizonParams& params,
                   float* worstElevation = nullptr);

// Generates a range that satisfies ClosesHorizon, widening and raising `params` in place until it
// does. This is spec 6.3's obligation discharged in one place: everything that builds a world goes
// through here, so no caller can forget to check, and the test that proves the check can fail is
// testing the same path the screen saver runs.
//
// `worstElevation`, if given, receives the final coverage margin in radians.
std::vector<Peak> GenerateClosedRange(HorizonParams* params, float* worstElevation = nullptr);

Mesh BuildHorizon(const std::vector<Peak>& peaks, uint64_t seed);

}  // namespace world

#endif
