// The sky, and the haze that everything distant fades into.
//
// Both live here for one reason: spec 6.3 requires the horizon range to "fade into the horizon
// haze with distance so the range reads as atmospheric rather than as a painted backdrop". That
// only works if the haze a mountain fades into is *the same colour* as the sky immediately behind
// it. Two separate implementations would agree at first and drift the moment either was touched,
// and the symptom — a faint outline around the range — is the exact thing the requirement is
// trying to prevent.
//
// So SkyGradient is the single definition, and the sky pass and the ground pass both call it.

#ifndef NUKE_SAVER_ATMOSPHERE_GLSL
#define NUKE_SAVER_ATMOSPHERE_GLSL

#include "noise.glsl"
#include "scene.glsl"

// Gradient plus horizon glow, with no stars and no celestial body. This is what distance fades
// into, so it must be the smooth part only: a mountain fading into a star would be a defect, and
// a mountain fading into the sun would be worse.
vec3 SkyGradient(vec3 dir) {
    float elevation = clamp(dir.y, 0.0, 1.0);

    // A tighter curve near the horizon: the interesting part of all four times of day is the band
    // just above it, and a linear blend spends most of the screen on the zenith.
    vec3 sky = mix(scene.horizonColor.rgb, scene.zenithColor.rgb, pow(elevation, 0.42));

    // The glow sits where the light is, derived from the same key direction as everything else,
    // so it cannot drift away from the body drawn on top of it.
    vec3  keyDir     = scene.keyDirection.xyz;
    float towardsKey = max(dot(normalize(vec3(dir.x, 0.0, dir.z)),
                               normalize(vec3(keyDir.x, 0.0, keyDir.z))), 0.0);
    sky += scene.horizonColor.rgb * pow(towardsKey, 4.0) * exp(-elevation * 7.0) * 1.6;

    return sky;
}

// Stars are placed in a cell grid over the direction sphere, so they are fixed to the world and
// the orbit moves past them rather than carrying them along (spec 6.3).
//
// One cell is sampled, not the 27-cell neighbourhood that would otherwise be needed to avoid
// clipping a star straddling a boundary. Instead the star is confined to the middle of its cell,
// so it can never reach an edge. That costs a little regularity in the placement, invisible at
// this density, and saves twenty-six hashes per pixel of sky.
float Stars(vec3 dir, float brightness) {
    if (brightness <= 0.0) return 0.0;

    const float kCellsPerUnit = 190.0;
    vec3  p    = dir * kCellsPerUnit;
    vec3  cell = floor(p);
    vec3  f    = p - cell;

    if (Hash13(cell) < 0.982) return 0.0;

    vec3 starPos = 0.25 + 0.5 * vec3(Hash13(cell + 1.7), Hash13(cell + 3.1), Hash13(cell + 5.3));

    float d         = length(f - starPos);
    float magnitude = Hash13(cell + 11.3);

    // Brighter stars are drawn slightly larger, which is how the eye reads magnitude.
    float radius = mix(0.055, 0.115, magnitude);
    return smoothstep(radius, 0.0, d) * mix(0.25, 1.0, magnitude) * brightness;
}

// The one celestial body, drawn at the key-light direction because that is the only direction
// there is (spec 6.3).
vec3 CelestialBody(vec3 dir) {
    vec3  keyDir     = scene.keyDirection.xyz;
    float bodyRadius = scene.keyDirection.w;
    float isMoon     = scene.zenithColor.w;

    float cosAngle = dot(dir, keyDir);

    // Far from the body both terms below are already exactly zero: the limb ends within a few
    // hundredths of a radian of the key direction, and 0.6^220 = 2^-162 underflows to 0 in fp32.
    // Returning early gives the same answer without the two cosines and the pow on every sky
    // pixel. Holds while bodyRadius stays well under 0.8 rad (sky.cpp uses 0.022 to 0.034).
    if (cosAngle <= 0.6) return vec3(0.0);

    // A soft limb, or the disc aliases into a ring of stair-steps at these radii.
    float limb = smoothstep(cos(bodyRadius * 1.06), cos(bodyRadius), cosAngle);

    vec3 body = scene.bodyColor.rgb * limb;

    // The moon may carry simple surface variation (spec 6.3). Two octaves of cheap value noise is
    // enough to stop it reading as a flat white circle; its phase is fixed and not modelled.
    if (isMoon > 0.5 && limb > 0.0) {
        vec3  surface = dir * 60.0;
        float mottle =
            0.78 + 0.22 * (Hash13(floor(surface)) * 0.65 + Hash13(floor(surface * 2.3)) * 0.35);
        body *= mottle;
    }

    // A wide halo. Real atmosphere does this, and without it the body reads as a sticker on the
    // gradient rather than as something behind air.
    body += scene.bodyColor.rgb * pow(max(cosAngle, 0.0), 220.0) * 0.012;

    return body;
}

// How much of the distant haze has swallowed a surface at this distance. Exponential, because
// that is what an atmosphere of roughly constant density does, and because it never quite reaches
// 1 — so the range stays faintly present rather than dissolving into flat sky.
float HazeAmount(float distanceMetres) {
    const float kScale = 1.0 / 17000.0;
    return 1.0 - exp(-distanceMetres * kScale);
}

#endif

// How much more of the key light reaches something at this altitude than reaches the ground.
//
// The default time of day is twilight, and at twilight the sun is below the *ground's* horizon but
// still well above the horizon of anything a few kilometres up: that is the whole mechanism behind
// alpenglow and behind why a contrail overhead is white while the desert under it has gone blue.
// Spec 7.1 wants the missile bright without being a glowing dot, and this is the honest way to
// get it — the airframe and its trail are lit, not emissive, they are simply the only things in
// the frame high enough to still be in the light.
//
// Flat at ground level so nothing else in the scene changes, and clamped, because past the point
// where the object is fully sunlit there is no more light to give it.
float HighAirLight(float altitudeMetres) {
    return 1.0 + 4.5 * smoothstep(60.0, 900.0, altitudeMetres);
}

// The same idea applied to the shadow terminator rather than to the magnitude. A surface at 3 km
// sees the key light from a direction that is a degree or two higher than the ground does, which
// is the difference between "the sun has set" and "the sun is setting".
float HighAirKeyAbove(vec3 keyDirection, float altitudeMetres) {
    float lift = 0.12 * smoothstep(60.0, 900.0, altitudeMetres);
    return smoothstep(-0.08, 0.06, keyDirection.y + lift);
}
