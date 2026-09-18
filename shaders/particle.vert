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
#define PARTICLE_READONLY
#include "scene.glsl"
#include "particle_common.glsl"

layout(location = 0) out vec2  vUv;
layout(location = 1) out vec4  vTint;  // rgb linear, w = 1 when the particle is its own emitter
layout(location = 2) out float vAlpha;
layout(location = 3) out vec3  vWorldPos;

// A hexagon around the unit disc, as a six-vertex triangle strip (the pipeline's topology). The
// fragment stage discards everything outside the disc, and a hexagon wastes 13% less area around
// it than a square does for the same six vertices. R = 2/sqrt(3) puts the flat sides at 1.
const float kHexR     = 1.1547005;
const vec2  kCorner[6] = vec2[6](vec2(-kHexR, 0.0), vec2(-0.5 * kHexR, -1.0),
                                 vec2(-0.5 * kHexR, 1.0), vec2(0.5 * kHexR, -1.0),
                                 vec2(0.5 * kHexR, 1.0), vec2(kHexR, 0.0));

void main() {
    vUv       = vec2(0.0);
    vTint     = vec4(0.0);
    vAlpha    = 0.0;
    vWorldPos = vec3(0.0);

    uint slot = sorted[uint(gl_InstanceIndex)];

    Particle p;
    // A particle at or under the fragment stage's alpha threshold draws nothing, so it is culled
    // here with the dead ones. Not in the sort passes: taking it out of the buckets changes the
    // order the atomics hand out slots inside a bucket, and with dust lit by the fireball that
    // reorders blends visibly.
    if (slot == kParticleNone || !ParticleAt(slot, pp.timing.x, p) || p.alpha <= 0.002) {
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

    // Only as large as what the fragment stage keeps. It discards alpha <= 0.002, and alpha is
    // p.alpha * (1 - r²)², so nothing survives past r² = 1 - sqrt(0.002 / p.alpha); a faint
    // particle is a small disc, not a full-size one that is mostly discarded. p.alpha is over the
    // threshold here, so the root is of a positive number. The 1% margin keeps rounding from
    // clipping a surviving pixel at the edge. vUv stays in unit-disc units, so the fragment stage
    // sees the same values it always did.
    float reach  = min(sqrt(1.0 - sqrt(0.002 / p.alpha)) * 1.01, 1.0);
    vec2  corner = kCorner[gl_VertexIndex] * reach;
    vec3  world  = p.pos + (right * corner.x + up * corner.y) * p.size;

    gl_Position = scene.viewProj * vec4(world, 1.0);
    vUv         = corner;
    vTint       = vec4(p.tint, p.additive ? 1.0 : 0.0);
    vAlpha      = p.alpha;
    vWorldPos   = world;
}
