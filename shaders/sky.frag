#version 450

// The sky (spec 6.3). The gradient, the stars and the body all live in atmosphere.glsl, because
// the ground pass needs the gradient too — a mountain has to fade into exactly the sky behind it.
//
// Analytic rather than a cubemap. A cubemap would need generating, storing and filtering, and
// spec 6.1 forbids loading one; evaluating the gradient per pixel costs less than the memory
// traffic of sampling a face, and the body then lands exactly where the light comes from by
// construction rather than by a baking step that could go stale.

#include "atmosphere.glsl"

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 dir = ViewRay(vNdc);

    vec3 sky = SkyGradient(dir);
    sky += Stars(dir, scene.keyColor.w);
    sky += CelestialBody(dir);

    // Below the horizon the earth is in the way. Fading to the ground colour, and taking the body
    // with it, keeps a twilight sun that has dipped below the horizon from glaring through the
    // ground it is supposed to be behind. Terrain covers most of this, but not the gap beyond the
    // skirt, and not before the terrain draws.
    //
    // The edge is centred exactly on dir.y = 0 and is one pixel wide, which matters more than it
    // looks. dir is normalised, so a threshold at any *other* value of dir.y describes a cone
    // about the vertical axis rather than a plane, and a cone projects to a curve: a fade from 0
    // to -0.045 bowed the horizon by 33 rows across a 3440-wide frame. Only dir.y = 0 itself is
    // planar. fwidth gives the antialiasing without moving the edge off it.
    float fade  = max(fwidth(dir.y), 1e-5);
    float below = smoothstep(-fade, fade, -dir.y);
    sky = mix(sky, scene.groundColor.rgb, below);

    outColor = vec4(sky, 1.0);
}
