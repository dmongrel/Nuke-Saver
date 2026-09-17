// Static world geometry (implementation plan, M3).
//
// One vertex format for everything generated on the CPU and never animated: the terrain and the
// horizon range. Both are lit by the same sun and hazed by the same distance term, so they share
// a pipeline; what distinguishes a mountain from the desert floor is a per-vertex value, not a
// separate shader.
#ifndef NUKE_SAVER_WORLD_MESH_H
#define NUKE_SAVER_WORLD_MESH_H

#include "core/math.h"

#include <cstdint>
#include <vector>

namespace world {

// 40 bytes.
struct Vertex {
    core::Vec3 position;
    core::Vec3 normal;

    // Linear scene-referred albedo, already carrying the per-instance variation of spec 5.2.
    //
    // Resolved on the CPU rather than in the shader, for two reasons. It keeps the palette in one
    // place — core/color.h — instead of a second transcription in GLSL that would drift from it.
    // And a shader deriving independent channels from one interpolated random has to reach for
    // something like fract(v * 7.31), which is a sawtooth across every triangle: that rendered as
    // a maze of contour lines over the whole desert, and it is not a bug that any amount of
    // tuning fixes, because the problem is asking a linear interpolant for uncorrelated values.
    core::Vec3 albedo;

    // 0 is sand, 1 is rock. Still needed after the albedo is resolved: it gates the dune normal
    // detail, which belongs on sand and would read as fur on a mountain face.
    float rockiness = 0.0f;
};

struct Mesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;

    bool   empty() const { return indices.empty(); }
    size_t vertexBytes() const { return vertices.size() * sizeof(Vertex); }
    size_t indexBytes() const { return indices.size() * sizeof(uint32_t); }
};

}  // namespace world

#endif
