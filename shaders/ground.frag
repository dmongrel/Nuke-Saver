#version 450

// Desert floor and horizon range (spec 5.3, 6.2, 6.3).
//
// Everything here is linear scene-referred, including the incoming albedo: the palette of spec 5.3
// is authored in display-referred HSV and decoded on the CPU, in core/color.h, once. There is
// deliberately no copy of it here — see world::Vertex for why the shader is the wrong place to
// derive per-instance colour variation from an interpolated attribute.

#include "atmosphere.glsl"
#include "fire.glsl"
#include "noise.glsl"
#include "shadow.glsl"

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in vec3  vAlbedo;
layout(location = 3) in float vRockiness;

layout(location = 0) out vec4 outColor;

void main() {
    vec3  toCamera = scene.cameraPos.xyz - vWorldPos;
    float distance = length(toCamera);
    vec3  viewDir  = toCamera / max(distance, 1e-4);

    vec3 albedo = vAlbedo;

    // The scorched footprint and the collar of stripped dust (spec 7.2).
    //
    // Both are surface terms on the desert floor rather than geometry, because both are exactly
    // that: ground that the blast has changed. The dust *cloud* the collar throws up is a particle
    // system and arrives with M6; what is here is the mark the shell leaves as it goes over.
    float shell = scene.blast.w;
    float ring  = 0.0;
    if (shell > 0.0) {
        float d = length(vWorldPos.xz - scene.blast.xz);

        // Scorch: everything the shell has already crossed, worst at the centre. It does not fade
        // — spec 7.7 wants a debris field over a scorched footprint at the end of the cycle.
        float burned = 1.0 - smoothstep(0.0, shell, d);
        albedo = mix(albedo, albedo * vec3(0.16, 0.13, 0.12), burned * 0.88 * (1.0 - vRockiness));

        // The collar, slightly ahead of the shell as spec 7.2 requires, and narrow: a wide one
        // reads as a second sunset on the sand rather than as a front moving over it.
        float lead  = shell * 1.06;
        float width = max(shell * 0.055, 12.0);
        ring = exp(-((d - lead) * (d - lead)) / (width * width)) * (1.0 - vRockiness);
    }

    // Dunes are normal-map detail, not geometry (spec 6.2). Two scales of gradient noise,
    // differentiated into a slope, and applied only to sand — a mountain face is rock, and
    // rippling it would read as fur.
    vec3 normal = normalize(vNormal);
    if (vRockiness < 0.9) {
        // Faded out with distance, because at 4 km the ripples are far below a pixel and all they
        // can do is alias.
        float detail = (1.0 - vRockiness) * exp(-distance / 1300.0);
        if (detail > 0.002) {
            vec2  p = vWorldPos.xz;
            float e = 1.5;
            float h  = Fbm2(p * 0.012, 3) * 0.75 + Fbm2(p * 0.08, 2) * 0.05;
            float hx = Fbm2((p + vec2(e, 0.0)) * 0.012, 3) * 0.75 +
                       Fbm2((p + vec2(e, 0.0)) * 0.08, 2) * 0.05;
            float hz = Fbm2((p + vec2(0.0, e)) * 0.012, 3) * 0.75 +
                       Fbm2((p + vec2(0.0, e)) * 0.08, 2) * 0.05;

            // Gently. At 14 this read as corduroy rather than sand, and at 4.5 it still read as
            // fur wherever the ground ran away from the camera: at a grazing angle a metre of
            // ground covers a fraction of a pixel, so any texture with a slope in it turns into
            // streaks along the view direction. The fade above is the real control — past about a
            // kilometre there is nothing here worth drawing.
            normal = normalize(normal + vec3(-(hx - h), 0.0, -(hz - h)) * detail * 2.6);
        }
    }

    vec3  keyDir   = scene.keyDirection.xyz;
    float lambert  = max(dot(normal, keyDir), 0.0);

    // The key fades out as the sun approaches and passes the horizon, so twilight loses its
    // direct light gradually instead of switching off at exactly zero elevation.
    float keyAbove = smoothstep(-0.08, 0.06, keyDir.y);

    vec3 lit = albedo * scene.keyColor.rgb * lambert * keyAbove * KeyShadow(vWorldPos, normal);

    // The sky this surface actually sits under, not a single authored constant. At twilight the
    // ambient colour of spec 5.4 is the violet overhead, which ignores the orange band filling
    // half the hemisphere and left the desert floor as a void with a lit city floating on it.
    // Taking the greater of the two keeps night — where the authored value is deliberately above
    // the almost-black gradient — from going darker still.
    //
    // Weighted toward the zenith because irradiance on a level surface weights by the cosine, and
    // the horizon band arrives at a grazing angle. A twilight foreground is meant to be dark; it
    // is not meant to be nothing.
    vec3 skyAmbient = max(scene.ambientColor.rgb,
                          mix(scene.horizonColor.rgb, scene.zenithColor.rgb, 0.7));

    // Weighted upward, because the sky is above and the ground is not.
    float skyFacing = 0.5 + 0.5 * normal.y;
    lit += albedo * skyAmbient * scene.groundColor.w * skyFacing;


    // The board as a light source (spec 7.6). A point light at the glyph band, amber, falling off
    // over its own radius — the rooftops beneath it must be visibly lit by it at night, and it
    // must not be absent at noon.
    vec3  toBoard   = scene.boardLight.xyz - vWorldPos;
    float boardDist = length(toBoard);
    float falloff   = scene.boardLight.w /
                      (1.0 + (boardDist * boardDist) /
                                 max(scene.boardColor.w * scene.boardColor.w, 1.0));
    lit += albedo * scene.boardColor.rgb *
              max(dot(normal, toBoard / max(boardDist, 1e-3)), 0.0) * falloff;

    // The fireball (spec 8.2). From phase 5 this is the dominant term, by a long way: the
    // scorched floor around the impact point is lit by nothing else.
    lit += FireContribution(albedo, vWorldPos, normal);

    // The collar is dust in the air just above the ground, so it is lit by the fireball and
    // scatters most of it back. Added rather than mixed: it is material between the eye and the
    // sand, not a different sand.
    if (ring > 0.0) {
        vec3 dust = vAlbedo * 1.6 + vec3(0.04);
        lit += ring * (dust * (scene.keyColor.rgb * 0.35 + skyAmbient * scene.groundColor.w) +
                       FireIrradiance(vWorldPos, vec3(0.0, 1.0, 0.0)) * dust);
    }

    // Haze, into exactly the sky that is behind this surface (spec 6.3). Using the view ray's own
    // gradient value rather than a single fog colour is what lets the range dissolve into the sky
    // instead of into a grey band that does not match it.
    vec3  haze   = SkyGradient(-viewDir);
    float amount = HazeAmount(distance);

    outColor = vec4(mix(lit, haze, amount), 1.0);
}
