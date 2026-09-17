#version 450

// HDR resolve (spec 8.1): exposure, ACES, sRGB encode, dither.
//
// This is the only place in the project where linear light becomes display-encoded. Spec 5.1
// says sRGB encoding happens once and nowhere else, and the swapchain is a UNORM format
// precisely so that this shader owns the transfer function rather than the hardware.

layout(set = 0, binding = 0) uniform sampler2D uHdr;

layout(push_constant) uniform Push {
    float exposure;   // linear multiplier applied before the curve
    float ditherAmp;  // in output LSBs; 1.0 is the usual choice for 8-bit
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

// Narkowicz's ACES fit. Cheap, and close enough that nobody has ever told the difference in a
// moving image.
vec3 ACESFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 LinearToSrgb(vec3 c) {
    bvec3 cutoff = lessThan(c, vec3(0.0031308));
    vec3  high   = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    vec3  low    = c * 12.92;
    return mix(high, low, vec3(cutoff));
}

// Interleaved gradient noise. One multiply-add and a fract, and it breaks up the banding that
// an 8-bit swapchain would otherwise show across the wide, slow gradients this project is full
// of - sky, fireball falloff, smoke.
float Dither(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec3 hdr = texture(uHdr, vUV).rgb * pc.exposure;
    vec3 sdr = LinearToSrgb(ACESFilm(hdr));

    // Dither in display space, centred on zero, scaled to the output quantum.
    sdr += (Dither(gl_FragCoord.xy) - 0.5) * (pc.ditherAmp / 255.0);

    outColor = vec4(clamp(sdr, 0.0, 1.0), 1.0);
}
