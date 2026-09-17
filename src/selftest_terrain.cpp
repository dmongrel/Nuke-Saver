// Selftests for the static geometry: the desert basin and the far-field range.
//
// Both are pure functions of a seed, so both are checkable exactly — which matters more here than
// anywhere else in the world, because these two carry a requirement that cannot be eyeballed.
// Spec 6.3 says the range MUST close the horizon from every point on the orbit at every shot
// height. A gap shows up on one bearing, at one height, in one cycle out of fifty; by the time a
// frame reveals it, fifty other frames have gone by that did not.
//
// The generator already checks its own work through ClosesHorizon. What these tests check is
// ClosesHorizon itself. An earlier version of it passed, with a 0.61 degree margin, on a range
// that visibly showed sky between every pair of peaks — it swept 180 bearings at 2 degrees while
// the peaks sat 3.75 degrees apart, so it stepped clean over the gaps it existed to find. A
// checker that cannot fail is worth nothing, so the case that matters below is the sabotaged one.

#include "app/settings.h"
#include "core/math.h"
#include "selftest_check.h"
#include "world/horizon.h"
#include "world/terrain.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>

namespace selftest {

using namespace core;
using namespace world;

namespace {

TerrainParams DefaultTerrain(uint64_t seed) {
    TerrainParams p;
    p.seed = seed;
    return p;
}

// Sized the way a real cycle sizes it. The bare defaults on HorizonParams are placeholders — the
// minimum height they carry is *below* a typical camera, so a range built from them straight off
// cannot close the horizon, and a test that used them would be testing a configuration that never
// ships.
HorizonParams DefaultHorizon(uint64_t seed) { return SizeHorizon(seed, 2600.0f, 3400.0f); }

// The triangle's normal, in the winding the pipeline treats as front-facing.
Vec3 FaceNormal(const Mesh& mesh, size_t tri) {
    const Vec3& a = mesh.vertices[mesh.indices[tri * 3 + 0]].position;
    const Vec3& b = mesh.vertices[mesh.indices[tri * 3 + 1]].position;
    const Vec3& c = mesh.vertices[mesh.indices[tri * 3 + 2]].position;
    return Cross(b - a, c - a);
}

}  // namespace

void TestTerrain() {
    std::printf("terrain (spec 6.2)\n");

    const TerrainParams p = DefaultTerrain(4242);

    // The city stands on a flat pad, or the buildings of M3b sink into dunes and the blast scorch
    // of M5 projects onto a surface it was never fitted to.
    bool padFlat = true;
    for (int i = 0; i < 64; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / 64.0f;
        for (float r : {0.0f, p.cityRadius * 0.4f, p.cityRadius * 0.85f}) {
            const float h = TerrainHeight(p, std::cos(a) * r, std::sin(a) * r);
            if (std::fabs(h) > 0.05f) padFlat = false;
        }
    }
    Check(padFlat, "the ground is flat under the city footprint");

    // ...and beyond the basin it is flat again, because that is where the horizon range stands.
    // A peak founded on a dune would float or sink depending on the seed.
    bool skirtFlat = true;
    for (int i = 0; i < 64; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / 64.0f;
        for (float r : {p.basinRadius * 1.4f, 14000.0f, 26000.0f}) {
            const float h = TerrainHeight(p, std::cos(a) * r, std::sin(a) * r);
            if (std::fabs(h) > 0.05f) skirtFlat = false;
        }
    }
    Check(skirtFlat, "the ground is flat where the horizon range stands");

    // Between the two there has to be something to look at.
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 512; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / 512.0f;
        const float r = p.cityRadius * 1.9f + (p.basinRadius * 0.75f - p.cityRadius * 1.9f) *
                                                  static_cast<float>(i % 17) / 16.0f;
        const float h = TerrainHeight(p, std::cos(a) * r, std::sin(a) * r);
        lo = std::fmin(lo, h);
        hi = std::fmax(hi, h);
    }
    Check(hi - lo > 40.0f, "the basin rim has relief between the pad and the skirt");

    // Same seed, same desert; different seed, different desert. The screen saver is restarted far
    // more often than it is watched, and a basin that did not vary would be the first thing seen.
    const TerrainParams other = DefaultTerrain(4243);
    Check(TerrainHeight(p, 1700.0f, -900.0f) == TerrainHeight(DefaultTerrain(4242), 1700.0f, -900.0f),
          "terrain height is deterministic for a seed");
    Check(TerrainHeight(p, 1700.0f, -900.0f) != TerrainHeight(other, 1700.0f, -900.0f),
          "a different seed gives a different basin");

