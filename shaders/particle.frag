#version 450

// Particle shading (spec 8.3).
//
// Two kinds of particle arrive here and the vertex stage says which: emitters carry their own
// radiance and are drawn additively, dust does not and is lit like any other surface. The dust
// case is deliberately the same set of terms the desert floor uses — key, sky ambient, the board,
// the fireball, then haze — because a dust cloud lit by a different rig than the ground it is
// sitting on is the fastest way to make a particle system look pasted on.

#include "atmosphere.glsl"
#include "fire.glsl"
#include "shadow.glsl"

layout(location = 0) in vec2  vUv;
layout(location = 1) in vec4  vTint;
layout(location = 2) in float vAlpha;
layout(location = 3) in vec3  vWorldPos;

layout(location = 0) out vec4 outColor;

void main() {
    float r2 = dot(vUv, vUv);
    if (r2 >= 1.0) discard;

    // A soft round falloff rather than a texture. Squared, so the edge is gradual enough that a
    // 60 m dust puff has no visible boundary against the one behind it.
    float soft  = 1.0 - r2;
    float alpha = vAlpha * soft * soft;
    if (alpha <= 0.002) discard;

    vec3  toCamera = scene.cameraPos.xyz - vWorldPos;
    float distance = length(toCamera);
    vec3  viewDir  = toCamera / max(distance, 1e-4);

    vec3 color;
    if (vTint.w > 0.5) {
        // An emitter: exhaust or an ember. Its own radiance, and no haze, because the haze of
        // spec 6.3 mixes toward the sky and mixing an additive sprite toward the sky just makes
        // it brighter.
        color = vTint.rgb;
    } else {
        // Dust. The billboard has no real normal, so it is shaded as a facing disc: half-Lambert
        // against the key, and the sky ambient weighted as if the puff were a small sphere, which
        // is closer to what a cloud of it actually does than a flat card is.
        vec3  normal   = viewDir;
        float keyAbove = HighAirKeyAbove(scene.keyDirection.xyz, vWorldPos.y);
        float wrap     = 0.5 + 0.5 * dot(normal, scene.keyDirection.xyz);

        // The missile's contrail hangs high enough that the key light still reaches it after the
        // ground has lost it. Same term the airframe uses, and the reason a contrail overhead at
        // dusk is white against a sky that has already gone blue. Flat at ground level, so the
        // four systems that live in the first hundred metres are untouched by it.
        //
        // Taken at a quarter strength. A contrail is dozens of overlapping sprites deep, so its
        // composite is the colour of one particle however faint each is made -- turning the alpha
        // down does not dim a solid trail, it only softens its edges. That colour has to be close
        // to the sky it hangs in; at full strength it was the key light, which at twilight is a
        // saturated orange, and the trail came out as a bar of it ruled across the sunset.
        float high = mix(1.0, HighAirLight(vWorldPos.y), 0.25);

        // Weighted toward the horizon rather than the zenith, which is the opposite of what the
        // ground floor does and for the same reason: the ground is a level surface that sees the
        // sky by the cosine, and a puff of dust is not a surface at all. It sees the whole
        // hemisphere, and at twilight most of that hemisphere's light is in the horizon band.
        vec3 skyAmbient = max(scene.ambientColor.rgb,
                              mix(scene.horizonColor.rgb, scene.zenithColor.rgb, 0.35));

        // Dust receives as well as neighbours the casters (spec 8.2). The settled disc under
        // the cap sits directly in the cloud shadow, and left lit it was the brightest thing on
        // the desert at the moment the sky above it went dark. Only the two key terms are
        // shadowed: sky ambient reaches a shaded puff, and so does the fireball, which is inside
        // the cloud rather than behind it.
        float keyShadow = KeyShadow(vWorldPos, normal);

        vec3 lit = vTint.rgb * scene.keyColor.rgb * wrap * wrap * keyAbove * high * keyShadow;

        // Above one on purpose. Dust is lit from every direction at once, including from the
        // ground it is hanging over, and at the cosine-weighted figure the ground uses it came out
        // the same colour as the ground — which for a screen saver means it is not there at all.
        lit += vTint.rgb * skyAmbient * (scene.groundColor.w * 1.9);

        // Forward scattering. A cloud between the eye and a low sun is brighter than the same
        // cloud lit from behind the eye, and at twilight — the default time of day — the sun is
        // always low. This is the term that makes the missile's trail read as smoke rather than as
        // a smear of dark paint on the sky.
        float forward = max(-dot(viewDir, scene.keyDirection.xyz), 0.0);
        lit += vTint.rgb * scene.keyColor.rgb *
               (forward * forward * forward * 1.4 * keyAbove * keyShadow);

        vec3  toBoard   = scene.boardLight.xyz - vWorldPos;
        float boardDist = length(toBoard);
        lit += vTint.rgb * scene.boardColor.rgb * scene.boardLight.w /
               (1.0 + (boardDist * boardDist) /
                          max(scene.boardColor.w * scene.boardColor.w, 1.0));

        // The one term that matters most: from phase 5 the fireball is the dominant light (spec
        // 8.2), and the collar dust of spec 7.7 exists precisely to have something in the air for
        // it to light.
        lit += FireContribution(vTint.rgb, vWorldPos, normal);

        color = mix(lit, SkyGradient(-viewDir), HazeAmount(distance));
    }

    outColor = vec4(color, alpha);
}
