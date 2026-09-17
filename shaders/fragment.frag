#version 450

// Fragments are double-sided and dark on the back (spec 7.3): a tumbling cloud has to flicker with
// contrast rather than go flat, and that contrast is the difference between a chip showing its lit
// face and one showing its shadowed one. With no shadow map in the scene this is the only thing
// giving the cloud internal form.

#include "atmosphere.glsl"
#include "fire.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec3  toCamera = scene.cameraPos.xyz - vWorldPos;
    float distance = length(toCamera);
    vec3  viewDir  = toCamera / max(distance, 1e-4);

    // Which side of the chip is towards the camera, decided from the geometry rather than from
    // gl_FrontFacing: with culling off, the winding the rasteriser sees depends on the viewport's
    // handedness, and this does not.
    vec3 normal = normalize(vNormal);
    bool back   = dot(normal, viewDir) < 0.0;
    if (back) normal = -normal;

    vec3  keyDir   = scene.keyDirection.xyz;
    float lambert  = max(dot(normal, keyDir), 0.0);
    float keyAbove = smoothstep(-0.08, 0.06, keyDir.y);

    vec3 shaded = vColor * scene.keyColor.rgb * lambert * keyAbove;

    vec3 skyAmbient = max(scene.ambientColor.rgb,
                          mix(scene.horizonColor.rgb, scene.zenithColor.rgb, 0.7));
    shaded += vColor * skyAmbient * scene.groundColor.w * (0.45 + 0.55 * normal.y);

    // The fireball (spec 7.5, 7.7): fragments near the stem base take the fire's colour, and
    // falling debris carries its remaining light. Both fall out of the one distance-weighted term
    // rather than being written as two special cases of where a fragment happens to be.
    shaded += FireContribution(vColor, vWorldPos, normal);

    // A back face keeps only a share of the ambient. Not black — a black chip against a bright sky
    // reads as a hole — but far enough down that the cloud's surface has grain in it.
    if (back) shaded *= 0.45;

    vec3 haze = SkyGradient(-viewDir);
    outColor  = vec4(mix(shaded, haze, HazeAmount(distance)), 1.0);
}
