#version 450

// Desert floor and horizon range (spec 5.3, 6.2, 6.3).
//
// Everything here is linear scene-referred, including the incoming albedo: the palette of spec 5.3
// is authored in display-referred HSV and decoded on the CPU, in core/color.h, once. There is
// deliberately no copy of it here — see world::Vertex for why the shader is the wrong place to
// derive per-instance colour variation from an interpolated attribute.

#include "atmosphere.glsl"
#include "noise.glsl"

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

    vec3 lit = albedo * scene.keyColor.rgb * lambert * keyAbove;

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

    // Haze, into exactly the sky that is behind this surface (spec 6.3). Using the view ray's own
    // gradient value rather than a single fog colour is what lets the range dissolve into the sky
    // instead of into a grey band that does not match it.
    vec3  haze   = SkyGradient(-viewDir);
    float amount = HazeAmount(distance);

    outColor = vec4(mix(lit, haze, amount), 1.0);
}
