#version 450

// The missile (spec 7.1). The only rigid body in the project that moves, so the only pipeline
// with a model matrix.

#include "scene.glsl"

layout(push_constant) uniform MissilePush {
    mat4 model;     // local -> world; a rotation and a translation, nothing else
    vec4 exhaust;   // x = plume emissive magnitude, yzw reserved
} pc;

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec3  inAlbedo;
layout(location = 3) in float inEmissive;  // world::Vertex::rockiness, repurposed (see missile.h)

layout(location = 0) out vec3  vWorldPos;
layout(location = 1) out vec3  vNormal;
layout(location = 2) out vec3  vAlbedo;
layout(location = 3) out float vEmissive;

void main() {
    vec4 world = pc.model * vec4(inPosition, 1.0);

    vWorldPos = world.xyz;
    // The model is rigid, so the upper 3x3 transforms normals as it stands. No inverse transpose:
    // there is no scale here to make one necessary.
    vNormal   = normalize(mat3(pc.model) * inNormal);
    vAlbedo   = inAlbedo;
    vEmissive = inEmissive;

    gl_Position = scene.viewProj * world;
}
