#version 450

// HDR resolve (spec 8.1): exposure, ACES, sRGB encode, dither.
//
// This is the only place in the project where linear light becomes display-encoded. Spec 5.1
// says sRGB encoding happens once and nowhere else, and the swapchain is a UNORM format
// precisely so that this shader owns the transfer function rather than the hardware.

// The tonemap's own set 0 is the HDR image and the exposure buffer, so the scene block lands in
// set 1 here. The blast refraction below needs the camera and the shell, which is why this pass
// sees the scene at all.
#define SCENE_SET 1

#define EXPOSURE_READONLY
#include "exposure.glsl"
#include "scene.glsl"

layout(set = 0, binding = 0) uniform sampler2D uHdr;
layout(set = 0, binding = 2) uniform sampler2D uBloom;

layout(push_constant) uniform Push {
    float exposure;    // the base exposure of the time of day; the adaptation's reference
    float ditherAmp;   // in output LSBs; 1.0 is the usual choice for 8-bit
    float nightShift;  // 0 by day, 1 when the moon is the key light
    float delta;
    float width;
    float height;
    float minExposure;
    float maxExposure;
    float bloom;  // how much of the chain is added back (spec 8.1)
    float pad0;
    float pad1;
    float pad2;
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

// The blast shell (spec 7.2): a refractive boundary that offsets the screen-space position of
// everything behind it, strongest at the shell surface and falling off sharply on both sides.
//
// Done here rather than as a pass of its own because this is the one place in the frame that
// already has the finished HDR image as a texture and is writing somewhere else. A separate pass
// would need a second full-size HDR image to read from, to the tune of 20 MB a monitor, to arrive
// at the same sample.
vec2 BlastRefraction(vec2 uv) {
    float radius = scene.blast.w;
    if (radius <= 0.0) return uv;

    vec3 origin    = scene.cameraPos.xyz;
    vec3 direction = ViewRay(uv * 2.0 - 1.0);

    vec3  toCentre = scene.blast.xyz - origin;
    float along    = dot(toCentre, direction);
    if (along <= 0.0) return uv;  // the shell is behind the camera

    // The impact parameter: how close this pixel's ray passes to the centre. A ray that grazes the
    // shell has one close to the radius, and that is exactly the set of pixels the boundary is.
    float impact = sqrt(max(dot(toCentre, toCentre) - along * along, 0.0));

    // Sharp on both sides, as spec 7.2 requires: this has to read as a moving boundary, not as a
    // general blur over the middle of the frame.
    float width = max(radius * 0.05, 4.0);
    float edge  = (impact - radius) / width;
    float band  = exp(-edge * edge);
    if (band < 0.002) return uv;

    // Outward from the shell's own centre on screen. A fixed direction would shear the image; this
    // is the direction the surface normal actually projects to.
    vec4 clip = scene.viewProj * vec4(scene.blast.xyz, 1.0);
    if (clip.w <= 0.0) return uv;
    vec2 centre = (clip.xy / clip.w) * 0.5 + 0.5;

    vec2 outward = uv - centre;
    float len    = length(outward);
    if (len < 1e-5) return uv;

    // The offset shrinks as the shell grows. A front a kilometre across bends the same amount of
    // light as one a hundred metres across did, spread over ten times the screen.
    float strength = 0.035 * band * clamp(220.0 / max(radius, 1.0), 0.18, 1.0);

    return uv + (outward / len) * strength;
}

void main() {
    // The adapted exposure of spec 8.2, not the authored one. The authored value is still here —
    // it is what the adaptation is anchored to and what it falls back on before the first frame
    // has been measured — but what multiplies the frame is what the histogram said.
    float exposure = ex.initialised > 0.5 ? ex.exposure : pc.exposure;

    vec2 uv  = BlastRefraction(vUV);
    vec3 hdr = texture(uHdr, uv).rgb;

    // Bloom (spec 8.1), added before the exposure rather than after it. It has to go through the
    // same adaptation as everything else: a glow that survives the flash unchanged would be the
    // one thing on screen the white-out did not reach.
    hdr += texture(uBloom, uv).rgb * pc.bloom;

    hdr *= exposure;
    vec3 sdr = LinearToSrgb(ACESFilm(Scotopic(hdr, pc.nightShift)));

    // Dither in display space, centred on zero, scaled to the output quantum.
    sdr += (Dither(gl_FragCoord.xy) - 0.5) * (pc.ditherAmp / 255.0);

    outColor = vec4(clamp(sdr, 0.0, 1.0), 1.0);
}
