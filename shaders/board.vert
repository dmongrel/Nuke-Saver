#version 450

// The countdown board (spec 7.6). Masts, face panels and segment bars, all instances of the same
// unit cube the city uses.

#include "scene.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 3) in vec4 inBaseYaw;  // xz centre, y underside, w yaw
layout(location = 4) in vec4 inSizeId;   // xz half-extent, y height, w segment id

layout(push_constant) uniform Push {
    uvec2 litMask;  // bit per segment id, so all four faces change in one frame
    float rise;     // 0 to 1, the board coming out of the ground (spec 6.5)
    float emissive; // linear scale for a lit bar (spec 5.1 puts it at 30-60)
} pc;

layout(location = 0) out vec3  vWorldPos;
layout(location = 1) out vec3  vNormal;
layout(location = 2) out float vLit;
layout(location = 3) out float vIsSegment;

void main() {
    // The whole board rises as one object, scaling about the ground plane — not each box about its
    // own base, which would leave a segment bar hanging at its final height and merely shrink.
    float rise = clamp(pc.rise, 0.0, 1.0);
    if (rise <= 0.0) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);  // outside the clip volume
        vWorldPos   = vec3(0.0);
        vNormal     = vec3(0.0, 1.0, 0.0);
        vLit        = 0.0;
        vIsSegment  = 0.0;
        return;
    }

    vec3 local = vec3(inPosition.x * inSizeId.x, inPosition.y * inSizeId.y,
                      inPosition.z * inSizeId.z);

    float c = cos(inBaseYaw.w);
    float s = sin(inBaseYaw.w);

    vec3 world = vec3(local.x * c - local.z * s + inBaseYaw.x,
                      (local.y + inBaseYaw.y) * rise,
                      local.x * s + local.z * c + inBaseYaw.z);

    vec3 n = vec3(inNormal.x * c - inNormal.z * s, inNormal.y, inNormal.x * s + inNormal.z * c);

    int  id       = int(inSizeId.w + 0.5);
    bool segment  = inSizeId.w >= -0.5;

    // 56 segment ids across two 32-bit words, because push constants have no 64-bit integer here.
    uint word = id < 32 ? pc.litMask.x : pc.litMask.y;
    uint bit  = uint(id < 32 ? id : id - 32);

    vWorldPos  = world;
    vNormal    = n;
    vIsSegment = segment ? 1.0 : 0.0;
    vLit       = (segment && ((word >> bit) & 1u) != 0u) ? 1.0 : 0.0;

    gl_Position = scene.viewProj * vec4(world, 1.0);
}
