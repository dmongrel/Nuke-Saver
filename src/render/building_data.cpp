#include "render/building_data.h"

namespace render {
namespace {

// One face, given its outward normal and the two in-plane axes. Written this way rather than as a
// table of 24 hand-entered vertices because the winding is the part that is easy to get wrong and
// impossible to see until back-face culling is on, and here it is stated once.
void AddFace(std::vector<BoxVertex>* vertices, std::vector<uint32_t>* indices, const float n[3],
             const float u[3], const float v[3], const float origin[3]) {
    const uint32_t base = static_cast<uint32_t>(vertices->size());

    // Corners in the order (0,0), (1,0), (1,1), (0,1) of the face's own uv. With u cross v equal
    // to the outward normal, that order is counter-clockwise seen from outside.
    const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    for (const auto& c : uv) {
        BoxVertex vert{};
        for (int k = 0; k < 3; ++k) {
            vert.position[k] = origin[k] + u[k] * c[0] + v[k] * c[1];
            vert.normal[k]   = n[k];
        }
        vert.uv[0] = c[0];
        vert.uv[1] = c[1];
        vertices->push_back(vert);
    }

    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) indices->push_back(base + i);
}

}  // namespace

void BuildUnitCube(std::vector<BoxVertex>* vertices, std::vector<uint32_t>* indices) {
    vertices->clear();
    indices->clear();
    vertices->reserve(24);
    indices->reserve(36);

    // Each face is given as origin + u + v with u cross v pointing outward. The v axis of every
    // side face points up, so the window rows in the fragment shader run the way a floor does.
    const float pX[3] = {1.0f, 0.0f, 0.0f};
    const float mX[3] = {-1.0f, 0.0f, 0.0f};
    const float pY[3] = {0.0f, 1.0f, 0.0f};
    const float mY[3] = {0.0f, -1.0f, 0.0f};
    const float pZ[3] = {0.0f, 0.0f, 1.0f};
    const float mZ[3] = {0.0f, 0.0f, -1.0f};

    const float spanX[3] = {2.0f, 0.0f, 0.0f};
    const float spanY[3] = {0.0f, 1.0f, 0.0f};
    const float spanZ[3] = {0.0f, 0.0f, 2.0f};
    const float negX[3]  = {-2.0f, 0.0f, 0.0f};
    const float negZ[3]  = {0.0f, 0.0f, -2.0f};

    const float cornerMM[3] = {-1.0f, 0.0f, -1.0f};  // low x, ground, low z
    const float cornerPM[3] = {1.0f, 0.0f, -1.0f};
    const float cornerPP[3] = {1.0f, 0.0f, 1.0f};
    const float cornerMP[3] = {-1.0f, 0.0f, 1.0f};
    const float topMM[3]    = {-1.0f, 1.0f, -1.0f};

    AddFace(vertices, indices, pZ, spanX, spanY, cornerMP);  // +z faces out, u runs +x
    AddFace(vertices, indices, mZ, negX, spanY, cornerPM);   // -z faces out, u runs -x
    AddFace(vertices, indices, pX, negZ, spanY, cornerPP);   // +x faces out, u runs -z
    AddFace(vertices, indices, mX, spanZ, spanY, cornerMM);  // -x faces out, u runs +z

    // The roof, and the underside. Note the axis order: for the roof, u must be spanZ and v spanX
    // for the cross product to point *up*. Written the other way round — which reads more
    // naturally — the roof is wound backwards, gets culled, and what is left on screen is the
    // underside drawn in its place, carrying a normal that points into the ground. Every roof in
    // the city came out black under a midday sun, which is the only angle that shows it.
    AddFace(vertices, indices, pY, spanZ, spanX, topMM);
    AddFace(vertices, indices, mY, spanX, spanZ, cornerMM);
}

void PackBuildings(const world::City& city, std::vector<BuildingInstance>* out) {
    out->clear();
    out->reserve(city.buildings.size());

    for (const world::Building& b : city.buildings) {
        BuildingInstance inst{};

        inst.centerRotation[0] = b.center.x;
        inst.centerRotation[1] = b.height;
        inst.centerRotation[2] = b.center.y;
        inst.centerRotation[3] = b.rotation;

        inst.extentGrowth[0] = b.halfExtent.x;
        inst.extentGrowth[1] = b.halfExtent.y;
        inst.extentGrowth[2] = b.growthStart;
        inst.extentGrowth[3] = b.growthDuration;

        inst.bodyColor[0] = b.bodyColor.x;
        inst.bodyColor[1] = b.bodyColor.y;
        inst.bodyColor[2] = b.bodyColor.z;
        inst.bodyColor[3] = b.windowSeed;

        inst.windowColor[0] = b.windowColor.x;
        inst.windowColor[1] = b.windowColor.y;
        inst.windowColor[2] = b.windowColor.z;
        inst.windowColor[3] = 0.0f;

        out->push_back(inst);
    }
}

}  // namespace render
