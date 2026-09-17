#version 450

// Every fragment in the world, drawn into the key light's depth map (spec 8.2). The same instanced
// call as fragment.vert — three vertices, no vertex buffer — with the light's matrix in place of
// the camera's and nothing carried to a fragment stage, because there is not one.

#include "scene.glsl"

#define FRAGMENT_SET 1
#define FRAGMENT_NO_PUSH
#include "fragment_common.glsl"

void main() {
    uint i = uint(gl_InstanceIndex);

    FragState s = state[i];
    FragRest  r = rest[i];

    // A fragment whose building still stands is not drawn — the building is, and it casts its own
    // shadow. Same test as fragment.vert, for the same reason.
    if (s.pos.w < 0.5) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);
        return;
    }

    vec3 local = gl_VertexIndex == 0 ? r.c0.xyz : (gl_VertexIndex == 1 ? r.c1.xyz : r.c2.xyz);
    gl_Position = scene.lightViewProj * vec4(s.pos.xyz + QuatRotate(s.quat, local), 1.0);
}
