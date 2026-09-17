#version 450

// One unit cube, five hundred instances (spec 6.4), rising from the ground (spec 6.5).
//
// The cube spans [-1,1] in x and z and [0,1] in y, so the growth scale is a plain multiply on y:
// the building grows up out of the ground instead of expanding about its own middle.

#include "scene.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

// Per instance. Matches render::BuildingInstance.
layout(location = 3) in vec4 inCenterRotation;  // xz centre, y height, w yaw
layout(location = 4) in vec4 inExtentGrowth;    // xy half-extent, z start, w duration
layout(location = 5) in vec4 inBodyColor;       // rgb, w window seed
layout(location = 6) in vec4 inWindowColor;     // rgb, w reserved (M4)

layout(location = 0) out vec3  vWorldPos;
layout(location = 1) out vec3  vNormal;
layout(location = 2) out vec3  vBodyColor;
layout(location = 3) out vec3  vWindowColor;
layout(location = 4) out float vFacadeU;    // metres along the face, for window columns
layout(location = 5) out float vWindowSeed;

void main() {
    float start    = inExtentGrowth.z;
    float duration = max(inExtentGrowth.w, 1e-3);

    // scene.timing.x is seconds into the growth phase, which the timeline of spec 4 places
    // wherever it likes in the cycle. Negative before the phase begins, which is what keeps the
    // desert empty through phase 0.
    float progress = (scene.timing.x - start) / duration;

    // Spec 6.5: ease out, and MUST NOT overshoot or bounce. A cubic ease-out is monotonic and
    // reaches exactly 1, which a spring or a back-ease would not.
    float t     = clamp(progress, 0.0, 1.0);
    float eased = 1.0 - pow(1.0 - t, 3.0);

    // A building MUST NOT be visible before its start time. Scaling to zero is not enough — that
    // leaves a flat quad lying on the ground, and five hundred of them make a visible sheet. So
    // the whole triangle is pushed outside the clip volume instead: z < 0 fails 0 <= z <= w.
    if (progress <= 0.0) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);
        vWorldPos   = vec3(0.0);
        vNormal     = vec3(0.0, 1.0, 0.0);
        vBodyColor  = vec3(0.0);
        vWindowColor = vec3(0.0);
        vFacadeU    = 0.0;
        vWindowSeed = 0.0;
        return;
    }

    vec2  halfExtent = inExtentGrowth.xy;
    float height     = inCenterRotation.y * eased;

    vec3 local = vec3(inPosition.x * halfExtent.x, inPosition.y * height,
                      inPosition.z * halfExtent.y);

    float c = cos(inCenterRotation.w);
    float s = sin(inCenterRotation.w);

    vec3 world = vec3(local.x * c - local.z * s + inCenterRotation.x, local.y,
                      local.x * s + local.z * c + inCenterRotation.z);

    // The normal rotates with the building but is not scaled: the transform is a rotation and a
    // per-axis scale, and for an axis-aligned box the face normals are the scale's own axes, so
    // they survive it unchanged.
    vec3 n = vec3(inNormal.x * c - inNormal.z * s, inNormal.y, inNormal.x * s + inNormal.z * c);

    // Metres along the facade. Which half-extent that is depends on which pair of faces this is:
    // the faces normal to x run along z, and vice versa. Taken from the *unrotated* normal, since
    // that is the one still aligned to the extents.
    float faceWidth = abs(inNormal.x) > 0.5 ? halfExtent.y : halfExtent.x;

    vWorldPos    = world;
    vNormal      = n;
    vBodyColor   = inBodyColor.rgb;
    vWindowColor = inWindowColor.rgb;
    vFacadeU     = inUv.x * faceWidth * 2.0;
    vWindowSeed  = inBodyColor.w;

    gl_Position = scene.viewProj * vec4(world, 1.0);
}
