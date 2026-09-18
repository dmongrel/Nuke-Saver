#version 450

// One call for every fragment in the world (spec 7.3). Three consecutive vertices per fragment and
// no vertex buffer at all: gl_VertexIndex names the fragment and the corner, and everything else is
// read out of the same two storage buffers the simulation writes.
//
// Not instanced, although it reads like instancing. The hardware packs vertex waves poorly when
// every instance is three vertices, so a plain draw of 3N vertices keeps the waves full; the
// vertices and their order are the same either way.

#include "scene.glsl"

#define FRAGMENT_SET 1
#define FRAGMENT_NO_PUSH
#include "fragment_common.glsl"

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;

void main() {
    uint i      = uint(gl_VertexIndex) / 3u;
    uint corner = uint(gl_VertexIndex) - i * 3u;

    FragState s = state[i];
    FragRest  r = rest[i];

    // A fragment whose building still stands is not drawn — the building is. Pushed outside the
    // clip volume rather than branched around, because there is no way for a vertex shader to
    // decline to produce a vertex.
    if (s.pos.w < 0.5) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);
        vWorldPos   = vec3(0.0);
        vNormal     = vec3(0.0, 1.0, 0.0);
        vColor      = vec3(0.0);
        return;
    }

    vec3 local = corner == 0u ? r.c0.xyz : (corner == 1u ? r.c1.xyz : r.c2.xyz);

    vec3 world = s.pos.xyz + QuatRotate(s.quat, local);

    // The face normal, from the rest corners and then turned with the body. Cheaper than storing
    // it, and it cannot disagree with the triangle it belongs to.
    vec3 n = normalize(cross(r.c1.xyz - r.c0.xyz, r.c2.xyz - r.c0.xyz));

    vWorldPos = world;
    vNormal   = QuatRotate(s.quat, n);
    vColor    = vec3(r.c0.w, r.c1.w, r.c2.w);

    gl_Position = scene.viewProj * vec4(world, 1.0);
}
