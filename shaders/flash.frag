#version 450

// The flash of phase 5 (spec 7.2).
//
// A fullscreen additive term, written into the HDR target rather than applied at the end. That is
// the whole point of doing it here: the auto-exposure of spec 8.2 reads the HDR buffer, so a flash
// that lives in it is a flash the exposure reacts to, and the slow recovery afterwards falls out
// of the adaptation rather than out of a scripted fade.

#include "scene.glsl"

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main() {
    float flash = scene.fireColor.w;
    if (flash <= 0.0) discard;

    // Very slightly hotter in the middle of the frame. Not a vignette — a vignette would read as a
    // lens — just enough gradient that the white has a centre.
    vec2  ndc    = vUV * 2.0 - 1.0;
    float centre = 1.0 - 0.12 * dot(ndc, ndc);

    outColor = vec4(vec3(flash * centre), 1.0);
}
