// The particle systems (spec 8.3), described once for the three compute passes and the draw.
//
// The design decision that shapes everything else here: a particle has **no state**. Its position,
// size, colour and opacity are a closed-form function of its index and the cycle clock, evaluated
// wherever they are needed. There is no per-particle buffer, no emitter to step, no free list and
// no atomic to allocate a slot.
//
// That is possible because particles, unlike the fragments of spec 7.3, never collide, never come
// to rest and never have to remember anything. It is worth doing because it removes the whole
// class of bug that particle systems are famous for -- counts that drift, slots that leak, a field
// that looks different depending on how many frames have been drawn -- and because it makes the
// five systems reproducible from the seed in exactly the sense spec 4.2 requires: ask for the same
// `t` and you get the same particle, whether that is the first frame of the cycle or a capture.
//
// Emission is a schedule, not an event. Slot j of a system with n slots and a lifetime L is born
// at t0 + (j + k*n) * L/n for generation k, so exactly one generation of each slot is alive at any
// moment and the live count settles at min(n, n * window / L) with nothing counting anything.
#ifndef NUKE_SAVER_PARTICLE_COMMON_GLSL
#define NUKE_SAVER_PARTICLE_COMMON_GLSL

const uint kParticleSystems = 5u;

// Depth buckets for the back-to-front sort. 256 is one workgroup for the prefix sum and fine
// enough that the ordering error inside a bucket is smaller than one particle's own extent.
const uint kSortBuckets = 256u;

#ifndef PARTICLE_SET
#define PARTICLE_SET 0
#endif

// The sort's output, and the only particle memory in the project. Two ranges of `total` entries:
// [0, total) holds the alpha-blended particles ordered far to near, [total, 2*total) holds the
// additive ones in whatever order they were scattered, because additive blending commutes.
// Unused entries hold kParticleNone and the vertex shader collapses them to a degenerate triangle.
layout(std430, set = PARTICLE_SET, binding = 0) buffer SortBuffer { uint sorted[]; };

layout(std430, set = PARTICLE_SET, binding = 1) buffer BinBuffer {
    uint count[kSortBuckets];   // particles per depth bucket
    uint cursor[kSortBuckets];  // running write position, seeded from the prefix sum
    uint additive;              // how many additive particles have been appended
    uint pad0;
    uint pad1;
    uint pad2;
} bins;

const uint kParticleNone = 0xffffffffu;

#ifndef PARTICLE_NO_PUSH
layout(push_constant) uniform ParticlePush {
    uvec4 caps;    // slot counts of systems 0..3
    vec4  tail;    // x system 4 slots, y gravity, zw horizontal wind (m/s)
    vec4  timing;  // x cycle time, y growth start, z growth end, w city radius
    vec4  mStart;  // xyz missile entry point, w missile phase start
    vec4  mDir;    // xyz missile direction of travel, w missile phase end
    vec4  blast;   // xyz impact point, w the shell's final reach in metres
    vec4  phases;  // x blast start, y blast duration, z gather start, w disperse end
    vec4  cloud;   // x stem height, y cap radius, z missile length, w sort range in metres
} pp;

uint SystemCapacity(uint s) {
    if (s == 0u) return pp.caps.x;
    if (s == 1u) return pp.caps.y;
    if (s == 2u) return pp.caps.z;
    if (s == 3u) return pp.caps.w;
    return uint(pp.tail.x);
}

uint ParticleTotal() {
    return pp.caps.x + pp.caps.y + pp.caps.z + pp.caps.w + uint(pp.tail.x);
}
#endif

