#version 450

// The missile's airframe and its exhaust (spec 7.1, 5.1, 5.3).

#include "atmosphere.glsl"
#include "fire.glsl"
#include "noise.glsl"

layout(push_constant) uniform MissilePush {
    mat4 model;
    vec4 exhaust;  // x = plume emissive magnitude
} pc;

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in vec3  vAlbedo;
layout(location = 3) in float vEmissive;

layout(location = 0) out vec4 outColor;

void main() {
    vec3  toCamera = scene.cameraPos.xyz - vWorldPos;
    float distance = length(toCamera);
    vec3  viewDir  = toCamera / max(distance, 1e-4);

    vec3 normal = normalize(vNormal);

    vec3  keyDir   = scene.keyDirection.xyz;
    float lambert  = max(dot(normal, keyDir), 0.0);
    float keyAbove = smoothstep(-0.08, 0.06, keyDir.y);

    vec3 shaded = vAlbedo * scene.keyColor.rgb * lambert * keyAbove;

    vec3 skyAmbient = max(scene.ambientColor.rgb,
                          mix(scene.horizonColor.rgb, scene.zenithColor.rgb, 0.7));
    shaded += vAlbedo * skyAmbient * scene.groundColor.w * (0.5 + 0.5 * normal.y);

    // The board is lit while the missile is in the air, and it is the brightest thing in the scene
    // at that moment, so it reaches the airframe like it reaches the rooftops.
    vec3  toBoard   = scene.boardLight.xyz - vWorldPos;
    float boardDist = length(toBoard);
    float falloff   = scene.boardLight.w /
                      (1.0 + (boardDist * boardDist) /
                                 max(scene.boardColor.w * scene.boardColor.w, 1.0));
    shaded += vAlbedo * scene.boardColor.rgb *
              max(dot(normal, toBoard / max(boardDist, 1e-3)), 0.0) * falloff;

    shaded += FireContribution(vAlbedo, vWorldPos, normal);

    if (vEmissive > 0.0) {
        // The plume flickers. A steady cone reads as a plastic fin, and the flicker is what tells
        // the eye this is combustion rather than a painted taper. Driven by the clock, so it is
        // frame-rate independent as spec 4.2 requires.
        float t       = scene.cameraPos.w;
        float flicker = 0.82 + 0.18 * Fbm3(vec3(vWorldPos.xz * 0.6, t * 22.0), 2);

        // The nozzle is white-hot and the tail is orange, so the ramp carries a hue as well as a
        // magnitude. Divided by the base exposure for the same reason the windows and the board
        // segments are: a rocket motor does not get brighter after dark.
        vec3 hot = mix(vec3(1.0, 0.42, 0.10), vec3(1.0, 0.94, 0.78), vEmissive * vEmissive);

        shaded += hot * (vEmissive * pc.exhaust.x * flicker /
                         max(scene.horizonColor.w, 1e-3));
    }

    vec3 haze = SkyGradient(-viewDir);
    outColor  = vec4(mix(shaded, haze, HazeAmount(distance)), 1.0);
}
