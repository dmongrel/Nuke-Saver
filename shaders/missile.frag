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

    // The missile enters a couple of kilometres up (spec 7.1), where the key light still reaches
    // it after it has left the ground. That is what makes the airframe read as a bright object
    // with a shape rather than as the dim grey cylinder it was, and it is the reason the exhaust
    // no longer has to carry the visibility on its own.
    float high     = HighAirLight(vWorldPos.y);
    vec3  keyDir   = scene.keyDirection.xyz;
    float lambert  = max(dot(normal, keyDir), 0.0);
    float keyAbove = HighAirKeyAbove(keyDir, vWorldPos.y);

    vec3 shaded = vAlbedo * scene.keyColor.rgb * lambert * keyAbove * high;

    vec3 skyAmbient = max(scene.ambientColor.rgb,
                          mix(scene.horizonColor.rgb, scene.zenithColor.rgb, 0.7));
    shaded += vAlbedo * skyAmbient * scene.groundColor.w * (0.5 + 0.5 * normal.y) * high;

    // A hard sheen along the body. A painted metal cylinder with nothing but a Lambert term on it
    // reads as matte plastic at any brightness, and the highlight running down the flank is what
    // carries the missile's shape at the distance it is now seen from.
    vec3  halfway = normalize(keyDir + viewDir);
    float sheen   = pow(max(dot(normal, halfway), 0.0), 48.0);
    shaded += scene.keyColor.rgb * sheen * keyAbove * high * 0.55;

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

        // Falls off with distance as well as along the plume. Bloom turns a small bright thing
        // far away into a round bead, and past a few kilometres the plume is exactly that: a
        // sub-pixel emitter with a halo, which is the failure mode the airframe lighting above
        // exists to replace. Near the city it is a rocket motor again.
        float near = mix(0.35, 1.0, 1.0 - smoothstep(1200.0, 4200.0, distance));

        shaded += hot * (vEmissive * pc.exhaust.x * flicker * near /
                         max(scene.horizonColor.w, 1e-3));
    }

    vec3 haze = SkyGradient(-viewDir);
    outColor  = vec4(mix(shaded, haze, HazeAmount(distance)), 1.0);
}
