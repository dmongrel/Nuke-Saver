#version 450

// Board surfaces (spec 7.6).
//
// Three materials on one mesh: the dark recessed face, an unlit bar, and a lit bar. The unlit bar
// must stay visible — a 7-segment display whose dark segments vanish is a defect, and it is the
// difference between a numeral and a shape.

#include "atmosphere.glsl"
#include "fire.glsl"
#include "shadow.glsl"

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in float vLit;
layout(location = 3) in float vIsSegment;

layout(push_constant) uniform Push {
    uvec2 litMask;
    float rise;
    float emissive;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec3  toCamera = scene.cameraPos.xyz - vWorldPos;
    float distance = length(toCamera);
    vec3  viewDir  = toCamera / max(distance, 1e-4);

    vec3 normal = normalize(vNormal);

    // Spec 5.3: the frame is a near-black grey, the bar a darker one. Both are deliberately far
    // below the buildings around them, so the board reads as an object the light falls on rather
    // than as a panel that glows.
    vec3 face = vec3(0.045, 0.043, 0.042);
    vec3 bar  = vec3(0.021, 0.020, 0.020);
    vec3 albedo = mix(face, bar, vIsSegment);

    vec3  keyDir   = scene.keyDirection.xyz;
    float lambert  = max(dot(normal, keyDir), 0.0);
    float keyAbove = smoothstep(-0.08, 0.06, keyDir.y);

    vec3 shaded = albedo * scene.keyColor.rgb * lambert * keyAbove *
                  KeyShadow(vWorldPos, normal);

    vec3 skyAmbient = max(scene.ambientColor.rgb,
                          mix(scene.horizonColor.rgb, scene.zenithColor.rgb, 0.7));
    shaded += albedo * skyAmbient * scene.groundColor.w * (0.5 + 0.5 * normal.y);

    shaded += FireContribution(albedo, vWorldPos, normal);

    // A lit segment. Amber, and emissive far above 1 so the bloom of M5 finds it (spec 5.1).
    // Divided by the base exposure for the same reason the windows are: the lamp does not change
    // between noon and midnight, only what surrounds it does.
    vec3 lamp = vec3(1.0, 0.62, 0.20);

    shaded += lamp * vLit * (pc.emissive / max(scene.horizonColor.w, 1e-3));

    vec3 haze = SkyGradient(-viewDir);
    outColor  = vec4(mix(shaded, haze, HazeAmount(distance)), 1.0);
}
