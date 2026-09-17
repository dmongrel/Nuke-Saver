#include "world/missile.h"

#include "core/color.h"
#include "core/rng.h"

#include <cmath>

namespace world {
namespace {

constexpr int kSegments = 16;  // around the body

// Proportions, as fractions of the overall length. Nose, then body, then the taper into the
// nozzle; the fins straddle the last quarter.
constexpr float kNoseEnd  = 0.26f;
constexpr float kBodyEnd  = 0.84f;
constexpr float kNozzle   = 0.74f;  // nozzle radius as a fraction of the body radius
constexpr float kFinFront = 0.72f;
constexpr float kFinSpan  = 1.70f;  // how far past the body radius a fin reaches

void AddVertex(Mesh* mesh, const core::Vec3& p, const core::Vec3& n, const core::Vec3& albedo,
               float emissive) {
    Vertex v;
    v.position  = p;
    v.normal    = n;
    v.albedo    = albedo;
    v.rockiness = emissive;
    mesh->vertices.push_back(v);
}

void AddTriangle(Mesh* mesh, uint32_t a, uint32_t b, uint32_t c) {
    mesh->indices.push_back(a);
    mesh->indices.push_back(b);
    mesh->indices.push_back(c);
}

// One ring of a body of revolution about the Z axis, and the quad band joining it to the ring
// before it. Normals are the radial direction tilted by the local slope, so the nose and the tail
// taper shade as cones rather than as cylinders wearing the wrong normal.
void AddRing(Mesh* mesh, float z, float radius, float slope, const core::Vec3& albedo,
             bool joinToPrevious) {
    const uint32_t first = static_cast<uint32_t>(mesh->vertices.size());

    for (int i = 0; i < kSegments; ++i) {
        const float a = core::kPi * 2.0f * (static_cast<float>(i) / kSegments);
        const float c = std::cos(a), s = std::sin(a);

        const core::Vec3 normal = core::Normalize(core::Vec3{c, s, slope});
        AddVertex(mesh, core::Vec3{c * radius, s * radius, z}, normal, albedo, 0.0f);
    }

    if (!joinToPrevious) return;

    const uint32_t previous = first - kSegments;
    for (int i = 0; i < kSegments; ++i) {
        const uint32_t j = static_cast<uint32_t>((i + 1) % kSegments);
        AddTriangle(mesh, previous + i, first + i, first + j);
        AddTriangle(mesh, previous + i, first + j, previous + j);
    }
}

// A fin: a flat plate, emitted as two sheets back to back so it is lit from either side without
// the pipeline having to turn culling off for the whole missile.
void AddFin(Mesh* mesh, float angle, float zFront, float zBack, float rInner, float rOuter,
            float thickness, const core::Vec3& albedo) {
    const float c = std::cos(angle), s = std::sin(angle);

    const core::Vec3 out{c, s, 0.0f};
    const core::Vec3 side{-s, c, 0.0f};

    // Swept back: the outer edge trails the inner one, which is what makes it read as a fin
    // rather than as a tab.
    const float sweep = (zFront - zBack) * 0.45f;

    const core::Vec3 corners[4] = {
        out * rInner + core::Vec3{0.0f, 0.0f, zFront},
        out * rOuter + core::Vec3{0.0f, 0.0f, zFront - sweep},
        out * rOuter + core::Vec3{0.0f, 0.0f, zBack},
        out * rInner + core::Vec3{0.0f, 0.0f, zBack},
    };

    for (int face = 0; face < 2; ++face) {
        const float      sign   = face == 0 ? 1.0f : -1.0f;
        const core::Vec3 offset = side * (thickness * 0.5f * sign);
        const core::Vec3 normal = side * sign;

        const uint32_t first = static_cast<uint32_t>(mesh->vertices.size());
        for (int i = 0; i < 4; ++i) AddVertex(mesh, corners[i] + offset, normal, albedo, 0.0f);

        if (face == 0) {
            AddTriangle(mesh, first, first + 1, first + 2);
            AddTriangle(mesh, first, first + 2, first + 3);
        } else {
            AddTriangle(mesh, first, first + 2, first + 1);
            AddTriangle(mesh, first, first + 3, first + 2);
        }
    }
}

// The exhaust plume (spec 7.1): a cone off the nozzle, emissive at the throat and fading to
// nothing at the tip. It is geometry rather than particles because the particle systems of spec
// 8.3 arrive with M6, and a missile with no flame is further from the spec than one with a
// simple flame.
void AddPlume(Mesh* mesh, float nozzleZ, float nozzleRadius, float plumeLength) {
    // Spec 5.1 puts the exhaust at 20 to 40 linear. The colour is the same white-to-orange the
    // fireball starts at, because it is the same kind of thing at a much smaller scale.
    const core::Vec3 hot{1.00f, 0.86f, 0.55f};

    const uint32_t tip = static_cast<uint32_t>(mesh->vertices.size());
    AddVertex(mesh, core::Vec3{0.0f, 0.0f, nozzleZ - plumeLength}, core::Vec3{0.0f, 0.0f, -1.0f},
              hot, 0.0f);

    // Three rings: a bright throat, a wider belly and a closing tail, so the plume has a shape
    // rather than being one linear ramp from the nozzle to a point.
    const float rings[3][3] = {
        // fraction of the plume length behind the nozzle, radius scale, emissive
        {0.00f, 1.00f, 1.00f},
        {0.34f, 1.45f, 0.62f},
        {0.68f, 1.05f, 0.24f},
    };

    uint32_t previous = 0;
    for (int r = 0; r < 3; ++r) {
        const float    z     = nozzleZ - plumeLength * rings[r][0];
        const float    rad   = nozzleRadius * rings[r][1];
        const uint32_t first = static_cast<uint32_t>(mesh->vertices.size());

        for (int i = 0; i < kSegments; ++i) {
            const float a = core::kPi * 2.0f * (static_cast<float>(i) / kSegments);
            const float c = std::cos(a), s = std::sin(a);
            AddVertex(mesh, core::Vec3{c * rad, s * rad, z},
                      core::Normalize(core::Vec3{c, s, 0.3f}), hot, rings[r][2]);
        }

        if (r > 0) {
            for (int i = 0; i < kSegments; ++i) {
                const uint32_t j = static_cast<uint32_t>((i + 1) % kSegments);
                AddTriangle(mesh, previous + i, first + i, first + j);
                AddTriangle(mesh, previous + i, first + j, previous + j);
            }
        }
        previous = first;
    }

    // Close it on the tip.
    for (int i = 0; i < kSegments; ++i) {
        const uint32_t j = static_cast<uint32_t>((i + 1) % kSegments);
        AddTriangle(mesh, previous + i, tip, previous + j);
    }
}

}  // namespace

Mesh BuildMissileMesh(uint64_t seed, float length, float radius) {
    core::Rng rng = core::Rng(seed).Fork(0x1551Eull);

    const core::Vec3 body = core::Albedo(core::palette::kMissileBody, seed, rng.NextU64(), 0x11ull);

    // Spec 5.3 gives no separate colour for the fins, and a missile whose fins match its body
    // exactly reads as a single extrusion. They take the body colour darkened, which is the
    // derivation 5.3 uses everywhere else it needs a related tone.
    const core::Vec3 fin = core::HsvToRgb(core::Darken(core::RgbToHsv(body), 0.30f));

    Mesh mesh;
    mesh.vertices.reserve(kSegments * 8 + 40);

    const float noseZ   = -length * kNoseEnd;
    const float bodyZ   = -length * kBodyEnd;
    const float tailZ   = -length;
    const float nozzleR = radius * kNozzle;

    // The nose apex, as a fan rather than a degenerate ring: a ring of coincident vertices would
    // give the tip sixteen normals pointing sixteen ways.
    const uint32_t apex = static_cast<uint32_t>(mesh.vertices.size());
    AddVertex(&mesh, core::Vec3{0.0f, 0.0f, 0.0f}, core::Vec3{0.0f, 0.0f, 1.0f}, body, 0.0f);

    const float noseSlope = radius / (length * kNoseEnd);
    AddRing(&mesh, noseZ, radius, noseSlope, body, false);

    const uint32_t noseRing = apex + 1;
    for (int i = 0; i < kSegments; ++i) {
        const uint32_t j = static_cast<uint32_t>((i + 1) % kSegments);
        AddTriangle(&mesh, apex, noseRing + j, noseRing + i);
    }

    AddRing(&mesh, bodyZ, radius, 0.0f, body, true);
    AddRing(&mesh, tailZ, nozzleR, (radius - nozzleR) / (length * (1.0f - kBodyEnd)), body, true);

    for (int f = 0; f < 4; ++f) {
        AddFin(&mesh, core::kPi * 0.5f * static_cast<float>(f), -length * kFinFront, tailZ, radius,
               radius * kFinSpan, radius * 0.22f, fin);
    }

    // Short. A plume longer than the missile made the whole object read as a streak of light with
    // a speck at the front, which is the opposite of spec 7.1's deliberate machine.
    AddPlume(&mesh, tailZ, nozzleR, length * 0.32f);

    return mesh;
}

core::Mat4 MissileTransform(const core::Vec3& position, const core::Vec3& direction) {
    // An orthonormal basis with +Z on the flight path. The up hint is world up, and the fallback
    // covers the case the spec's "shallow descent" never actually reaches: a vertical dive.
    const core::Vec3 forward = direction;

    core::Vec3 right = core::Cross(core::Vec3{0.0f, 1.0f, 0.0f}, forward);
    if (core::LengthSq(right) < 1e-6f) right = core::Vec3{1.0f, 0.0f, 0.0f};
    right = core::Normalize(right);

    const core::Vec3 up = core::Cross(forward, right);

    core::Mat4 m = core::Mat4::Identity();
    m.m[0][0]    = right.x;
    m.m[0][1]    = right.y;
    m.m[0][2]    = right.z;
    m.m[1][0]    = up.x;
    m.m[1][1]    = up.y;
    m.m[1][2]    = up.z;
    m.m[2][0]    = forward.x;
    m.m[2][1]    = forward.y;
    m.m[2][2]    = forward.z;
    m.m[3][0]    = position.x;
    m.m[3][1]    = position.y;
    m.m[3][2]    = position.z;
    return m;
}

}  // namespace world
