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
//
// GradientCorners is the noise given its cell `i` and the four corner gradients; the hashing is
// left to the caller, so one that evaluates several nearby points can hash each corner once.
float GradientCorners(vec2 p, vec2 i, vec2 g00, vec2 g10, vec2 g01, vec2 g11) {
    vec2 f = p - i;
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float n00 = dot(g00, f - vec2(0.0, 0.0));
    float n10 = dot(g10, f - vec2(1.0, 0.0));
    float n01 = dot(g01, f - vec2(0.0, 1.0));
    float n11 = dot(g11, f - vec2(1.0, 1.0));

    return mix(mix(n00, n10, u.x), mix(n01, n11, u.x), u.y) * 1.414;
}

vec2 Gradient(vec2 corner) { return Hash22(corner) * 2.0 - 1.0; }

// Gradient noise at p and at two neighbours, px displaced from it along +x only and pz along +y
// only, each by less than one lattice cell. The three cells then share corners — px sits in p's
// cell or the one to its right, pz in p's cell or the one above — so eight hashes cover what
// three separate evaluations would hash twelve for. Every hash input is the same integer lattice
// point a separate evaluation would use, so the result is theirs, not an approximation of it.
vec3 GradientNoise2x3(vec2 p, vec2 px, vec2 pz) {
    vec2 i   = floor(p);
    vec2 g00 = Gradient(i + vec2(0.0, 0.0)), g10 = Gradient(i + vec2(1.0, 0.0));
    vec2 g01 = Gradient(i + vec2(0.0, 1.0)), g11 = Gradient(i + vec2(1.0, 1.0));
    vec2 g20 = Gradient(i + vec2(2.0, 0.0)), g21 = Gradient(i + vec2(2.0, 1.0));
    vec2 g02 = Gradient(i + vec2(0.0, 2.0)), g12 = Gradient(i + vec2(1.0, 2.0));

    vec2 ix = floor(px), iz = floor(pz);
    bool sx = ix.x > i.x, sz = iz.y > i.y;

    return vec3(GradientCorners(p, i, g00, g10, g01, g11),
                GradientCorners(px, ix, sx ? g10 : g00, sx ? g20 : g10, sx ? g11 : g01,
                                sx ? g21 : g11),
                GradientCorners(pz, iz, sz ? g01 : g00, sz ? g11 : g10, sz ? g02 : g01,
                                sz ? g12 : g11));
}

// Fractal sum of GradientNoise2x3 over the three points, octave for octave. The offset doubles
// with each octave, so it must stay under one cell at the finest one.
vec3 Fbm2x3(vec2 p, vec2 px, vec2 pz, int octaves) {
    vec3  sum = vec3(0.0);
    float amp = 0.5;
    for (int i = 0; i < octaves; ++i) {
        sum += GradientNoise2x3(p, px, pz) * amp;
        p *= 2.03;
        px *= 2.03;
        pz *= 2.03;
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
