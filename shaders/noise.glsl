// Compact GLSL noise. The CPU side has its own in core/noise.h; this is not a port of it and does
// not need to match. Terrain *shape* is generated once on the CPU and must be reproducible; what
// is done here is surface detail, which only has to look right.

#ifndef NUKE_SAVER_NOISE_GLSL
#define NUKE_SAVER_NOISE_GLSL

float Hash12(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

vec2 Hash22(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.xx + q.yz) * q.zy);
}

// Gradient noise, roughly [-1,1]. Quintic fade, because this drives a normal and Perlin's cubic
// creases visibly once you differentiate it.
float GradientNoise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    vec2 g00 = Hash22(i + vec2(0.0, 0.0)) * 2.0 - 1.0;
    vec2 g10 = Hash22(i + vec2(1.0, 0.0)) * 2.0 - 1.0;
    vec2 g01 = Hash22(i + vec2(0.0, 1.0)) * 2.0 - 1.0;
    vec2 g11 = Hash22(i + vec2(1.0, 1.0)) * 2.0 - 1.0;

    float n00 = dot(g00, f - vec2(0.0, 0.0));
    float n10 = dot(g10, f - vec2(1.0, 0.0));
    float n01 = dot(g01, f - vec2(0.0, 1.0));
    float n11 = dot(g11, f - vec2(1.0, 1.0));

    return mix(mix(n00, n10, u.x), mix(n01, n11, u.x), u.y) * 1.414;
}

float Fbm2(vec2 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < octaves; ++i) {
        sum += GradientNoise2(p) * amp;
        p *= 2.03;
        amp *= 0.5;
    }
    return sum;
}

#endif
