#include "world/horizon.h"

#include "app/log.h"
#include "core/color.h"
#include "core/noise.h"
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
// A peak is a base ring on the ground, a shoulder part way up, and an apex. The shoulder is what
// makes it read as rock rather than as a cone: the break in slope gives it ridgelines running
// down from the summit, and the saddles it makes with its neighbours become the spurs between
// them.
constexpr float kShoulderFrac = 0.58f;  // shoulder radius, as a fraction of the base
constexpr float kLiftMin      = 0.10f;  // shoulder height, as a fraction of the peak height
constexpr float kLiftMax      = 0.30f;
constexpr float kReachMin     = 0.85f;  // the narrowest the check credits a peak with being
constexpr float kReachMax     = 1.15f;  // the widest the shell builds one

// Height as a fraction of peak.height at a lateral offset `u` measured as a fraction of the
// peak's reach, for a given shoulder lift. Base at u = 1, shoulder at u = kShoulderFrac, apex at
// u = 0, linear between.
float ShellProfile(float u, float lift) {
    if (u >= 1.0f) return 0.0f;
    if (u <= 0.0f) return 1.0f;
    if (u >= kShoulderFrac) {
        return core::Lerp(0.0f, lift, (1.0f - u) / (1.0f - kShoulderFrac));
    }
    return core::Lerp(lift, 1.0f, (kShoulderFrac - u) / kShoulderFrac);
}

// The same shape at its *lower bound* over the jitter the shell applies: smallest lift, and `u`
// measured against the narrowest reach. The peak that gets built is never shorter at a given
// offset than the check assumed, which is what makes the check conservative rather than hopeful.
float PeakProfile(float u) { return ShellProfile(u, kLiftMin); }

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

namespace {

// --- the shell --------------------------------------------------------------------------------

// Angular samples per peak spacing. Four puts about twenty samples across the widest part of a
// peak, which is enough for the facets to read as rock once the roughness is on, and few enough
// that the range does not cost more than the city standing in front of it.
constexpr int kShellPerSpacing = 4;

// Metres between rings. Chosen so the quads come out roughly square at the middle row rather than
// as radial stripes, which is what an aspect mismatch on a polar grid reads as.
constexpr float kShellRadialStep = 520.0f;

// Roughness, as a fraction of the local height. It multiplies, and only ever upward: the coverage
// check assumed the lower-bound profile, so a surface never shorter than that profile is a surface
// the check still describes.
constexpr float kShellRough = 0.20f;

core::FbmParams ShellRoughParams() {
    core::FbmParams p;
    p.octaves   = 4;
    p.frequency = 1.0f / 900.0f;  // spurs on the scale of a shoulder, not of a whole peak
    return p;
}

// The field the rock colour is drawn from. Long wavelength: the range is twenty kilometres away
// and hazed, so anything finer than a whole shoulder averages out to one flat grey before it
// reaches the eye.
core::FbmParams ShellTintParams() {
    core::FbmParams p;
    p.octaves    = 3;
    p.frequency  = 1.0f / 1700.0f;
    p.lacunarity = 2.1f;
    return p;
}

// A peak carrying the two jitters the old per-corner construction had, drawn once per peak rather
// than once per corner. Both are one-sided on purpose: reach is never below peak.radius and lift
// is never below kLiftMin, which are exactly the values SilhouetteElevation assumes, so the built
// surface is nowhere below the surface the coverage check was run against.
struct ShellPeak {
    Vec2  center{};
    float height = 0.0f;
    float reach  = 0.0f;
    float lift   = 0.0f;
};

}  // namespace