    // Normals: unit length, and never pointing into the ground. A downward normal is invisible
    // until back-face culling is on, and then it is a hole.
    bool unit = true, upward = true;
    for (int i = 0; i < 256; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / 256.0f;
        const float r = 60.0f + 18.0f * static_cast<float>(i);
        const Vec3  n = TerrainNormal(p, std::cos(a) * r, std::sin(a) * r);
        if (std::fabs(Length(n) - 1.0f) > 1e-3f) unit = false;
        if (n.y <= 0.0f) upward = false;
    }
    Check(unit, "terrain normals are unit length");
    Check(upward, "terrain normals never point below the horizontal");

    const Mesh mesh = BuildTerrain(p);
    Check(!mesh.empty(), "the terrain mesh is not empty");
    Check(mesh.indices.size() % 3 == 0, "the terrain index count is a whole number of triangles");

    uint32_t maxIndex = 0;
    for (uint32_t i : mesh.indices) maxIndex = i > maxIndex ? i : maxIndex;
    Check(maxIndex < mesh.vertices.size(), "every terrain index is inside the vertex buffer");

    // Winding. Every terrain triangle is part of a height field, so its front face is the one
    // pointing up; getting this backwards makes the whole desert vanish under culling.
    bool wound = true, degenerate = false;
    for (size_t t = 0; t < mesh.indices.size() / 3; ++t) {
        const Vec3 n = FaceNormal(mesh, t);
        if (n.y <= 0.0f) wound = false;
        if (Length(n) < 1e-6f) degenerate = true;
    }
    Check(wound, "every terrain triangle is wound counter-clockwise seen from above");
    Check(!degenerate, "no terrain triangle is degenerate");

    // Albedo is resolved on the CPU and must already be linear and plausible: a value above 1
    // would mean the sRGB decode was skipped, and an exactly-equal pair of neighbours would mean
    // the colour field is not varying at all.
    bool inRange = true;
    for (const Vertex& v : mesh.vertices) {
        if (v.albedo.x < 0.0f || v.albedo.x > 1.0f) inRange = false;
        if (v.albedo.y < 0.0f || v.albedo.y > 1.0f) inRange = false;
        if (v.albedo.z < 0.0f || v.albedo.z > 1.0f) inRange = false;
        if (v.rockiness < 0.0f || v.rockiness > 1.0f) inRange = false;
    }
    Check(inRange, "terrain albedo is inside the unit range and rockiness is normalised");

    // The variation of spec 5.2 has to have a scale in metres, not one value per vertex. Sampled
    // along a line, neighbours a few metres apart must be close and points a few hundred metres
    // apart must not be — a per-vertex draw fails the first half, a constant fails the second.
    float nearDiff = 0.0f, farDiff = 0.0f;
    for (int i = 0; i < 200; ++i) {
        const float x  = 900.0f + static_cast<float>(i) * 7.0f;
        const Vec3  c0 = GroundAlbedo(p, x, 0.0f, 0.0f);
        const Vec3  c1 = GroundAlbedo(p, x + 4.0f, 0.0f, 0.0f);
        const Vec3  c2 = GroundAlbedo(p, x + 400.0f, 0.0f, 0.0f);
        nearDiff += Length(c1 - c0);
        farDiff += Length(c2 - c0);
    }
    Check(nearDiff * 4.0f < farDiff, "ground colour varies over metres, not per vertex");
    Check(farDiff > 0.0f, "ground colour is not constant");
}

