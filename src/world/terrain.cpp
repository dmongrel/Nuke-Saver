#include "world/terrain.h"

#include "core/color.h"
#include "core/noise.h"
#include "core/rng.h"

namespace world {
namespace {

using core::FbmParams;
using core::Vec3;

// Two fields, doing two different jobs. Dunes are broad and gentle and cover everything; the
// ridged field supplies the mesas and the rim of hills, and is weighted so it only appears away
// from the centre — that weighting is what makes the basin a basin rather than noise everywhere.
FbmParams DuneParams() {
    FbmParams p;
    p.octaves    = 6;  // spec 6.2 asks for 5 to 7
    p.frequency  = 1.0f / 1100.0f;
    p.lacunarity = 2.05f;
    p.gain       = 0.48f;
    return p;
}

FbmParams RidgeParams() {
    FbmParams p;
    p.octaves    = 5;
    p.frequency  = 1.0f / 2400.0f;
    p.lacunarity = 2.2f;
    p.gain       = 0.52f;
    return p;
}

constexpr float kDuneAmplitude  = 16.0f;
constexpr float kRidgeAmplitude = 240.0f;

// The colour variation of spec 5.2 as a field over the ground rather than a draw per vertex.
//
// The desert is one continuous surface, not a set of instances, so "each draws its own colour"
// has to be read as "the colour varies" — and the variation needs a scale in metres. Given one
// random value per vertex instead, the thing being coloured is the mesh: on a radial grid seen
// from a low camera the rings compress along the view direction to nothing while the segments do
// not, and the desert came out as a fine speckle converging on the viewer. A field at 40-110 m
// compresses the same way and simply reads as a gradient.
//
// Six channels at six wavelengths, so no two of hue, saturation and value share a pattern.
void ColorDraw(uint64_t seed, float x, float z, float draw[6]) {
    static const float kWavelength[6] = {74.0f, 113.0f, 91.0f, 37.0f, 52.0f, 43.0f};

    for (int i = 0; i < 6; ++i) {
        FbmParams p;
        p.octaves    = 3;
        p.frequency  = 1.0f / kWavelength[i];
        p.lacunarity = 2.1f;
        p.gain       = 0.5f;

        // Fbm2 is roughly [-1, 1] but rarely reaches either end, so the remap is scaled to use
        // more of the palette range than a straight 0.5 + 0.5n would.
        const float n =
            core::Fbm2(x, z, seed ^ (0xC010Bull + static_cast<uint64_t>(i) * 0x9E37ull), p);
        draw[i] = core::Saturate(0.5f + 0.75f * n);
    }
}

}  // namespace

core::Vec3 GroundAlbedo(const TerrainParams& params, float x, float z, float rockiness) {
    float draw[6];
    ColorDraw(params.seed, x, z, draw);
    return core::Lerp(core::AlbedoFrom(core::palette::kDesertFloor, draw),
                      core::AlbedoFrom(core::palette::kRock, draw), core::Saturate(rockiness));
}

float TerrainHeight(const TerrainParams& params, float x, float z) {
    const float r = std::sqrt(x * x + z * z);

    // Beyond the basin the ground flattens to zero and stays there. The skirt exists to give the
    // horizon range something to stand on, not to be looked at, and any relief out there would
    // only compete with the mountains it is supposed to sit under.
    const float basinFade =
        1.0f - core::SmoothStep((r - params.basinRadius * 0.82f) / (params.basinRadius * 0.30f));

    const float dunes =
        core::Fbm2(x, z, params.seed ^ 0xD00Eull, DuneParams()) * kDuneAmplitude;

    // The rim rises with distance from the centre, so the middle of the basin stays open ground.
    const float rimWeight = core::SmoothStep(
        (r - params.cityRadius * 1.7f) / (params.basinRadius * 0.85f - params.cityRadius * 1.7f));
    const float ridge =
        core::Ridged2(x, z, params.seed ^ 0x21D6Eull, RidgeParams()) * kRidgeAmplitude * rimWeight;

    float height = (dunes + ridge) * basinFade;

    // Flatten where the city will stand. Fully flat inside the footprint, easing out over a
    // margin so the pad does not read as a plateau someone cut.
    const float pad = core::SmoothStep((params.cityRadius * 1.35f - r) /
                                       (params.cityRadius * 1.35f - params.cityRadius * 0.9f));
    height          = core::Lerp(height, 0.0f, core::Saturate(pad));

    return height;
}

core::Vec3 TerrainNormal(const TerrainParams& params, float x, float z) {
    // Stepped at the near ring spacing rather than something arbitrarily small: the normal should
    // describe the surface the mesh actually has, not a finer one it does not.
    const float e = params.nearSpacing * 0.5f;

    const float hx = TerrainHeight(params, x + e, z) - TerrainHeight(params, x - e, z);
    const float hz = TerrainHeight(params, x, z + e) - TerrainHeight(params, x, z - e);

    return core::Normalize(Vec3{-hx, 2.0f * e, -hz});
}

Mesh BuildTerrain(const TerrainParams& params) {
    Mesh mesh;

    // Ring radii: fixed spacing out to where proportional spacing overtakes it, then geometric.
    // Expressed as a rule rather than a table so changing nearSpacing changes the whole mesh
    // coherently instead of needing the table re-tuned.
    std::vector<float> radii;
    radii.push_back(0.0f);
    for (float r = 0.0f; r < params.skirtRadius;) {
        const float step = std::fmax(params.nearSpacing, r * params.spacingGrowth);
        r += step;
        radii.push_back(std::fmin(r, params.skirtRadius));
    }

    const uint32_t segments   = params.segments;
    const uint32_t ringCount  = static_cast<uint32_t>(radii.size());

    mesh.vertices.reserve(static_cast<size_t>(ringCount) * segments + 1);

    // The centre vertex. A radial grid has a singularity there; one vertex fills it, and the city
    // stands on top of it anyway.
    {
        Vertex v;
        v.position  = Vec3{0.0f, TerrainHeight(params, 0.0f, 0.0f), 0.0f};
        v.normal    = TerrainNormal(params, 0.0f, 0.0f);
        v.rockiness = 0.0f;
        v.albedo    = GroundAlbedo(params, 0.0f, 0.0f, 0.0f);
        mesh.vertices.push_back(v);
    }

    for (uint32_t ring = 1; ring < ringCount; ++ring) {
        const float radius = radii[ring];
        for (uint32_t s = 0; s < segments; ++s) {
            const float angle = core::kTwoPi * static_cast<float>(s) / static_cast<float>(segments);
            const float x     = std::cos(angle) * radius;
            const float z     = std::sin(angle) * radius;

            Vertex v;
            v.position = Vec3{x, TerrainHeight(params, x, z), z};
            v.normal   = TerrainNormal(params, x, z);

            // Rock shows on the steep faces of the mesas and the rim; the flats stay sand.
            // Derived from the surface itself, so it cannot disagree with the geometry.
            v.rockiness = core::Saturate((1.0f - v.normal.y) * 3.2f);

            // Sampled at the world position, so refining the mesh refines the colour instead of
            // reshuffling it.
            v.albedo = GroundAlbedo(params, x, z, v.rockiness);

            mesh.vertices.push_back(v);
        }
    }

    const auto ringBase = [segments](uint32_t ring) -> uint32_t {
        return 1 + (ring - 1) * segments;  // ring 0 is the single centre vertex
    };

    mesh.indices.reserve(static_cast<size_t>(ringCount) * segments * 6);

    // The innermost ring fans from the centre vertex.
    for (uint32_t s = 0; s < segments; ++s) {
        const uint32_t next = (s + 1) % segments;
        mesh.indices.push_back(0);
        mesh.indices.push_back(ringBase(1) + next);
        mesh.indices.push_back(ringBase(1) + s);
    }

    for (uint32_t ring = 1; ring + 1 < ringCount; ++ring) {
        const uint32_t inner = ringBase(ring);
        const uint32_t outer = ringBase(ring + 1);

        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t next = (s + 1) % segments;

            // Counter-clockwise when seen from above, matching the front face the pipeline
            // expects. Getting this backwards is invisible while culling is off and then makes
            // the ground vanish the moment it is turned on.
            mesh.indices.push_back(inner + s);
            mesh.indices.push_back(inner + next);
            mesh.indices.push_back(outer + next);

            mesh.indices.push_back(inner + s);
            mesh.indices.push_back(outer + next);
            mesh.indices.push_back(outer + s);
        }
    }

    return mesh;
}

}  // namespace world
