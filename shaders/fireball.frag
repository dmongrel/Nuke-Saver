#version 450

// The fireball's surface (spec 7.2, 5.1).
//
// Everything here is emission. The fireball is not lit by anything — it is what lights everything
// else, and that half of it lives in fire.glsl where the surface passes read it.

#include "noise.glsl"
#include "scene.glsl"

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vDirection;
layout(location = 2) in float vDisplace;

layout(location = 0) out vec4 outColor;

void main() {
    vec3  toCamera = scene.cameraPos.xyz - vWorldPos;
    vec3  viewDir  = toCamera / max(length(toCamera), 1e-4);

    // Only the near half. Decided from the geometry rather than from culling, because the winding
    // a lat-long grid presents to the rasteriser depends on the viewport's handedness and getting
    // it backwards here would make the fireball vanish rather than look wrong.
    float facing = dot(vDirection, viewDir);
    if (facing <= 0.0) discard;

    float t = scene.cameraPos.w;

    // Mottling, at a finer scale than the displacement and moving at a different rate, so the
    // surface boils rather than sliding around as one piece. Two octaves: a third added an eighth
    // of zero-mean amplitude at thirty cells per unit, under 0.04 of heat, and cost a third of the noise
    // on every fireball pixel for detail the vertex displacement and the bloom both smear away.
    float fine = Fbm3(vDirection * 7.0 + vec3(0.0, -t * 0.9, t * 0.4), 2);

    // The hollows are hotter than the ridges. That is the wrong way round for a solid, and the
    // right way round for this: the bright gas is inside and the crests are the cooling skin
    // being thrown off it.
    float heat = 1.0 - 0.45 * vDisplace + 0.30 * fine;

    // The limb of an optically thick ball of fire is cooler than its centre — the line of sight
    // there grazes the surface and never reaches the hot core. That is the opposite of the rim
    // boost that most emissive spheres get, and it is what stops this reading as a bubble.
    float limb = smoothstep(0.0, 0.55, facing);

    vec3 emissive = scene.fireColor.rgb * max(heat, 0.0) * (0.35 + 0.65 * limb);

    // A soft silhouette. The alpha is the blend's coverage term, so the last few degrees of the
    // limb fade in rather than cutting against the sky.
    outColor = vec4(emissive, smoothstep(0.0, 0.10, facing));
}
