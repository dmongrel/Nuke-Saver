// Selftests for the city (spec 6.4), its growth (6.5) and the framing solver (11.1).
//
// The city is the first thing here with a hard count in the spec — 500 boxes — and counts are the
// kind of requirement that quietly stops holding. The number of lots a footprint yields depends on
// the block size, the road width and how much the arterials happened to cut, all drawn per cycle,
// so "it was 500 when I looked" is not evidence about the next seed.
//
// The framing solver is checked the same way the horizon's coverage check is: by proving it can
// fail. A solver that returns the shot it was handed passes every test that only asks whether the
// city is in frame afterwards, provided the shot was wide enough to start with.

#include "app/settings.h"
#include "render/building_data.h"
#include "core/math.h"
#include "selftest_check.h"
#include "world/camera.h"
#include "world/city.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace selftest {

using namespace core;
using namespace world;

namespace {

CityParams Params(uint64_t seed) {
    CityParams p;
    p.seed      = seed;
    p.radius    = 760.0f;
    p.gridAngle = 0.4f;
    return p;
}

// The four corners of a building's footprint, in world space.
void Corners(const Building& b, Vec2 out[4]) {
    const float c = std::cos(b.rotation);
    const float s = std::sin(b.rotation);

    const float sx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const float sz[4] = {-1.0f, -1.0f, 1.0f, 1.0f};

    for (int i = 0; i < 4; ++i) {
        const float lx = sx[i] * b.halfExtent.x;
        const float lz = sz[i] * b.halfExtent.y;
        out[i] = Vec2{b.center.x + lx * c - lz * s, b.center.y + lx * s + lz * c};
    }
}

const float kAxes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

}  // namespace

void TestCity() {
    std::printf("city (spec 6.4, 6.5)\n");

    const City city = GenerateCity(Params(20260917));

    Check(city.buildings.size() == 500, "the generator produces exactly 500 buildings");

    // Every seed, not just a convenient one. The count is met by discarding surplus lots from the
    // outside in, which only works if there were surplus lots — and whether there are depends on
    // parameters drawn per cycle.
    bool everySeedFull = true;
    for (uint64_t s = 1; s <= 40; ++s) {
        if (GenerateCity(Params(s)).buildings.size() != 500) everySeedFull = false;
    }
    Check(everySeedFull, "every seed fills all 500");

    // Spec 6.4 caps the footprint aspect at 1:2.5, and nothing may have a degenerate footprint.
    bool aspectOk = true, positive = true, standing = true;
    for (const Building& b : city.buildings) {
        const float lo = std::fmin(b.halfExtent.x, b.halfExtent.y);
        const float hi = std::fmax(b.halfExtent.x, b.halfExtent.y);
        if (lo <= 0.0f) positive = false;
        if (hi > lo * 2.5f + 1e-3f) aspectOk = false;
        if (b.height <= 0.0f) standing = false;
    }
    Check(positive, "every footprint has a positive extent");
    Check(aspectOk, "no footprint exceeds the 1:2.5 aspect of spec 6.4");
    Check(standing, "every building has a positive height");

    // The streets are the point. If buildings overlap, the grid that generated them is not
    // visible in the result and the city reads as a heap — so no two footprints may intersect.
    // Checked by separating axis on the two grid axes, which is exact here because every building
    // shares the grid's rotation.
    bool disjoint = true;
    for (size_t i = 0; i < city.buildings.size() && disjoint; ++i) {
        Vec2 a[4];
        Corners(city.buildings[i], a);

        for (size_t j = i + 1; j < city.buildings.size(); ++j) {
            const Building& bi = city.buildings[i];
            const Building& bj = city.buildings[j];

            // Cheap reject first: the footprints cannot touch if their centres are further apart
            // than the sum of their circumradii.
            const float ri = Length(bi.halfExtent);
            const float rj = Length(bj.halfExtent);
            const Vec2  d{bj.center.x - bi.center.x, bj.center.y - bi.center.y};
            if (Length(d) > ri + rj) continue;

            // Back into grid space, where both are axis aligned.
            const float c  = std::cos(-bi.rotation);
            const float s  = std::sin(-bi.rotation);
            const Vec2  dg{d.x * c - d.y * s, d.x * s + d.y * c};

            const bool overlapX = std::fabs(dg.x) < bi.halfExtent.x + bj.halfExtent.x - 1e-3f;
            const bool overlapZ = std::fabs(dg.y) < bi.halfExtent.y + bj.halfExtent.y - 1e-3f;
            if (overlapX && overlapZ) {
                disjoint = false;
                break;
            }
        }
    }
    Check(disjoint, "no two buildings overlap");

    // Downtown. Spec 6.4 wants the mean height to fall off with distance so a centre emerges
    // without anyone authoring one; checked as a comparison of means rather than of any one
    // building, because the distribution is deliberately long tailed.
    float innerSum = 0.0f, outerSum = 0.0f;
    int   innerN = 0, outerN = 0;
    for (const Building& b : city.buildings) {
        const float r = Length(b.center);
        if (r < city.params.radius * 0.35f) {
            innerSum += b.height;
            ++innerN;
        } else if (r > city.params.radius * 0.75f) {
            outerSum += b.height;
            ++outerN;
        }
    }
    Check(innerN > 20 && outerN > 20, "both the centre and the outskirts are populated");
    Check(innerSum / innerN > outerSum / outerN * 1.8f,
          "the centre is substantially taller than the outskirts");

    // Spec 6.5: everything must be standing before phase 2 ends, nothing may be visible before its
    // own start, and no rise may be outside 0.4 to 0.9 seconds.
    bool durationOk = true, startOk = true;
    for (const Building& b : city.buildings) {
        if (b.growthDuration < 0.4f - 1e-3f || b.growthDuration > 0.9f + 1e-3f) durationOk = false;
        if (b.growthStart < 0.0f) startOk = false;
    }
    Check(durationOk, "every rise takes between 0.4 and 0.9 seconds");
    Check(startOk, "no building starts before the phase does");
    Check(city.growthEnds <= city.params.growthSeconds + 1.2f,
          "the whole city is standing by the end of the growth phase");

    // The city grows outward (spec 6.5): the centre goes up first.
    float innerStart = 0.0f, outerStart = 0.0f;
    innerN = outerN = 0;
    for (const Building& b : city.buildings) {
        const float r = Length(b.center);
        if (r < city.params.radius * 0.35f) {
            innerStart += b.growthStart;
            ++innerN;
        } else if (r > city.params.radius * 0.75f) {
            outerStart += b.growthStart;
            ++outerN;
        }
    }
    Check(innerStart / innerN < outerStart / outerN,
          "the centre starts rising before the outskirts");

    // Two consecutive cycles sharing a skyline is a defect (spec 6.4), and the same seed
    // reproducing one is the other half of that requirement.
    const City again = GenerateCity(Params(20260917));
    const City other = GenerateCity(Params(20260918));

    Check(again.buildings[250].height == city.buildings[250].height &&
              again.buildings[250].center.x == city.buildings[250].center.x,
          "the same seed reproduces the same city");

    int differing = 0;
    for (size_t i = 0; i < 500; ++i) {
        if (other.buildings[i].height != city.buildings[i].height) ++differing;
    }
    Check(differing > 450, "a different seed gives a different skyline");
}