float Shell::HeightAt(float x, float z) const {
    if (rings < 2 || bearings < 2) return 0.0f;

    const float r = std::sqrt(x * x + z * z);
    if (r < innerRadius || r >= outerRadius()) return 0.0f;

    const float fr = (r - innerRadius) / radialStep;
    const int   j  = static_cast<int>(fr);
    const float tj = fr - static_cast<float>(j);

    float bearing = std::atan2(z, x);
    if (bearing < 0.0f) bearing += core::kTwoPi;
    const float fb = bearing / core::kTwoPi * static_cast<float>(bearings);
    const int   i  = static_cast<int>(fb) % bearings;
    const float ti = fb - std::floor(fb);
    const int   i1 = (i + 1) % bearings;

    const float h00 = height[static_cast<size_t>(j) * bearings + i];
    const float h10 = height[static_cast<size_t>(j) * bearings + i1];
    const float h01 = height[static_cast<size_t>(j + 1) * bearings + i];
    const float h11 = height[static_cast<size_t>(j + 1) * bearings + i1];

    return core::Lerp(core::Lerp(h00, h10, ti), core::Lerp(h01, h11, ti), tj);
}

Shell BuildShell(const std::vector<Peak>& peaks, const HorizonParams& params, uint64_t seed) {
    Shell shell;
    if (peaks.empty()) return shell;

    std::vector<ShellPeak> sp;
    sp.reserve(peaks.size());

    float inner = 0.0f;
    float outer = 0.0f;
    bool  first = true;

    uint64_t id = 0;
    for (const Peak& peak : peaks) {
        ShellPeak s;
        s.center = peak.center;
        s.height = peak.height;
        s.reach  = peak.radius * core::HashRange(seed, id, 2, 1.0f, kReachMax);
        s.lift   = core::HashRange(seed, id, 20, kLiftMin, kLiftMax);
        sp.push_back(s);

        const float centerRadius =
            std::sqrt(peak.center.x * peak.center.x + peak.center.y * peak.center.y);
        inner = first ? centerRadius - s.reach : std::fmin(inner, centerRadius - s.reach);
        outer = std::fmax(outer, centerRadius + s.reach);
        first = false;
        ++id;
    }

    shell.bearings    = params.peaksPerRow * kShellPerSpacing;
    shell.radialStep  = kShellRadialStep;
    shell.innerRadius = std::fmax(inner, 0.0f);
    shell.rings = static_cast<int>((outer - shell.innerRadius) / kShellRadialStep) + 2;
    shell.height.assign(static_cast<size_t>(shell.rings) * shell.bearings, 0.0f);

    // Peaks bucketed by bearing, so filling one grid point tests the dozen or so peaks that could
    // possibly reach it rather than all 216. Without this the fill is quadratic enough to show up
    // as a stall at the cycle reset.
    const float bucketAngle = core::kTwoPi / static_cast<float>(shell.bearings);

    std::vector<std::vector<int>> bucket(static_cast<size_t>(shell.bearings));
    int halfSpan = 1;
    for (size_t k = 0; k < sp.size(); ++k) {
        float bearing = std::atan2(sp[k].center.y, sp[k].center.x);
        if (bearing < 0.0f) bearing += core::kTwoPi;
        const int b = static_cast<int>(bearing / bucketAngle) % shell.bearings;
        bucket[static_cast<size_t>(b)].push_back(static_cast<int>(k));

        const float centerRadius =
            std::sqrt(sp[k].center.x * sp[k].center.x + sp[k].center.y * sp[k].center.y);
        const float halfAngle =
            std::asin(core::Clamp(centerRadius > 0.0f ? sp[k].reach / centerRadius : 1.0f, 0.0f,
                                  1.0f));
        const int span = static_cast<int>(halfAngle / bucketAngle) + 2;
        if (span > halfSpan) halfSpan = span;
    }
    if (halfSpan > shell.bearings / 2) halfSpan = shell.bearings / 2;

    const core::FbmParams rough = ShellRoughParams();

    std::vector<int> candidates;
    for (int i = 0; i < shell.bearings; ++i) {
        const float angle = bucketAngle * static_cast<float>(i);
        const float ca    = std::cos(angle);
        const float sa    = std::sin(angle);

        // Gathered once and reused down the whole radial line: which peaks can reach a bearing
        // does not depend on how far out along it the sample sits.
        candidates.clear();
        for (int d = -halfSpan; d <= halfSpan; ++d) {
            const int b = ((i + d) % shell.bearings + shell.bearings) % shell.bearings;
            for (int k : bucket[static_cast<size_t>(b)]) candidates.push_back(k);
        }

        for (int j = 0; j < shell.rings; ++j) {
            const float r = shell.RingRadius(j);
            const float x = ca * r;
            const float z = sa * r;

            float h = 0.0f;
            for (int k : candidates) {
                const ShellPeak& s  = sp[static_cast<size_t>(k)];
                const float      dx = x - s.center.x;
                const float      dz = z - s.center.y;
                const float      d2 = dx * dx + dz * dz;
                if (d2 >= s.reach * s.reach) continue;
                h = std::fmax(h, s.height * ShellProfile(std::sqrt(d2) / s.reach, s.lift));
            }

            if (h > 0.0f) {
                h *= 1.0f +
                     kShellRough * (0.5f + 0.5f * core::Fbm2(x, z, seed ^ 0x51E11ull, rough));
            }
            shell.height[static_cast<size_t>(j) * shell.bearings + i] = h;
        }
    }

    // Snap every summit onto the grid. A straight edge between two samples that straddle an apex
    // cuts the corner off it, and the one part of the profile that has to survive intact is the
    // part that clears the camera. Raising the nearest grid point rather than inserting a new one
    // keeps the field single-valued, which is the whole point of building it this way.
    for (const ShellPeak& s : sp) {
        const float centerRadius =
            std::sqrt(s.center.x * s.center.x + s.center.y * s.center.y);
        const int j =
            static_cast<int>((centerRadius - shell.innerRadius) / shell.radialStep + 0.5f);
        if (j < 0 || j >= shell.rings) continue;

        float bearing = std::atan2(s.center.y, s.center.x);
        if (bearing < 0.0f) bearing += core::kTwoPi;
        const int i = static_cast<int>(bearing / bucketAngle + 0.5f) % shell.bearings;

        float& at = shell.height[static_cast<size_t>(j) * shell.bearings + i];
        at        = std::fmax(at, s.height);
    }

    return shell;
}

