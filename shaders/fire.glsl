// The fireball as a light source (spec 8.2).
//
// "From phase 5 the fireball MUST be the dominant light." Every surface pass in the project needs
// the same answer to that, so it is written once here rather than four times with four sets of
// constants that would drift apart the first time one of them was tuned.
//
// It is a sphere light, not a point light: at the start of phase 6 the fireball is a couple of
// hundred metres across and the buildings nearest it are inside that radius. A point light with
// inverse-square falloff goes to infinity there, and what that looks like is a handful of
// buildings at the centre blowing out to pure white while their neighbours are lit normally. The
// falloff below flattens inside the radius, which is what a real emitter of that size does.

#ifndef NUKE_SAVER_FIRE_GLSL
#define NUKE_SAVER_FIRE_GLSL

#include "scene.glsl"

// How much of the fireball's authored emissive becomes irradiance on the scene.
//
// Well below one, and it has to be. Spec 5.1 fixes the emissive magnitude — 2,000 to 4,000 at the
// start of phase 6 — but that number is authored for what the fireball's own surface looks like
// through the tonemap, not for what it does to the desert. Used directly as radiance it lit the
// whole eight-kilometre basin to the top of the curve, out to the mountains, with no falloff
// visible anywhere in the frame: the ground went uniformly white-yellow and the scorched footprint
// underneath it stopped existing.
const float kFireLightScale = 0.10;

// Irradiance from the fireball at `worldPos` on a surface facing `normal`, in linear scene units,
// ready to multiply by albedo. Zero when there is no fireball, so callers need no phase test.
vec3 FireIrradiance(vec3 worldPos, vec3 normal) {
    float radius = scene.fireLight.w;
    if (radius <= 0.0) return vec3(0.0);

    vec3  toFire   = scene.fireLight.xyz - worldPos;
    float distance = length(toFire);
    vec3  lightDir = toFire / max(distance, 1e-3);

    // Inverse square outside the sphere, constant inside it. The 1 keeps the whole thing finite
    // at the centre without changing anything at the distances that matter.
    float d        = max(distance, radius);
    float falloff  = (radius * radius) / (d * d);

    // Half-Lambert rather than a hard terminator. The fireball fills a large solid angle from
    // anywhere in the city, so a surface angled away from it is still lit by the part of it that
    // is above that surface's horizon, and a clamped dot product makes those faces black.
    float wrap = 0.5 + 0.5 * dot(normal, lightDir);

    return scene.fireColor.rgb * (kFireLightScale * falloff * wrap * wrap);
}

// The colour a surface picks up from the fire, tinted toward the fire's own hue. Spec 7.5 and 7.7
// ask for fragments near the stem base to take the fireball's colour and for falling debris to
// carry its remaining light; this is the term that does both, and using it everywhere is what
// keeps the whole scene reading as lit by one source rather than as a set of objects each with
// its own idea of the light.
vec3 FireContribution(vec3 albedo, vec3 worldPos, vec3 normal) {
    return albedo * FireIrradiance(worldPos, normal);
}

#endif
