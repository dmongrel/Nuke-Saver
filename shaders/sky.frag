#version 450

// The sky (spec 6.3): a gradient from the time-of-day table, stars fixed to the sky, and one
// celestial body drawn at the key-light direction.
//
// Analytic rather than a cubemap. A cubemap would need generating, storing and filtering, and
// spec 6.1 forbids loading one; evaluating the gradient per pixel costs less than the memory
// traffic of sampling a face, and the body then lands exactly where the light comes from by
// construction rather than by a baking step that could go stale.

#include "scene.glsl"

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

float Hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// Stars are placed in a cell grid over the direction sphere, so they are fixed to the world and
// the orbit moves past them rather than carrying them along (spec 6.3).
//
// One cell is sampled, not the 27-cell neighbourhood, which would otherwise be needed to avoid
// clipping a star that straddles a boundary. Instead the star is confined to the middle of its
// cell, so it can never reach an edge. That costs a little regularity in the placement, which at
// this density is not visible, and saves twenty-six hashes per pixel of sky.
float Stars(vec3 dir, float brightness) {
    if (brightness <= 0.0) return 0.0;

    const float kCellsPerUnit = 190.0;
    vec3  p    = dir * kCellsPerUnit;
    vec3  cell = floor(p);
    vec3  f    = p - cell;

    // Only a small fraction of cells host a star; the rest of the sky stays empty.
    if (Hash13(cell) < 0.982) return 0.0;

    // Confined to [0.25, 0.75] so the disc never crosses a cell boundary.
    vec3 starPos = 0.25 + 0.5 * vec3(Hash13(cell + 1.7), Hash13(cell + 3.1), Hash13(cell + 5.3));

    float d         = length(f - starPos);
    float magnitude = Hash13(cell + 11.3);

    // Brighter stars are drawn slightly larger, which is how the eye reads magnitude.
    float radius = mix(0.055, 0.115, magnitude);
    float disc   = smoothstep(radius, 0.0, d);

    return disc * mix(0.25, 1.0, magnitude) * brightness;
}

void main() {
    vec3 dir = ViewRay(vNdc);

    float starBrightness = scene.keyColor.w;
    float isMoon         = scene.zenithColor.w;
    vec3  keyDir         = scene.keyDirection.xyz;
    float bodyRadius     = scene.keyDirection.w;

    // Elevation above the horizon, 0 at the horizon and 1 at the zenith.
    float elevation = clamp(dir.y, 0.0, 1.0);

    // A tighter curve near the horizon: the interesting part of every one of the four settings
    // is the band just above it, and a linear blend spends most of the screen on the zenith.
    float t = pow(elevation, 0.42);
    vec3  sky = mix(scene.horizonColor.rgb, scene.zenithColor.rgb, t);

    // The horizon glow sits where the light is. Derived from the same key direction as
    // everything else, so it cannot drift away from the body drawn on top of it.
    float towardsKey = max(dot(normalize(vec3(dir.x, 0.0, dir.z)),
                               normalize(vec3(keyDir.x, 0.0, keyDir.z))), 0.0);
    float glowBand   = exp(-elevation * 7.0);
    sky += scene.horizonColor.rgb * pow(towardsKey, 4.0) * glowBand * 1.6;

    sky += Stars(dir, starBrightness);

    // The celestial body. cos(angle) against the key direction, with a soft limb so it does not
    // alias into a ring of stair-steps at these radii.
    float cosAngle = dot(dir, keyDir);
    float cosEdge  = cos(bodyRadius);
    float limb     = smoothstep(cos(bodyRadius * 1.06), cosEdge, cosAngle);

    vec3 body = scene.bodyColor.rgb * limb;

    // The moon may carry simple surface variation (spec 6.3). Two octaves of cheap value noise
    // across the disc is enough to stop it reading as a flat white circle; its phase is fixed and
    // deliberately not modelled.
    if (isMoon > 0.5 && limb > 0.0) {
        vec3  surface = dir * 60.0;
        float mottle  = 0.78 + 0.22 * (Hash13(floor(surface)) * 0.65 +
                                       Hash13(floor(surface * 2.3)) * 0.35);
        body *= mottle;
    }

    // A wide halo around the body. Real atmosphere does this, and without it the body reads as a
    // sticker on the gradient rather than as something behind air.
    float halo = pow(max(cosAngle, 0.0), 220.0);
    body += scene.bodyColor.rgb * halo * 0.012;

    sky += body;

    // Below the horizon the earth is in the way. Fading to the ground colour, and taking the body
    // with it, keeps a twilight sun that has dipped below the horizon from glaring through the
    // ground it is supposed to be behind.
    //
    // The edge is centred exactly on dir.y = 0 and is one pixel wide, which matters more than it
    // looks. dir is normalised, so a threshold at any *other* value of dir.y describes a cone
    // about the vertical axis rather than a plane, and a cone projects to a curve: a fade from 0
    // to -0.045 put the visible edge at -0.0225 and bowed the horizon by 33 rows across a 3440-
    // wide frame. Only dir.y = 0 itself is planar. fwidth gives the antialiasing without moving
    // the edge off it.
    float fade  = max(fwidth(dir.y), 1e-5);
    float below = smoothstep(-fade, fade, -dir.y);
    sky = mix(sky, scene.groundColor.rgb, below);

    outColor = vec4(sky, 1.0);
}
