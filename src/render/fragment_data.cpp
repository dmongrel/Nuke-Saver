#include "render/fragment_data.h"

#include <cmath>

namespace render {
namespace {

// The surface area of a box, which is what decides how finely it is worth cutting.
float BoxArea(float hx, float hz, float height) {
    const float w = hx * 2.0f;
    const float d = hz * 2.0f;
    return 2.0f * (w * height + d * height + w * d);
}

// How big a triangle should be, in square metres, at each quality level. Derived from what a
// typical building gets: at quality 1 a 30 m by 30 m by 100 m block has about 14,000 m2 of surface
// and 200 triangles, which is 70 m2 apiece.
float TargetTriangleArea(int quality) {
    static const float kAreas[4] = {46.0f, 70.0f, 117.0f, 233.0f};
    if (quality < 0) quality = 0;
    if (quality > 3) quality = 3;
    return kAreas[quality];
}

int SideQuadsForArea(float area, int quality) {
    const float triangles = area / TargetTriangleArea(quality);

    // Triangles are 10 * S once S is even, which is the regime this lands in for everything bigger
    // than a segment bar. One quad a side is the floor: below that a box stops being cut at all.
    int s = static_cast<int>(std::lround(triangles / 10.0f));
    if (s < 1) s = 1;
    if (s > 30) s = 30;
    return s;
}

void Push(std::vector<ShatterBox>* out, uint32_t* next, const ShatterBox& box, int sideQuads) {
    ShatterBox b = box;
    b.extentHeight[3] = static_cast<float>(sideQuads);
    b.color[3]        = static_cast<float>(*next);
    out->push_back(b);
    *next += static_cast<uint32_t>(SideQuadsToTriangles(sideQuads));
}

}  // namespace

int SideQuadsForQuality(int quality) {
    static const int kLevels[4] = {30, 20, 12, 6};
    if (quality < 0) quality = 0;
    if (quality > 3) quality = 3;
    return kLevels[quality];
}

void PackShatterBoxes(const world::City& city, const world::Board& board, int quality,
                      std::vector<ShatterBox>* out, FragmentLayout* layout) {
    out->clear();
    out->reserve(city.buildings.size() + board.boxes.size());

    uint32_t next = 0;

    const int buildingQuads = SideQuadsForQuality(quality);

    for (const world::Building& b : city.buildings) {
        ShatterBox box{};
        box.centerYaw[0] = b.center.x;
        box.centerYaw[1] = 0.0f;  // buildings stand on the ground
        box.centerYaw[2] = b.center.y;
        box.centerYaw[3] = b.rotation;

        box.extentHeight[0] = b.halfExtent.x;
        box.extentHeight[1] = b.halfExtent.y;
        box.extentHeight[2] = b.height;

        box.color[0] = b.bodyColor.x;
        box.color[1] = b.bodyColor.y;
        box.color[2] = b.bodyColor.z;

        Push(out, &next, box, buildingQuads);
    }

    for (const world::BoardBox& b : board.boxes) {
        ShatterBox box{};
        box.centerYaw[0] = b.center.x;
        box.centerYaw[1] = b.base;
        box.centerYaw[2] = b.center.y;
        box.centerYaw[3] = b.yaw;

        box.extentHeight[0] = b.halfExtent.x;
        box.extentHeight[1] = b.halfExtent.y;
        box.extentHeight[2] = b.height;

        // A segment bar carries the lit amber into the cloud even though it is dark by the time
        // the blast arrives. That is the point of spec 7.3's requirement: the cloud should read as
        // the city and the board it was made of, and amber among the grey is how the board shows.
        const core::Vec3 color = b.segmentId >= 0 ? board.litColor : board.faceColor;
        box.color[0]           = color.x;
        box.color[1]           = color.y;
        box.color[2]           = color.z;

        Push(out, &next, box,
             SideQuadsForArea(BoxArea(b.halfExtent.x, b.halfExtent.y, b.height), quality));
    }

    layout->boxes = static_cast<uint32_t>(out->size());
    layout->total = next;
}

}  // namespace render
