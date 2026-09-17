// Reading the key light's shadow map (spec 8.2).
//
// Included by every surface that receives: the city, the desert, the fragments and the board.
// Needs scene.glsl for the light matrix and the map's parameters, so include that first.
#ifndef NUKE_SAVER_SHADOW_GLSL
#define NUKE_SAVER_SHADOW_GLSL

// A comparison sampler, so the hardware does the depth test and filters the result across its 2x2
// neighbourhood rather than handing back a depth to compare by hand. That is what makes a 3x3 tap
// pattern cover four times the area it looks like it does.
layout(set = SCENE_SET, binding = 1) uniform sampler2DShadow shadowMap;

// How much of the key light reaches this point: 1 lit, 0 fully shadowed.
float KeyShadow(vec3 worldPos, vec3 normal) {
    if (scene.shadow.w <= 0.0) return 1.0;

    // Offset along the surface's own normal by about a texel before looking up. The alternative is
    // a constant depth bias large enough for the worst glancing surface in the scene, and a bias
    // that large lifts every shadow off the thing casting it -- the object appears to hover, which
    // is a worse artefact than the acne it was meant to cure.
    vec3 at = worldPos + normal * scene.shadow.y;

    vec4 clip = scene.lightViewProj * vec4(at, 1.0);
    vec3 ndc  = clip.xyz / clip.w;

    // Outside the map is lit, not shadowed. The map covers the city, the debris field and the
    // cloud; at a low sun the cloud's own shadow lands kilometres beyond all three, and clamping
    // to the edge texel would smear whatever happened to be on the border across the desert.
    if (any(greaterThan(abs(ndc.xy), vec2(1.0))) || ndc.z > 1.0) return 1.0;

    vec2  uv    = ndc.xy * 0.5 + 0.5;
    float depth = ndc.z - scene.shadow.z;

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(shadowMap, vec3(uv + vec2(x, y) * scene.shadow.x, depth));
        }
    }
    return sum * (1.0 / 9.0);
}

#endif