// --- randomness ---------------------------------------------------------------------------------
// Stateless, and salted with the generation so that a slot reused after its particle dies comes
// back as a different particle rather than the same one blinking.
uint PHashU(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float PHash(uint seed, uint salt) {
    return float(PHashU(seed * 0x9e3779b9u + salt * 0x85ebca6bu + 0x27d4eb2fu)) / 4294967296.0;
}

vec3 PHash3(uint seed, uint salt) {
    return vec3(PHash(seed, salt), PHash(seed, salt + 1u), PHash(seed, salt + 2u));
}

struct Particle {
    vec3  pos;
    float size;      // world-space radius of the billboard
    vec3  tint;      // linear; above 1 for the additive systems, which are emitters
    float alpha;
    float rnd;
    uint  system;
    bool  additive;  // decides which of the two draws owns it, and so which blend mode
};

#ifndef PARTICLE_NO_PUSH
// Which generation of slot `j` is alive at `t`, if any. `t0`/`t1` bound the emission window; a
// particle born inside it lives out its whole life even if that runs past t1, which is what makes
// the missile's trail persist after the missile has gone (spec 7.1).
bool SlotAge(uint j, uint n, float t0, float t1, float life, float t, out float age, out uint gen) {
    age = 0.0;
    gen = 0u;
    if (n == 0u || t1 <= t0 || life <= 0.0) return false;

    float rate  = float(n) / life;  // births per second across the whole system
    float slots = float(n);

    // The last generation of this slot whose birth still falls inside the emission window.
    float lastGen = floor(((t1 - t0) * rate - float(j)) / slots);
    if (lastGen < 0.0) return false;

    float g = min(floor(((t - t0) * rate - float(j)) / slots), lastGen);
    if (g < 0.0) return false;

    float birth = t0 + (float(j) + g * slots) / rate;
    age = t - birth;
    if (age < 0.0 || age >= life) return false;

    gen = uint(g);
    return true;
}

// The missile's nose at an arbitrary time, so a contrail particle can be placed where the missile
// actually was when it was emitted rather than where the missile is now. The easing mirrors
// Detonation::MissileEase: a contrail laid down at a constant pace behind a decelerating missile
// bunches up at the wrong end and reads as a dashed line.
vec3 MissileNoseAt(float tt) {
    float t0 = pp.mStart.w;
    float t1 = pp.mDir.w;
    float u  = clamp((tt - t0) / max(t1 - t0, 1e-3), 0.0, 1.0);

    float slowed = 1.0 - (1.0 - u) * (1.0 - u);
    return mix(pp.mStart.xyz, pp.blast.xyz, mix(u, slowed, 0.55));
}

// The same law as Detonation::ShellRadius, because the collar dust of spec 7.7 is kicked up by the
// front and has to be born on it. Duplicated deliberately: the alternative is a per-frame upload
// of a curve that is one line of arithmetic.
float ShellRadiusAt(float tt) {
    float start = pp.phases.x;
    float span  = pp.phases.y;
    if (tt < start || span <= 0.0) return 0.0;
    return pp.blast.w * (1.0 - exp(-6.5 * (tt - start) / span));
}

// Integral of a velocity decaying as exp(-k*age): how far something thrown at `v0` has travelled.
float DragTravel(float v0, float k, float age) {
    return v0 * (1.0 - exp(-k * age)) / max(k, 1e-3);
}

// Which depth bucket a particle falls in, 0 farthest. Square-rooted so the near half of the view
// gets most of the buckets, which is where two overlapping particles are large enough on screen
// for the wrong order to be visible at all.
uint ParticleBucket(vec3 pos, vec3 camera) {
    float d = distance(pos, camera);
    float u = clamp(sqrt(d / max(pp.cloud.w, 1.0)), 0.0, 1.0);
    return uint(clamp((1.0 - u) * float(kSortBuckets - 1u), 0.0, float(kSortBuckets - 1u)));
}

// The whole system, in one function. `false` means this index has no live particle this frame.
bool ParticleAt(uint index, float t, out Particle p) {
    p.pos      = vec3(0.0);
    p.size     = 0.0;
    p.tint     = vec3(0.0);
    p.alpha    = 0.0;
    p.rnd      = 0.0;
    p.system   = 0u;
    p.additive = false;

    uint base = 0u;
    uint sys  = kParticleSystems;
    uint cap  = 0u;
    for (uint s = 0u; s < kParticleSystems; ++s) {
        uint c = SystemCapacity(s);
        if (index < base + c) {
            sys = s;
            cap = c;
            break;
        }
        base += c;
    }
    if (sys == kParticleSystems) return false;

    uint  j    = index - base;
    vec2  wind = pp.tail.zw;
    float age;
    uint  gen;
    float life;

    p.system = sys;

    // --- 0: growth puffs (phase 1) ---------------------------------------------------------------
    // Dust kicked up under the rising city. Spread over the footprint rather than tied to
    // individual buildings: the building list lives in the fragment set, and at this density a
    // puff that is near a building is indistinguishable from one that is on it.
    if (sys == 0u) {
        life = 2.6;
        if (!SlotAge(j, cap, pp.timing.y, pp.timing.z, life, t, age, gen)) return false;

        uint  seed  = j * 131u + gen * 7919u;
        float u     = age / life;
        float birth = t - age;
        float a     = PHash(seed, 1u) * 6.2831853;
        p.rnd       = PHash(seed, 3u);

        // On the construction front, not over the whole footprint. Spec 6.5 grows the city
        // outward from the centre, and city.cpp puts a building at normalised radius x at
        // pow(x, 0.7) through the phase; this is that inverted, so the dust sweeps outward with
        // the buildings instead of hanging over desert that is still empty.
        float progress = clamp((birth - pp.timing.y) / max(pp.timing.z - pp.timing.y, 1e-3),
                               0.0, 1.0);
        float front    = pow(progress, 1.0 / 0.7);
        float r        = pp.timing.w * clamp(front + (PHash(seed, 2u) - 0.5) * 0.24, 0.0, 1.0);

        p.pos   = vec3(cos(a) * r, 1.0 + age * (1.4 + 1.6 * p.rnd), sin(a) * r) +
                  vec3(wind.x, 0.0, wind.y) * (age * 0.25);
        p.size  = mix(4.0, 19.0, u) * (0.6 + 0.8 * p.rnd);
        p.tint  = vec3(0.46, 0.36, 0.26);
        p.alpha = 0.32 * sin(3.14159265 * u);
        return true;
    }

    // --- 1: missile exhaust and contrail (phase 4) -----------------------------------------------
    // One system with two populations, split by slot rather than by hash so each gets its own
    // emission rate: a quarter-second flame and a ten-second contrail cannot share a schedule.
    //
    // The contrail is thin on purpose. It used to be born at a twelfth of a body length across and
    // grow to half of one -- a 90 m smoke bank hanging behind a 190 m missile, which at the range
    // the missile now enters from was the only thing visible of it: a dark smear with a bright dot
    // at the leading end. A contrail is a scratch on the sky, and what makes it read over the
    // mountains is its length and its brightness, not its width.
    if (sys == 1u) {
        uint hotSlots = max(cap / 5u, 1u);
        bool hot      = j < hotSlots;

        // Seven seconds, so the far end of the trail is visibly older than the near end by the
        // time the missile arrives: at sixteen the whole trail sat at the same age and faded as a
        // unit rather than thinning out behind.
        life = hot ? 0.26 : 7.0;

        uint local = hot ? j : j - hotSlots;

        // The trail takes a slice of the slots rather than all that are left. Emission rate is
        // slots over lifetime, so the slice is what sets how densely the trail is laid down: with
        // the whole remainder it was a puff every half metre, and a stack that deep is opaque
        // whatever each puff's alpha says.
        uint n = hot ? hotSlots : max(cap / 9u, 1u);
        if (!hot && local >= n) return false;
        if (!SlotAge(local, n, pp.mStart.w, pp.mDir.w, life, t, age, gen)) return false;

        uint  seed  = (j + 1u) * 977u + gen * 6151u;
        float u     = age / life;
        float birth = t - age;
        vec3  jit   = PHash3(seed, 11u) * 2.0 - 1.0;
        p.rnd       = PHash(seed, 17u);

        // Emitted at the nozzle, which is one body length behind the nose.
        vec3 nozzle = MissileNoseAt(birth) - pp.mDir.xyz * pp.cloud.z;

        if (hot) {
            // The flame, kept close to the nozzle and dim enough to stay a flame. At four times
            // this magnitude and two and a half times this size it bloomed into a featureless
            // bead that swallowed the airframe, which is the "glowing dot" the missile is not
            // supposed to be.
            p.pos      = nozzle - pp.mDir.xyz * (age * 26.0) +
                         jit * (pp.cloud.z * 0.02 + age * 5.0);
            p.size     = mix(pp.cloud.z * 0.030, pp.cloud.z * 0.075, u);
            p.tint     = mix(vec3(1.9, 1.05, 0.38), vec3(0.6, 0.16, 0.03), u);
            p.alpha    = 1.0 - u * u;
            p.additive = true;
        } else {
            // The contrail. It barely moves: a smoke trail that drifts is a smoke trail, and what
            // is wanted is the line the missile drew. It widens slowly, sags a little rather than
            // rising, and takes almost no jitter, so the emitted points lie on the path instead of
            // in a tube around it.
            p.pos   = nozzle - pp.mDir.xyz * (age * 1.2) +
                      jit * (pp.cloud.z * 0.03 + age * 0.9) +
                      vec3(wind.x, 0.0, wind.y) * (age * 0.22) - vec3(0.0, age * 0.5, 0.0);

            // Thin against the missile, not thin in pixels. At a fiftieth of a body length it was
            // under a pixel across from where the camera stands and the trail simply was not
            // there; a sixth of one still reads as a scratch on the sky next to a 190 m airframe.
            p.size  = mix(pp.cloud.z * 0.035, pp.cloud.z * 0.15, sqrt(u));

            // Grey, not white. What the eye reads as a white contrail is a grey one against a
            // brighter sky; given a near-white albedo and the sky ambient the dust systems need,
            // this one came out brighter than the sunset behind it.
            p.tint  = vec3(0.34, 0.40, 0.56);

            // And faint. It is smoke thinning behind a missile, not a line drawn on the sky.
            p.alpha = 0.26 * (1.0 - u * u) * smoothstep(0.0, 0.02, u);
        }
        return true;
    }

    // --- 2: ground collar dust (phases 6-7) ------------------------------------------------------
    // The base surge: a skirt of dust thrown up where the shell meets the ground, born on the
    // front and left behind by it (spec 7.7).
    if (sys == 2u) {
        life     = 6.0;
        float t0 = pp.phases.x;
        float t1 = pp.phases.x + pp.phases.y * 0.55;
        if (!SlotAge(j, cap, t0, t1, life, t, age, gen)) return false;

        uint  seed  = j * 1597u + gen * 33107u;
        float u     = age / life;
        float birth = t - age;
        float a     = PHash(seed, 1u) * 6.2831853;
        p.rnd       = PHash(seed, 5u);

        float ring = ShellRadiusAt(birth) * (0.80 + 0.24 * PHash(seed, 2u));
        float r    = ring + DragTravel(50.0 * (0.6 + 0.8 * PHash(seed, 3u)), 0.55, age);
        float h    = 2.0 + DragTravel(18.0 * (0.4 + 1.2 * PHash(seed, 4u)), 0.75, age);

        p.pos   = vec3(pp.blast.x + cos(a) * r, h, pp.blast.z + sin(a) * r) +
                  vec3(wind.x, 0.0, wind.y) * (age * 0.4);
        p.size  = mix(12.0, 64.0, sqrt(u));
        p.tint  = vec3(0.44, 0.34, 0.25);
        p.alpha = 0.14 * smoothstep(0.0, 0.10, u) * (1.0 - u * u);
        return true;
    }

    // --- 3: settled dust (phases 7-10) -----------------------------------------------------------
    // What is left hanging over the footprint once the debris is down. Low, wide and very faint:
    // its job is to keep the basin from reading as clean desert for the last half of the cycle.
    if (sys == 3u) {
        life     = 30.0;
        float t0 = pp.phases.x + pp.phases.y * 0.6;
        float t1 = pp.phases.w;
        if (!SlotAge(j, cap, t0, t1, life, t, age, gen)) return false;

        uint  seed = j * 2011u + gen * 15991u;
        float u    = age / life;
        float a    = PHash(seed, 1u) * 6.2831853;
        float r    = sqrt(PHash(seed, 2u)) * pp.blast.w * 0.95;
        float h0   = 6.0 + PHash(seed, 4u) * 80.0;
        p.rnd      = PHash(seed, 6u);

        p.pos   = vec3(pp.blast.x + cos(a) * r, h0 * (1.0 - 0.45 * u), pp.blast.z + sin(a) * r) +
                  vec3(wind.x, 0.0, wind.y) * (age * 0.55);
        p.size  = mix(35.0, 95.0, u);
        p.tint  = vec3(0.42, 0.33, 0.25);
        p.alpha = 0.105 * sin(3.14159265 * u);
        return true;
    }

    // --- 4: embers (phases 8-9) ------------------------------------------------------------------
    // Hot points carried up the stem while the cloud is gathering. Additive, tiny and flickering:
    // they are the one thing in the back half of the cycle that is still emitting.
    life     = 5.0;
    float t0 = pp.phases.z;
    float t1 = pp.phases.w;
    if (!SlotAge(j, cap, t0, t1, life, t, age, gen)) return false;

    uint  seed = j * 3559u + gen * 21851u;
    float u    = age / life;
    float a    = PHash(seed, 1u) * 6.2831853;
    float r    = pp.cloud.y * (0.10 + 0.42 * PHash(seed, 2u));
    float rise = 26.0 + PHash(seed, 3u) * 70.0;
    float y0   = PHash(seed, 4u) * pp.cloud.x * 0.45;
    p.rnd      = PHash(seed, 6u);

    float spread = 0.35 + 0.65 * u;
    p.pos = vec3(pp.blast.x + cos(a) * r * spread, y0 + rise * age,
                 pp.blast.z + sin(a) * r * spread) +
            vec3(wind.x, 0.0, wind.y) * (age * 0.8);
    p.size     = mix(3.4, 1.1, u) * (0.6 + 0.8 * p.rnd);
    p.tint     = mix(vec3(16.0, 5.6, 1.0), vec3(3.4, 0.55, 0.06), u);
    p.alpha    = (1.0 - u * u) * (0.55 + 0.45 * sin(t * 9.0 + p.rnd * 31.0));
    p.additive = true;
    return true;
}
#endif

#endif
