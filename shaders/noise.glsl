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

// Three dimensions, for the things that are volumes rather than surfaces: the star field, the
// fireball's skin and the smoke. Value noise rather than gradient noise here — the fireball's
// displacement is never differentiated, so the extra eight dot products a gradient version costs
// buy nothing visible.
float Hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float ValueNoise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float n000 = Hash13(i + vec3(0.0, 0.0, 0.0));
    float n100 = Hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = Hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = Hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = Hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = Hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = Hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = Hash13(i + vec3(1.0, 1.0, 1.0));

    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z) * 2.0 - 1.0;
}

float Fbm3(vec3 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < octaves; ++i) {
        sum += ValueNoise3(p) * amp;
        p *= 2.07;
        amp *= 0.5;
    }
    return sum;
}

#endif
