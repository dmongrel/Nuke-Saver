#include "world/horizon.h"

#include "app/log.h"
#include "core/color.h"
#include "core/rng.h"

namespace world {
namespace {

using core::Vec2;
using core::Vec3;

// The peak's shape, in normalised terms, shared by the mesh and the coverage check.
//
// These have to be one definition. The check exists to catch gaps in the range that is actually
// built, so a check that models a different solid is a check of nothing — and the previous pair
// disagreed in the dangerous direction: the mesh lifted its corners off the ground while the
// check assumed a plain cone standing on it, which sits *above* the real silhouette everywhere
// below the shoulders. It reported coverage the geometry did not have.
//
// A peak is a base ring on the ground, a shoulder ring part way up, and an apex. The shoulders
// are what make it read as rock rather than as a cone: the break in slope gives it ridgelines
// running down from the summit, and the low corners become the spurs between them.
constexpr int   kSides        = 7;      // odd, so opposite faces do not mirror each other
constexpr float kShoulderFrac = 0.58f;  // shoulder ring radius, as a fraction of the base
constexpr float kLiftMin      = 0.10f;  // shoulder height, as a fraction of the peak height
constexpr float kLiftMax      = 0.30f;
constexpr float kReachMin     = 0.85f;  // base corner radius, as a fraction of peak.radius
constexpr float kReachMax     = 1.15f;

// Height as a fraction of peak.height at a lateral offset `u` measured as a fraction of
// peak.radius. This is the *lower bound* over the per-corner jitter: it uses the smallest lift,
// so the peak that gets built is never shorter at a given offset than the check assumed.
float PeakProfile(float u) {
    if (u >= 1.0f) return 0.0f;
    if (u <= 0.0f) return 1.0f;
    if (u >= kShoulderFrac) {
        return core::Lerp(0.0f, kLiftMin, (1.0f - u) / (1.0f - kShoulderFrac));
    }
    return core::Lerp(kLiftMin, 1.0f, (kShoulderFrac - u) / kShoulderFrac);
}

}  // namespace

HorizonParams SizeHorizon(uint64_t seed, float maxCameraHeight, float orbitRadius) {
    HorizonParams params;
    params.seed            = seed;
    params.maxCameraHeight = maxCameraHeight;
    params.orbitRadius     = orbitRadius;

    // Clear the camera with room to spare, then span most of a kilometre above that so the range
    // has summits rather than a uniform wall.
    params.minHeight = maxCameraHeight * 1.35f + 900.0f;
    params.maxHeight = params.minHeight * 1.7f;

    return params;
}

std::vector<Peak> GenerateClosedRange(HorizonParams* params, float* worstElevation) {
    std::vector<Peak> peaks = GeneratePeaks(*params);

    float worst = 0.0f;
    for (int attempt = 0; attempt < 6 && !ClosesHorizon(peaks, *params, &worst); ++attempt) {
        // Widen and raise rather than re-rolling: a re-roll is a lottery, whereas the reason a
        // ring fails is always the same two things - the peaks are too short to clear this
        // camera, or too narrow to overlap their neighbours.
        //
        // Note what is *not* adjusted here. Adding peaks per row does nothing, because the base
        // width is derived from the spacing: more peaks means proportionally narrower ones and
        // the overlap ratio is unchanged. An earlier version turned that knob and could not
        // converge no matter how many attempts it was given.
        params->widthFactor *= 1.30f;
        params->minHeight *= 1.15f;
        params->maxHeight *= 1.15f;
        app::Log("horizon: gap at %.3f deg, raising peaks (attempt %d)", worst / core::kDegToRad,
                 attempt + 1);
        peaks = GeneratePeaks(*params);
    }

    if (worstElevation) ClosesHorizon(peaks, *params, worstElevation);
    return peaks;
}

std::vector<Peak> GeneratePeaks(const HorizonParams& params) {
    core::Rng rng = core::Rng(params.seed).Fork(0x40120Full);

    std::vector<Peak> peaks;
    peaks.reserve(static_cast<size_t>(params.rows) * params.peaksPerRow);

    for (int row = 0; row < params.rows; ++row) {
        // Rows are spread between the inner and outer radius. The nearest row does most of the
        // covering; the ones behind it give the range depth.
        const float rowT = params.rows > 1 ? static_cast<float>(row) / (params.rows - 1) : 0.0f;
        const float rowRadius = core::Lerp(params.innerRadius, params.outerRadius, rowT);

        // Each row is offset so peaks do not line up radially between rows, which would read as
        // one row with strange shading rather than as three.
        const float rowPhase = rng.Range(0.0f, core::kTwoPi);

        for (int i = 0; i < params.peaksPerRow; ++i) {
            const float base =
                core::kTwoPi * static_cast<float>(i) / static_cast<float>(params.peaksPerRow);

            // Jitter is kept below half the spacing. Beyond that peaks swap places, which tears
            // holes in the ring - the exact failure the coverage check exists to catch, and one
            // worth not causing on purpose.
            const float spacing = core::kTwoPi / static_cast<float>(params.peaksPerRow);
            const float angle   = base + rowPhase + rng.Range(-0.35f, 0.35f) * spacing;

            const float radius = rowRadius * rng.Range(0.94f, 1.06f);

            Peak peak;
            peak.center = Vec2{std::cos(angle) * radius, std::sin(angle) * radius};

            // Squared, so most peaks sit near the bottom of the range and a few stand well above
            // it. A uniform draw gives a row of near-equals, which reads as a fence however
            // irregular each one is; a real range is mostly shoulders with occasional summits.
            const float t = rng.Unit();
            peak.height   = core::Lerp(params.minHeight, params.maxHeight, t * t);
            if (rng.Chance(0.08f)) peak.height *= rng.Range(1.25f, 1.55f);  // the dominant few

            // Wide enough that neighbours in the same row overlap. `spacing * rowRadius` is the
            // arc between neighbours, so widthFactor is literally how many gaps wide each base
            // is; anything at or below 1 leaves the silhouette touching the ground between peaks.
            peak.radius = rowRadius * spacing * params.widthFactor * rng.Range(0.85f, 1.15f);

            peaks.push_back(peak);
        }
    }

    return peaks;
}

float SilhouetteElevation(const std::vector<Peak>& peaks, const Vec3& eye, float bearing) {
    const Vec2 dir{std::cos(bearing), std::sin(bearing)};

    float best = -core::kPi;

    for (const Peak& peak : peaks) {
        const Vec2 toPeak{peak.center.x - eye.x, peak.center.y - eye.z};

        // How far along the view bearing the peak sits, and how far off it.
        const float along = core::Dot(toPeak, dir);
        if (along <= 0.0f) continue;  // behind the viewer

        // The narrowest the base can be after the per-corner reach jitter. Using the nominal
        // radius here would credit the peak with width some of its corners do not have.
        const float radius = peak.radius * kReachMin;

        const float lateral = std::fabs(toPeak.x * dir.y - toPeak.y * dir.x);
        if (lateral >= radius) continue;  // this bearing misses the peak entirely

        const float apex = peak.height * PeakProfile(lateral / radius);

        const float elevation = std::atan2(apex - eye.y, along);
        if (elevation > best) best = elevation;
    }

    return best;
}

bool ClosesHorizon(const std::vector<Peak>& peaks, const HorizonParams& params,
                   float* worstElevation) {
    float worst = core::kPi;

    // Bearing resolution has to be finer than the narrowest thing being looked for, and what is
    // being looked for is a gap between two peaks. At 72 peaks per row those sit 5 degrees apart,
    // so a sweep at 2-degree steps can step clean over a gap and report the range closed - which
    // is exactly what an earlier version of this did, passing with a 0.61 degree margin on a
    // frame that visibly showed sky between every pair of peaks.
    //
    // 1440 bearings is a quarter of a degree. The orbit and height sampling is cut back to pay
    // for it, which is the right trade: a gap exists at essentially every orbit position or none,
    // whereas it exists at one bearing and not its neighbour.
    const int kBearings = 1440;

    // Both ends of the radius range, not just the middle: the orbit creeps in or out over the
    // cycle, and the near end is where the gaps between the rows open up.
    const float radii[]   = {params.orbitRadius * 0.7f, params.orbitRadius * 1.3f};
    const float heights[] = {2.0f, params.maxCameraHeight};

    for (int o = 0; o < 16; ++o) {
        const float orbitAngle = core::kTwoPi * static_cast<float>(o) / 16.0f;
        for (float radius : radii) {
            for (float height : heights) {
                const Vec3 eye{std::cos(orbitAngle) * radius, height,
                               std::sin(orbitAngle) * radius};

                for (int b = 0; b < kBearings; ++b) {
                    const float bearing =
                        core::kTwoPi * static_cast<float>(b) / static_cast<float>(kBearings);
                    const float e = SilhouetteElevation(peaks, eye, bearing);
                    if (e < worst) worst = e;
                }
            }
        }
    }

    if (worstElevation) *worstElevation = worst;
    return worst > 0.0f;
}

Mesh BuildHorizon(const std::vector<Peak>& peaks, uint64_t seed) {
    // Three triangles per side: two for the base-to-shoulder band, one for the shoulder-to-apex
    // cap. The base ring stays on the ground. An earlier version lifted the corners directly and
    // left the solid open underneath, which from a camera two metres above a plain 14 km away
    // showed as horizontal shelves of sky under every peak in the front row.
    constexpr int kTrisPerPeak = kSides * 3;

    Mesh mesh;
    mesh.vertices.reserve(peaks.size() * kTrisPerPeak * 3);
    mesh.indices.reserve(peaks.size() * kTrisPerPeak * 3);

    uint64_t id = 0;
    for (const Peak& peak : peaks) {
        const float      yaw    = core::HashFloat(seed, id, 0) * core::kTwoPi;
        const core::Vec3 albedo = core::Albedo(core::palette::kRock, seed, id, 1);

        Vec3 base[kSides];
        Vec3 shoulder[kSides];
        for (int c = 0; c < kSides; ++c) {
            const float angle =
                yaw + core::kTwoPi * static_cast<float>(c) / static_cast<float>(kSides);
            const float reach = peak.radius * core::HashRange(seed, id, 2 + static_cast<uint64_t>(c),
                                                              kReachMin, kReachMax);
            const float lift  = peak.height * core::HashRange(seed, id, 20 + static_cast<uint64_t>(c),
                                                             kLiftMin, kLiftMax);

            const float cx = std::cos(angle);
            const float cz = std::sin(angle);

            base[c]     = Vec3{peak.center.x + cx * reach, 0.0f, peak.center.y + cz * reach};
            shoulder[c] = Vec3{peak.center.x + cx * reach * kShoulderFrac, lift,
                               peak.center.y + cz * reach * kShoulderFrac};
        }

        // The apex leans off centre, which is most of what stops a row of peaks looking like a
        // row of peaks. Kept inside the shoulder ring so no face folds back on itself.
        const Vec3 apex{peak.center.x + core::HashRange(seed, id, 40, -0.20f, 0.20f) * peak.radius,
                        peak.height,
                        peak.center.y + core::HashRange(seed, id, 41, -0.20f, 0.20f) * peak.radius};

        // Flat shaded: one normal per face, so the facets read as rock rather than as a smoothly
        // inflated blob. That means no vertex can be shared between faces.
        //
        // Winding: the corners run clockwise seen from above in this coordinate system (x = cos,
        // z = sin, y up), so each triangle is listed so that Cross(v1 - v0, v2 - v0) points
        // outward and the face is counter-clockwise from outside — which is what the pipeline
        // calls front. This mesh is the one thing here that is back-face culled, so getting it
        // backwards turns the whole range inside out.
        const auto emit = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
            const Vec3 normal = core::Normalize(core::Cross(b - a, c - a));
            const Vec3 tri[3] = {a, b, c};
            for (int k = 0; k < 3; ++k) {
                Vertex v;
                v.position  = tri[k];
                v.normal    = normal;
                v.albedo    = albedo;
                v.rockiness = 1.0f;  // always rock, never sand
                mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
                mesh.vertices.push_back(v);
            }
        };

        for (int c = 0; c < kSides; ++c) {
            const int n = (c + 1) % kSides;

            emit(base[c], shoulder[c], base[n]);
            emit(base[n], shoulder[c], shoulder[n]);
            emit(shoulder[c], apex, shoulder[n]);
        }

        ++id;
    }

    return mesh;
}

}  // namespace world
