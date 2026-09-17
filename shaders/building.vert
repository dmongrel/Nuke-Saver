#version 450

// One unit cube, five hundred instances (spec 6.4), rising from the ground (spec 6.5).
//
// The cube spans [-1,1] in x and z and [0,1] in y, so the growth scale is a plain multiply on y:
// the building grows up out of the ground instead of expanding about its own middle.
//
// The placement itself is in building_common.glsl, because the shadow pass has to work it out the
// same way (spec 8.2).

#include "scene.glsl"
#include "building_common.glsl"

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
    BuildingVertex b =
        BuildingPlace(inPosition, inNormal, inUv, inCenterRotation, inExtentGrowth);

    // A building MUST NOT be visible before its start time. Scaling to zero is not enough — that
    // leaves a flat quad lying on the ground, and five hundred of them make a visible sheet. So
    // the whole triangle is pushed outside the clip volume instead: z < 0 fails 0 <= z <= w.
    if (b.hidden) {
        gl_Position  = vec4(0.0, 0.0, -1.0, 1.0);
        vWorldPos    = vec3(0.0);
        vNormal      = vec3(0.0, 1.0, 0.0);
        vBodyColor   = vec3(0.0);
        vWindowColor = vec3(0.0);
        vFacadeU     = 0.0;
        vWindowSeed  = 0.0;
        return;
    }

    vWorldPos    = b.world;
    vNormal      = b.normal;
    vBodyColor   = inBodyColor.rgb;
    vWindowColor = inWindowColor.rgb;
    vFacadeU     = b.facadeU;
    vWindowSeed  = inBodyColor.w;

    gl_Position = scene.viewProj * vec4(b.world, 1.0);
}
