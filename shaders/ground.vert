#version 450

// The desert floor and the horizon range. One pipeline for both: they are lit by the same sun and
// swallowed by the same haze, and what tells a mountain from a dune is the per-vertex rockiness,
// not a different shader.

#include "scene.glsl"

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec3  inAlbedo;
layout(location = 3) in float inRockiness;

layout(location = 0) out vec3  vWorldPos;
layout(location = 1) out vec3  vNormal;
layout(location = 2) out vec3  vAlbedo;
layout(location = 3) out float vRockiness;

void main() {
    vWorldPos  = inPosition;
    vNormal    = inNormal;
    vAlbedo    = inAlbedo;
    vRockiness = inRockiness;

    gl_Position = scene.viewProj * vec4(inPosition, 1.0);
}