Mesh BuildHorizon(const Shell& shell, uint64_t seed) {
    Mesh mesh;
    if (shell.rings < 2 || shell.bearings < 2) return mesh;

    const float bucketAngle = core::kTwoPi / static_cast<float>(shell.bearings);

    const auto heightAt = [&](int i, int j) {
        const int ci = ((i % shell.bearings) + shell.bearings) % shell.bearings;
        const int cj = j < 0 ? 0 : (j >= shell.rings ? shell.rings - 1 : j);
        return shell.height[static_cast<size_t>(cj) * shell.bearings + ci];
    };

    const auto positionAt = [&](int i, int j) {
        const int   ci    = ((i % shell.bearings) + shell.bearings) % shell.bearings;
        const float angle = bucketAngle * static_cast<float>(ci);
        const float r     = shell.RingRadius(j < 0 ? 0 : (j >= shell.rings ? shell.rings - 1 : j));
        return Vec3{std::cos(angle) * r, heightAt(i, j), std::sin(angle) * r};
    };

    // Smooth shaded, sharing one vertex between the six triangles that meet at it. The old
    // construction was flat shaded because its facets *were* the rock: seven sides to a peak and
    // nothing else to look at. A height field has its detail in the field, so faceting it only
    // draws the grid, and a regular polar grid drawn on a mountain reads as a wireframe rather
    // than as stone. The normal is the field's own gradient, by central difference.
    mesh.vertices.reserve(static_cast<size_t>(shell.rings) * shell.bearings);

    // Colour is drawn from a smooth field rather than per vertex. A per-vertex draw is the
    // mistake world::Vertex warns about: interpolating independent randoms across a triangle
    // gives contour lines, and on a surface this size they read as a topographic map.
    const core::FbmParams tint = ShellTintParams();

    for (int j = 0; j < shell.rings; ++j) {
        for (int i = 0; i < shell.bearings; ++i) {
            const Vec3 here = positionAt(i, j);

            const Vec3 alongBearing = positionAt(i + 1, j) - positionAt(i - 1, j);
            const Vec3 alongRadius  = positionAt(i, j + 1) - positionAt(i, j - 1);

            Vertex v;
            v.position  = here;
            v.normal    = core::Normalize(core::Cross(alongBearing, alongRadius));
            v.rockiness = 1.0f;  // always rock, never sand

            float draw[6];
            for (int k = 0; k < 6; ++k) {
                const float n = core::Fbm2(here.x, here.z,
                                           seed ^ (0x51E11ull + static_cast<uint64_t>(k) * 0x9E37ull),
                                           tint);
                draw[k] = core::Saturate(0.5f + 0.75f * n);
            }
            v.albedo = core::AlbedoFrom(core::palette::kRock, draw);

            mesh.vertices.push_back(v);
        }
    }

    for (int j = 0; j + 1 < shell.rings; ++j) {
        for (int i = 0; i < shell.bearings; ++i) {
            const int i1 = (i + 1) % shell.bearings;

            const float h00 = heightAt(i, j);
            const float h10 = heightAt(i1, j);
            const float h01 = heightAt(i, j + 1);
            const float h11 = heightAt(i1, j + 1);

            // Flat ground needs no triangles. The terrain skirt is already there, and the gaps
            // between the rows are meant to show it.
            if (h00 <= 0.0f && h10 <= 0.0f && h01 <= 0.0f && h11 <= 0.0f) continue;

            const uint32_t v00 = static_cast<uint32_t>(j) * shell.bearings + i;
            const uint32_t v10 = static_cast<uint32_t>(j) * shell.bearings + i1;
            const uint32_t v01 = static_cast<uint32_t>(j + 1) * shell.bearings + i;
            const uint32_t v11 = static_cast<uint32_t>(j + 1) * shell.bearings + i1;

            // The diagonal alternates. A fixed one leaves a herringbone running round the whole
            // range, which even under smooth shading shows up along the ridgelines.
            if (((i + j) & 1) == 0) {
                const uint32_t tri[6] = {v00, v10, v11, v00, v11, v01};
                mesh.indices.insert(mesh.indices.end(), tri, tri + 6);
            } else {
                const uint32_t tri[6] = {v00, v10, v01, v10, v11, v01};
                mesh.indices.insert(mesh.indices.end(), tri, tri + 6);
            }
        }
    }

    return mesh;
}

