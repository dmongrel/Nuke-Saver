#version 450

// Particle billboards (spec 8.3). Six vertices an instance, no vertex buffer: the instance index
// is a slot in the sorted list, the slot names a particle, and the particle's whole state is a
// closed-form function of its index and the clock. Nothing is uploaded per frame but the push
// block.
//
// Both draws use this shader. They differ only in blend state and in the `firstInstance` they are
// given, which is what selects the alpha half of the sorted buffer or the additive half.
//
// It also lights the dust (see DustColor), so the fragment stage has only the falloff left to do.

#define SCENE_SET 0
#define PARTICLE_SET 1
#define PARTICLE_READONLY
#include "scene.glsl"
#include "atmosphere.glsl"
#include "fire.glsl"
#include "shadow.glsl"
#include "particle_common.glsl"

layout(location = 0) out vec2  vUv;
layout(location = 1) out vec3  vColor;  // linear radiance: an emitter's own, or the dust's as lit
layout(location = 2) out float vAlpha;

// A hexagon around the unit disc, as a six-vertex triangle strip (the pipeline's topology). The
// fragment stage discards everything outside the disc, and a hexagon wastes 13% less area around
// it than a square does for the same six vertices. R = 2/sqrt(3) puts the flat sides at 1.
const float kHexR     = 1.1547005;
const vec2  kCorner[6] = vec2[6](vec2(-kHexR, 0.0), vec2(-0.5 * kHexR, -1.0),
                                 vec2(-0.5 * kHexR, 1.0), vec2(0.5 * kHexR, -1.0),
                                 vec2(0.5 * kHexR, 1.0), vec2(kHexR, 0.0));

// The dust's colour at one point of a sprite: key, sky ambient, the board, the fireball, then
// haze -- the same set of terms the desert floor uses, because a dust cloud lit by a different rig
// than the ground it is sitting on is the fastest way to make a particle system look pasted on.
//
// Evaluated here, at the corners, and interpolated across the sprite, rather than per pixel. Dust
// is soft blobs tens of metres across at a peak alpha of about 0.1, and nothing these terms depend
// on changes enough across one to see; per pixel it cost a nine-tap shadow lookup and the whole
// lighting rig for every fragment of the largest fragment workload in the scene. The one place the
// two differ is a shadow edge crossing a single puff, which is now a gradient across it.
vec3 DustColor(vec3 tint, vec3 worldPos) {
    vec3  toCamera = scene.cameraPos.xyz - worldPos;
    float distance = length(toCamera);
    vec3  viewDir  = toCamera / max(distance, 1e-4);

    // Dust. The billboard has no real normal, so it is shaded as a facing disc: half-Lambert
    // against the key, and the sky ambient weighted as if the puff were a small sphere, which
    // is closer to what a cloud of it actually does than a flat card is.
    vec3  normal   = viewDir;
    float keyAbove = HighAirKeyAbove(scene.keyDirection.xyz, worldPos.y);
    float wrap     = 0.5 + 0.5 * dot(normal, scene.keyDirection.xyz);

    // The missile's contrail hangs high enough that the key light still reaches it after the
    // ground has lost it. Same term the airframe uses, and the reason a contrail overhead at
    // dusk is white against a sky that has already gone blue. Flat at ground level, so the
    // four systems that live in the first hundred metres are untouched by it.
    //
    // Taken at a quarter strength. A contrail is dozens of overlapping sprites deep, so its
    // composite is the colour of one particle however faint each is made -- turning the alpha
    // down does not dim a solid trail, it only softens its edges. That colour has to be close
    // to the sky it hangs in; at full strength it was the key light, which at twilight is a
    // saturated orange, and the trail came out as a bar of it ruled across the sunset.
    float high = mix(1.0, HighAirLight(worldPos.y), 0.25);

    // Weighted toward the horizon rather than the zenith, which is the opposite of what the
    // ground floor does and for the same reason: the ground is a level surface that sees the
    // sky by the cosine, and a puff of dust is not a surface at all. It sees the whole
    // hemisphere, and at twilight most of that hemisphere's light is in the horizon band.
    vec3 skyAmbient = max(scene.ambientColor.rgb,
                          mix(scene.horizonColor.rgb, scene.zenithColor.rgb, 0.35));

    // Dust receives as well as neighbours the casters (spec 8.2). The settled disc under
    // the cap sits directly in the cloud shadow, and left lit it was the brightest thing on
    // the desert at the moment the sky above it went dark. Only the two key terms are
    // shadowed: sky ambient reaches a shaded puff, and so does the fireball, which is inside
    // the cloud rather than behind it.
    float keyShadow = KeyShadow(worldPos, normal);

    vec3 lit = tint * scene.keyColor.rgb * wrap * wrap * keyAbove * high * keyShadow;

    // Above one on purpose. Dust is lit from every direction at once, including from the
    // ground it is hanging over, and at the cosine-weighted figure the ground uses it came out
    // the same colour as the ground — which for a screen saver means it is not there at all.
    lit += tint * skyAmbient * (scene.groundColor.w * 1.9);

    // Forward scattering. A cloud between the eye and a low sun is brighter than the same
    // cloud lit from behind the eye, and at twilight — the default time of day — the sun is
    // always low. This is the term that makes the missile's trail read as smoke rather than as
    // a smear of dark paint on the sky.
    float forward = max(-dot(viewDir, scene.keyDirection.xyz), 0.0);
    lit += tint * scene.keyColor.rgb *
           (forward * forward * forward * 1.4 * keyAbove * keyShadow);

    vec3  toBoard   = scene.boardLight.xyz - worldPos;
    float boardDist = length(toBoard);
    lit += tint * scene.boardColor.rgb * scene.boardLight.w /
           (1.0 + (boardDist * boardDist) /
                      max(scene.boardColor.w * scene.boardColor.w, 1.0));

    // The one term that matters most: from phase 5 the fireball is the dominant light (spec
    // 8.2), and the collar dust of spec 7.7 exists precisely to have something in the air for
    // it to light.
    lit += FireContribution(tint, worldPos, normal);

    return mix(lit, SkyGradient(-viewDir), HazeAmount(distance));
}

void main() {
    vUv    = vec2(0.0);
    vColor = vec3(0.0);
    vAlpha = 0.0;

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
    vUv    = corner;
    vAlpha = p.alpha;

    // An emitter -- exhaust or an ember -- is its own radiance, and takes no haze: the haze of
    // spec 6.3 mixes toward the sky, and mixing an additive sprite toward the sky only makes it
    // brighter.
    vColor = p.additive ? p.tint : DustColor(p.tint, world);
}