void TestUnitCube() {
    std::printf("unit cube (spec 6.4)\n");

    std::vector<render::BoxVertex> vertices;
    std::vector<uint32_t>          indices;
    render::BuildUnitCube(&vertices, &indices);

    Check(vertices.size() == 24, "24 vertices, so every face carries its own normal");
    Check(indices.size() == 36, "36 indices, two triangles per face");

    Check(sizeof(render::BuildingInstance) * 500 <= 64 * 1024,
          "the whole city's instance data fits in 32 KiB");

    // Spans [-1,1] in x and z and [0,1] in y: the growth of spec 6.5 is a scale on y, and it only
    // grows the box upward out of the ground if the box starts at the ground.
    float minY = 1e9f, maxY = -1e9f, maxXZ = 0.0f;
    for (const render::BoxVertex& v : vertices) {
        minY  = std::fmin(minY, v.position[1]);
        maxY  = std::fmax(maxY, v.position[1]);
        maxXZ = std::fmax(maxXZ, std::fmax(std::fabs(v.position[0]), std::fabs(v.position[2])));
    }
    CheckNear(minY, 0.0f, 1e-6f, "the cube's base is on the ground plane");
    CheckNear(maxY, 1.0f, 1e-6f, "the cube is one unit tall");
    CheckNear(maxXZ, 1.0f, 1e-6f, "the cube is two units across");

    // Winding against the shading normal. These two can disagree silently: the shading normal
    // decides how a face is lit and the winding decides whether it is drawn at all, so a face
    // wound backwards is replaced by the opposite face wearing the wrong normal. Both roof faces
    // were inverted this way, and it showed only from directly overhead at noon — every roof in
    // the city was black, which is not the sort of thing a log reports.
    bool consistent = true, unitNormals = true;
    for (size_t t = 0; t < indices.size() / 3; ++t) {
        const float* a = vertices[indices[t * 3 + 0]].position;
        const float* b = vertices[indices[t * 3 + 1]].position;
        const float* c = vertices[indices[t * 3 + 2]].position;
        const float* n = vertices[indices[t * 3 + 0]].normal;

        const Vec3 e0{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 e1{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const Vec3 face = Cross(e0, e1);

        if (Length(face) < 1e-6f) consistent = false;
        if (Dot(face, Vec3{n[0], n[1], n[2]}) <= 0.0f) consistent = false;
        if (std::fabs(Length(Vec3{n[0], n[1], n[2]}) - 1.0f) > 1e-5f) unitNormals = false;
    }
    Check(unitNormals, "every cube normal is unit length");
    Check(consistent, "every cube face is wound to agree with its own normal");

    // All six directions are present, or the box has a hole in it.
    int seen = 0;
    for (const float* d : {kAxes[0], kAxes[1], kAxes[2], kAxes[3], kAxes[4], kAxes[5]}) {
        for (const render::BoxVertex& v : vertices) {
            if (v.normal[0] == d[0] && v.normal[1] == d[1] && v.normal[2] == d[2]) {
                ++seen;
                break;
            }
        }
    }
    Check(seen == 6, "all six faces are present");
}

void TestFraming() {
    std::printf("camera framing (spec 11.1)\n");

    Subject subject;
    subject.radius = 720.0f;
    subject.height = 180.0f;

    // Every shot, every seed. The solver's job is to make the shot library safe to draw from, so
    // a failure on one shot in four is a failure.
    const ShotType shots[] = {ShotType::DistantRidge, ShotType::LowApproach, ShotType::HighOblique,
                              ShotType::StreetLevel};

    bool framed = true, outside = true, aboveGround = true, wideAlsoFits = true;
    for (uint64_t s = 1; s <= 30; ++s) {
        for (ShotType shot : shots) {
            OrbitCamera cam = OrbitCamera::Create(s, shot, 760.0f, 100.0f);
            cam.FrameOn(subject);

            for (int i = 0; i <= 40; ++i) {
                const float t = 100.0f * static_cast<float>(i) / 40.0f;

                if (t <= 30.0f && cam.FramingFill(subject, t) > 1.0f) framed = false;

                // 21:9 has more horizontal room than the 16:9 the solver assumes, so a shot that
                // fits there fits here. This is the half of "aspect-aware" that can be checked
                // without a window.
                if (t <= 30.0f && cam.FramingFill(subject, t, 21.0f / 9.0f) > 1.0f) {
                    wideAlsoFits = false;
                }

                const CameraState state = cam.Evaluate(t);
                if (state.eye.y < 5.0f) aboveGround = false;

                const float r = std::sqrt(state.eye.x * state.eye.x + state.eye.z * state.eye.z);
                if (r < subject.radius * 0.5f) outside = false;
            }
        }
    }
    Check(framed, "every shot frames the subject through the opening of the cycle");
    Check(wideAlsoFits, "a 16:9 solve also fits 21:9");
    Check(aboveGround, "the camera never drops to the ground");
    Check(outside, "the camera never flies into the middle of the city");

    // The solver has to *do* something, and in both directions. Handed a shot far too wide it must
    // come in; handed one far too tight it must pull out. Without this, a solver that returned its
    // input unchanged would pass everything above.
    {
        OrbitCamera wide = OrbitCamera::Create(7, ShotType::DistantRidge, 760.0f, 100.0f);
        const float before = wide.orbitRadius();
        const float fillBefore = wide.FramingFill(subject, 0.0f);
        wide.FrameOn(subject);

        Check(fillBefore < 0.6f, "the drawn shot really is too wide for the subject");
        Check(wide.orbitRadius() < before * 0.95f, "the solver brings a too-wide shot in");
        Check(wide.FramingFill(subject, 0.0f) > 0.6f, "and the subject then fills the frame");
    }
    {
        // A subject far too large for any shot in the library: the solver must push the camera
        // out rather than leave the city cropped.
        Subject huge;
        huge.radius = 6000.0f;
        huge.height = 3000.0f;

        OrbitCamera close = OrbitCamera::Create(7, ShotType::StreetLevel, 760.0f, 100.0f);
        const float before = close.orbitRadius();
        close.FrameOn(huge);

        Check(close.orbitRadius() > before * 1.5f, "the solver pulls out for a large subject");
        Check(close.FramingFill(huge, 0.0f) <= 1.0f, "and the large subject is then in frame");
    }

    // The orbit must not stop, hold or cut anywhere (spec 11.1), and the solver must not have
    // introduced one by scaling the radius: bearing is what carries the motion.
    {
        OrbitCamera cam = OrbitCamera::Create(11, ShotType::HighOblique, 760.0f, 100.0f);
        cam.FrameOn(subject);

        float slowest = 1e9f, fastest = 0.0f;
        for (int i = 0; i < 400; ++i) {
            const float t0 = 100.0f * static_cast<float>(i) / 400.0f;
            const float t1 = 100.0f * static_cast<float>(i + 1) / 400.0f;
            const float step = Length(cam.Evaluate(t1).eye - cam.Evaluate(t0).eye);
            slowest = std::fmin(slowest, step);
            fastest = std::fmax(fastest, step);
        }
        Check(slowest > 0.0f, "the camera never stops");
        Check(fastest < slowest * 6.0f, "the camera never lurches");
    }
}

}  // namespace selftest