float ShellElevation(const Shell& shell, const Vec3& eye, float bearing) {
    if (shell.rings < 2) return -core::kPi;

    const Vec2  dir{std::cos(bearing), std::sin(bearing)};
    const float eyeRadius = std::sqrt(eye.x * eye.x + eye.z * eye.z);

    // Half a ring apart, so a march cannot step over a saddle the grid actually has.
    const float step  = shell.radialStep * 0.5f;
    const float reach = eyeRadius + shell.outerRadius();

    float best = -core::kPi;
    for (float t = step; t <= reach; t += step) {
        const float h = shell.HeightAt(eye.x + dir.x * t, eye.z + dir.y * t);
        if (h <= 0.0f) continue;
        const float elevation = std::atan2(h - eye.y, t);
        if (elevation > best) best = elevation;
    }

    return best;
}

bool ShellClosesHorizon(const Shell& shell, const HorizonParams& params, float* worstElevation) {
    float worst = core::kPi;

    const int   kBearings = 1440;
    const float radii[]   = {params.orbitRadius * 0.7f, params.orbitRadius * 1.3f};
    const float heights[] = {2.0f, params.maxCameraHeight};

    for (int o = 0; o < 16; ++o) {
        const float orbitAngle = core::kTwoPi * static_cast<float>(o) / 16.0f;
        for (float radius : radii) {
            for (float height : heights) {
                const Vec3 eye{std::cos(orbitAngle) * radius, height,
                               std::sin(orbitAngle) * radius};

                for (int b = 0; b < kBearings; ++b) {
                    const float e = ShellElevation(
                        shell, eye,
                        core::kTwoPi * static_cast<float>(b) / static_cast<float>(kBearings));
                    if (e < worst) worst = e;
                }
            }
        }
    }

    if (worstElevation) *worstElevation = worst;
    return worst > 0.0f;
}

}  // namespace world
