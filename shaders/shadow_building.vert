#version 450

// The city, drawn into the key light's depth map (spec 8.2). Position only: there is no fragment
// shader on this pipeline and no colour attachment to write to.
//
// Everything about where a building is and whether it is standing comes from the same
// building_common.glsl the scene pass uses, so the map and the picture cannot drift apart.

#include "scene.glsl"
#include "building_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 3) in vec4 inCenterRotation;
layout(location = 4) in vec4 inExtentGrowth;
layout(location = 5) in vec4 inBodyColor;
layout(location = 6) in vec4 inWindowColor;

void main() {
    BuildingVertex b =
        BuildingPlace(inPosition, inNormal, inUv, inCenterRotation, inExtentGrowth);

    if (b.hidden) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);
        return;
    }
    gl_Position = scene.lightViewProj * vec4(b.world, 1.0);
}
