#version 450

// HDR resolve (spec 8.1): exposure, ACES, sRGB encode, dither.
//
// This is the only place in the project where linear light becomes display-encoded. Spec 5.1
// says sRGB encoding happens once and nowhere else, and the swapchain is a UNORM format
// precisely so that this shader owns the transfer function rather than the hardware.

layout(set = 0, binding = 0) uniform sampler2D uHdr;

layout(push_constant) uniform Push {
    float exposure;    // linear multiplier applied before the curve
    float ditherAmp;   // in output LSBs; 1.0 is the usual choice for 8-bit
    float nightShift;  // 0 by day, 1 when the moon is the key light
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

// The Purkinje shift. Below roughly the luminance of a moonlit surface the cones stop
// contributing and vision goes rod-only: colour drains away and what is left reads blue.
//
// Without it a night scene is a day scene with the gain turned down, which is exactly what this
// looked like — moonlight on desert sand is warm in physical terms, so the whole basin came out a
// mid brown that no amount of dimming turns into night. The shift is keyed on luminance rather
// than applied flat, so the city's own windows and, later, the fireball keep their colour: they
// are well above the threshold, which is the point.
vec3 Scotopic(vec3 color, float amount) {
    if (amount <= 0.0) return color;

    float lum  = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float rods = amount * (1.0 - smoothstep(0.004, 0.10, lum));

    return mix(color, vec3(lum) * vec3(0.72, 0.90, 1.34), rods);
}

void main() {
    vec3 hdr = texture(uHdr, vUV).rgb * pc.exposure;
    vec3 sdr = LinearToSrgb(ACESFilm(Scotopic(hdr, pc.nightShift)));

    // Dither in display space, centred on zero, scaled to the output quantum.
    sdr += (Dither(gl_FragCoord.xy) - 0.5) * (pc.ditherAmp / 255.0);

    outColor = vec4(clamp(sdr, 0.0, 1.0), 1.0);
}