void TestHorizon() {
    std::printf("horizon range (spec 6.3)\n");

    const HorizonParams       params = DefaultHorizon(20260917);
    const std::vector<Peak>   peaks  = GeneratePeaks(params);

    Check(peaks.size() == static_cast<size_t>(params.rows) * params.peaksPerRow,
          "the generator produces the requested number of peaks");

    bool tallEnough = true, positiveRadius = true, inRing = true;
    for (const Peak& peak : peaks) {
        if (peak.height < params.minHeight * 0.99f) tallEnough = false;
        if (peak.radius <= 0.0f) positiveRadius = false;
        const float r = std::sqrt(peak.center.x * peak.center.x + peak.center.y * peak.center.y);
        if (r < params.innerRadius * 0.9f || r > params.outerRadius * 1.1f) inRing = false;
    }
    Check(tallEnough, "no peak is shorter than the minimum height");
    Check(positiveRadius, "every peak has a positive base radius");
    Check(inRing, "every peak stands between the inner and outer radius");

    // A peak is taller than the highest camera, or it cannot rise above the horizon line at all,
    // however wide it is. This is the constraint that actually sizes the range.
    bool clearsCamera = true;
    for (const Peak& peak : peaks) {
        if (peak.height <= params.maxCameraHeight) clearsCamera = false;
    }
    Check(clearsCamera, "every peak is taller than the highest point of the camera orbit");

    // SilhouetteElevation against a single peak of known geometry, so the profile is pinned
    // rather than merely self-consistent.
    {
        std::vector<Peak> one(1);
        one[0].center = Vec2{10000.0f, 0.0f};
        one[0].height = 2000.0f;
        one[0].radius = 1000.0f;

        const Vec3 eye{0.0f, 0.0f, 0.0f};

        const float onAxis = SilhouetteElevation(one, eye, 0.0f);
        CheckNear(onAxis, std::atan2(2000.0f, 10000.0f), 1e-3f,
                  "on the axis the silhouette is the full height of the peak");

        // Well outside the base: nothing there, so the sky reaches the ground.
        const float clear = SilhouetteElevation(one, eye, 0.4f);
        Check(clear < 0.0f, "a bearing that misses the peak reports a gap");

        // Behind the viewer is not in front of them.
        const float behind = SilhouetteElevation(one, eye, kPi);
        Check(behind < 0.0f, "a peak behind the viewer does not close the horizon ahead");

        // Part way out the profile falls off, monotonically.
        float previous = onAxis;
        bool  monotonic = true;
        for (int i = 1; i <= 12; ++i) {
            const float bearing = 0.085f * static_cast<float>(i) / 12.0f;
            const float e       = SilhouetteElevation(one, eye, bearing);
            if (e > previous + 1e-4f) monotonic = false;
            previous = e;
        }
        Check(monotonic, "the silhouette falls away from the axis toward the base");
    }

    // The range the generator settles on closes the horizon. Note that this is asserted of
    // GenerateClosedRange's result, not of the first roll: the first roll is allowed to fail, and
    // the widening loop is the part that makes the guarantee.
    {
        HorizonParams settled = DefaultHorizon(20260917);
        float         margin  = 0.0f;
        const std::vector<Peak> closed = GenerateClosedRange(&settled, &margin);
        Check(ClosesHorizon(closed, settled), "the generated range closes the horizon");
        Check(margin > 0.0f, "the coverage margin is positive");
        Check(settled.widthFactor >= params.widthFactor,
              "the widening loop only ever widens");
    }

    // ...and the check can fail. Three separate ways of breaking a range, because an assertion
    // that only ever passes is indistinguishable from one that is wired to true.
    {
        HorizonParams narrow = params;
        narrow.widthFactor   = 0.55f;  // bases no longer overlap
        Check(!ClosesHorizon(GeneratePeaks(narrow), narrow),
              "a range whose bases do not overlap is reported as open");
    }
    {
        HorizonParams low = params;
        low.minHeight     = 200.0f;
        low.maxHeight     = 400.0f;  // shorter than the camera
        Check(!ClosesHorizon(GeneratePeaks(low), low),
              "a range shorter than the camera is reported as open");
    }
    {
        // One arc removed. This is the failure the sweep resolution exists for: the hole is a few
        // degrees wide, and the sweep that shipped before this test swept at two-degree steps and
        // walked straight past holes of exactly this size.
        //
        // The arc has to be wider than one peak. At widthFactor 2.7 each base spans nearly three
        // times the spacing to its neighbours, so removing a single peak leaves a dip that the
        // two either side still cover — which is the point of the overlap, and means a one-peak
        // sabotage would prove nothing about the checker.
        std::vector<Peak> gapped;
        for (const Peak& peak : peaks) {
            const float bearing = std::atan2(peak.center.y, peak.center.x);
            if (bearing > 0.45f && bearing < 0.95f) continue;
            gapped.push_back(peak);
        }
        Check(gapped.size() < peaks.size(), "the sabotage actually removed peaks");
        Check(!ClosesHorizon(gapped, params), "a missing arc is reported as open");
    }

    // The mesh, and the height field it is triangulated from. Its base must be on the ground: an
    // earlier version lifted every corner, which left the solid open underneath and showed as
    // horizontal shelves of sky beneath the front row from a camera standing on the plain.
    const Shell shell = BuildShell(peaks, params, params.seed);
    const Mesh  mesh  = BuildHorizon(shell, params.seed);
    Check(!mesh.empty(), "the horizon mesh is not empty");
    Check(mesh.indices.size() % 3 == 0, "the horizon index count is a whole number of triangles");

    uint32_t maxIndex = 0;
    for (uint32_t i : mesh.indices) maxIndex = i > maxIndex ? i : maxIndex;
    Check(maxIndex < mesh.vertices.size(), "every horizon index is inside the vertex buffer");

    float lowest = 1e9f;
    int   onGround = 0;
    for (const Vertex& v : mesh.vertices) {
        lowest = std::fmin(lowest, v.position.y);
        if (v.position.y == 0.0f) ++onGround;
    }
    Check(lowest >= 0.0f, "no horizon vertex is below the ground plane");
    Check(onGround > 0, "the peaks stand on the ground rather than floating above it");

    bool degenerate = false;
    for (size_t t = 0; t < mesh.indices.size() / 3; ++t) {
        if (Length(FaceNormal(mesh, t)) < 1e-3f) degenerate = true;
    }
    Check(!degenerate, "no horizon triangle is degenerate");

    // Upward-facing. The range is the one thing in the world that is back-face culled, and a
    // height field's whole claim is that it has no underside: every triangle in it is the top of
    // the surface, so every winding has to come out with the normal above the horizontal. One
    // face listed the other way round turns a hillside into a hole.
    {
        bool upward = true, everySeed = true;
        for (uint64_t s = 1; s <= 40; ++s) {
            std::vector<Peak> one(1);
            one[0].center = Vec2{6000.0f, -2500.0f};
            one[0].height = 2800.0f;
            one[0].radius = 900.0f;

            HorizonParams solo = params;
            solo.peaksPerRow   = 24;

            const Mesh mesh1 = BuildHorizon(BuildShell(one, solo, s), s);
            if (mesh1.indices.size() % 3 != 0 || mesh1.empty()) everySeed = false;

            for (size_t t = 0; t < mesh1.indices.size() / 3; ++t) {
                if (FaceNormal(mesh1, t).y <= 0.0f) upward = false;
            }
        }
        Check(everySeed, "a single peak meshes into whole triangles for every seed");
        Check(upward, "every horizon face points up, so the range has no underside to cull away");
    }

    // The surface that ships closes the horizon, not just the closed form the generator checked.
    //
    // These are two different questions and the file they live in has been wrong about it before:
    // the check is a closed form over analytic cones, and what gets drawn is a triangulated height
    // field built from them. The one holds for the other only because the field is never below the
    // profile the check assumed — the shell widens reach and raises lift, and never the reverse.
    // This is that claim, measured rather than asserted.
    {
        HorizonParams settled = SizeHorizon(41ull, 900.0f, 3400.0f);
        settled.seed          = 41ull;

        float                   margin = 0.0f;
        const std::vector<Peak> closed = GenerateClosedRange(&settled, &margin);
        const Shell             built  = BuildShell(closed, settled, settled.seed);

        float shellMargin = 0.0f;
        Check(ShellClosesHorizon(built, settled, &shellMargin),
              "the height field that ships closes the horizon, not only the cones behind it");

        // Sabotage, so the check above is known to be able to fail. Flattening an arc of the field
        // is the same failure a missing arc of peaks would produce, applied to the thing drawn.
        Shell gapped = built;
        for (int j = 0; j < gapped.rings; ++j) {
            for (int i = 0; i < gapped.bearings / 12; ++i) {
                gapped.height[static_cast<size_t>(j) * gapped.bearings + i] = 0.0f;
            }
        }
        Check(!ShellClosesHorizon(gapped, settled),
              "a flattened arc of the height field is reported as open");
    }

    // Finally, end to end: the world generator has to settle on a range that passes, for any
    // seed, without the retry loop running out of attempts.
    bool everySeedCloses = true;
    for (uint64_t s = 1; s <= 24; ++s) {
        const World w = Generate(app::Settings{}, s);
        if (!ClosesHorizon(GeneratePeaks(w.horizon), w.horizon)) everySeedCloses = false;
        if (w.horizonMesh.empty()) everySeedCloses = false;
    }
    Check(everySeedCloses, "the world generator closes the horizon for every seed it is given");
}

}  // namespace selftest
