#version 450

// The fireball (spec 7.2): an emissive sphere with a noise-displaced surface.
//
// No vertex buffer. The sphere is a lat-long grid built out of gl_VertexIndex, the same way the
// fragments are built out of gl_InstanceIndex — a mesh of a few thousand vertices that exists for
// one second of the cycle is not worth a buffer, an upload and a destructor.

#include "noise.glsl"
#include "scene.glsl"

// Rings from pole to pole and segments around. The displacement below is low-frequency, so this
// is about as coarse as it can be before the lobes start to show facets.
const int kRings    = 40;
const int kSegments = 72;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vDirection;  // the undisplaced unit direction from the centre
layout(location = 2) out float vDisplace;  // how far the surface moved, -1 to 1

// The churn. It has to turn and rise, because a fireball that only pulses reads as a light bulb.
vec3 ChurnSample(vec3 dir, float t) {
    return dir * 2.6 + vec3(0.0, -t * 0.55, t * 0.17);
}

void main() {
    int quad   = gl_VertexIndex / 6;
    int corner = gl_VertexIndex - quad * 6;

    int ring = quad / kSegments;
    int seg  = quad - ring * kSegments;

    // Two triangles, as offsets into the quad.
    const vec2 offsets[6] =
        vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 o = offsets[corner];

    float phi   = 3.14159265 * (float(ring) + o.y) / float(kRings);
    float theta = 6.28318531 * (float(seg) + o.x) / float(kSegments);

    vec3 dir = vec3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));

    float t = scene.cameraPos.w;
    float n = Fbm3(ChurnSample(dir, t), 4);

    // Displacement, and a squash: a fireball at this stage is wider than it is tall, because it is
    // sitting on the ground it is pushing against.
    float radius = scene.fireLight.w * (1.0 + 0.22 * n);
    vec3  local  = dir * radius;
    local.y     *= 0.86;

    vDirection = dir;
    vDisplace  = n;
    vWorldPos  = scene.fireLight.xyz + local;

    gl_Position = scene.viewProj * vec4(vWorldPos, 1.0);
}
