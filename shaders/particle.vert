#version 450

// Particle billboards (spec 8.3). Six vertices an instance, no vertex buffer: the instance index
// is a slot in the sorted list, the slot names a particle, and the particle's whole state is a
// closed-form function of its index and the clock. Nothing is uploaded per frame but the push
// block.
//
// Both draws use this shader. They differ only in blend state and in the `firstInstance` they are
// given, which is what selects the alpha half of the sorted buffer or the additive half.

#define SCENE_SET 0
#define PARTICLE_SET 1
#include "scene.glsl"
#include "particle_common.glsl"

layout(location = 0) out vec2  vUv;
layout(location = 1) out vec4  vTint;  // rgb linear, w = 1 when the particle is its own emitter
layout(location = 2) out float vAlpha;
layout(location = 3) out vec3  vWorldPos;

// Two triangles, corners in [-1, 1]. Written out rather than derived from bit tricks because six
// vec2s are cheaper to read than the arithmetic that would reproduce them.
const vec2 kCorner[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
                                vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));

void main() {
    vUv       = vec2(0.0);
    vTint     = vec4(0.0);
    vAlpha    = 0.0;
    vWorldPos = vec3(0.0);

    uint slot = sorted[uint(gl_InstanceIndex)];

    Particle p;
    if (slot == kParticleNone || !ParticleAt(slot, pp.timing.x, p)) {
        // Behind the far plane, so the clipper throws the whole triangle away before it rasterises.
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        return;
    }

    vec3  toCamera = scene.cameraPos.xyz - p.pos;
    float distance = length(toCamera);
    vec3  view     = toCamera / max(distance, 1e-3);

    // Camera-facing, with world up as the reference. The degenerate case is the camera looking
    // straight down the Y axis, which spec 11.1's orbit never does.
    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), view) + vec3(1e-5, 0.0, 0.0));
    vec3 up    = cross(view, right);

    vec2 corner = kCorner[gl_VertexIndex];
    vec3 world  = p.pos + (right * corner.x + up * corner.y) * p.size;

    gl_Position = scene.viewProj * vec4(world, 1.0);
    vUv         = corner;
    vTint       = vec4(p.tint, p.additive ? 1.0 : 0.0);
    vAlpha      = p.alpha;
    vWorldPos   = world;
}
