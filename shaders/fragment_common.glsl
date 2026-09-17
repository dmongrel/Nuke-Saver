// Shared layout for the fragment system (spec 7.3 to 7.7).
//
// Three buffers and one push block, described once so the init pass, the simulation pass and the
// draw cannot drift apart. Everything a fragment needs is in GPU memory from the moment the world
// is built; the CPU writes the box list and then never touches per-fragment data again, which is
// what spec 7.3 requires.
#ifndef NUKE_SAVER_FRAGMENT_COMMON_GLSL
#define NUKE_SAVER_FRAGMENT_COMMON_GLSL

// One shatterable box: a building, or one of the countdown board's bars and plates. Matches
// render::ShatterBox.
struct ShatterBox {
    vec4 centerYaw;    // xz footprint centre, y underside, w yaw
    vec4 extentQuads;  // xy half-extent, z height, w side quads S
    vec4 color;        // rgb linear albedo, w index of this box's first fragment
};

// Four sides of S quads and two caps of S/2, two triangles each.
int BoxCapQuads(int sideQuads) { return max(sideQuads / 2, 1); }
int BoxTriangles(int sideQuads) { return 8 * sideQuads + 4 * BoxCapQuads(sideQuads); }

// The rest shape of one triangle, written once by the init pass and read every frame after.
// Corners are held relative to the anchor so the simulation only ever moves one point and one
// orientation per fragment.
struct FragRest {
    vec4 anchor;  // xyz world position at rest, w = parent box index
    vec4 c0;        // xyz corner offset, w = colour r
    vec4 c1;        // w = colour g
    vec4 c2;        // w = colour b
};

// State lives in one buffer written in place: each invocation touches only its own fragment, so
// there is nothing to ping-pong.
struct FragState {
    vec4 pos;   // xyz world anchor, w = phase (see kFrag* below)
    vec4 vel;   // xyz m/s, w = the fragment's own release time during disperse
    vec4 quat;  // orientation, xyzw
    vec4 spin;  // xyz rad/s, w = a per-fragment random, drawn once and held
};

// A fragment is in exactly one of these. Held as a float because the whole struct is vec4s and a
// mixed-type struct in std430 is a padding rule waiting to be got wrong.
const float kFragIntact  = 0.0;  // its building still stands; nothing is drawn for it
const float kFragFlying  = 1.0;  // ballistic: impulse, gravity, drag, tumble (spec 7.4)
const float kFragSettled = 2.0;  // at rest on the ground
const float kFragGather  = 3.0;  // drawn toward its place on the mushroom (spec 7.5)

// The compute passes own set 0; the draw already spends set 0 on the scene uniforms and puts
// these in set 1. Same three buffers either way, so the set index is the only thing that varies
// and it is named by whoever includes this.
#ifndef FRAGMENT_SET
#define FRAGMENT_SET 0
#endif

layout(std430, set = FRAGMENT_SET, binding = 0) readonly buffer BoxBuffer { ShatterBox boxes[]; };
layout(std430, set = FRAGMENT_SET, binding = 1) buffer RestBuffer { FragRest rest[]; };
layout(std430, set = FRAGMENT_SET, binding = 2) buffer StateBuffer { FragState state[]; };

#ifndef FRAGMENT_NO_PUSH
layout(push_constant) uniform FragmentPush {
    uvec4 counts;   // x box count, y total fragments, zw unused
    vec4  blast;    // xyz impact point, w current shell radius in metres
    vec4  timing;   // x cycle seconds, y frame delta, z gather start, w gather end
    vec4  release;  // x disperse start, y disperse end, z cloud growth 0..1, w city radius
    vec4  wind;     // xyz m/s, w gravity (positive, applied downward)
    vec4  cloud;    // x stem height, y cap centre height, z cap radius, w cap tube radius
} fp;
#endif

// --- small helpers ------------------------------------------------------------------------------

// The same stateless hash the CPU side uses in spirit: a fragment's randomness must not depend on
// how many frames have been drawn, or a cloud would reshuffle itself every time the frame rate
// changed.
uint FragHashU(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float FragHash(uint seed, uint salt) {
    return float(FragHashU(seed * 0x9e3779b9u + salt * 0x85ebca6bu + 0x165667b1u)) /
           4294967296.0;
}

// Which box a fragment belongs to. The boxes are in fragment order and each carries the index of
// its first, so this is a binary search over 728 entries — ten steps, and the price of letting a
// 2 m rail and a 136 m plate be cut as finely as each deserves rather than both getting twelve
// triangles because the arithmetic was easier.
uint FragmentBox(uint fragment, uint boxCount) {
    uint lo = 0u;
    uint hi = boxCount - 1u;
    while (lo < hi) {
        uint mid = (lo + hi + 1u) >> 1u;
        if (uint(boxes[mid].color.w + 0.5) <= fragment) lo = mid;
        else hi = mid - 1u;
    }
    return lo;
}

vec3 QuatRotate(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// Integrates an orientation under a constant angular velocity. First order and then renormalised,
// which is all a tumbling chip of concrete needs and costs four multiplies.
vec4 QuatIntegrate(vec4 q, vec3 omega, float dt) {
    vec4 dq = 0.5 * dt * vec4(omega * q.w + cross(omega, q.xyz), -dot(omega, q.xyz));
    return normalize(q + dq);
}

#ifndef FRAGMENT_NO_PUSH
// Where this fragment belongs on the mushroom (spec 7.5). Assignment is a pure function of the
// fragment's rest position and index, so it is stable for the whole cycle: a fragment that starts
// near the centre goes to the stem and one from the outskirts goes to the cap rim, every frame,
// without anything being stored.
vec3 MushroomTarget(uint index, vec3 restPos, float grow) {
    const float cityRadius = max(fp.release.w, 1.0);

    // How far out this fragment's building stood, 0 at the centre and 1 at the edge.
    float r0 = clamp(length(restPos.xz) / cityRadius, 0.0, 1.0);

    float a  = FragHash(index, 11u) * 6.2831853;
    float u  = FragHash(index, 23u);
    float v  = FragHash(index, 37u);

    float stemHeight = fp.cloud.x * grow;
    float capHeight  = fp.cloud.y * grow;
    float capRadius  = fp.cloud.z * grow;
    float capTube    = fp.cloud.w * grow;

    // Spec 7.5: the surface has to churn, and the cloud must never look like a solid model. The
    // wobble is a function of the clock, so a fragment's place on the cloud keeps moving even
    // after it has arrived there — which is the difference between a cloud and a sculpture.
    float t   = fp.timing.x;
    float wob = sin(a * 3.0 + t * 0.33 + FragHash(index, 53u) * 6.2831) *
                sin(a * 1.7 - t * 0.21 + FragHash(index, 59u) * 6.2831);

    if (r0 < 0.42) {
        // The stem: a tapering column. Fragments from the middle of the city rise through it.
        float h     = mix(0.04, 1.0, u) * stemHeight;
        float taper = mix(1.0, 0.45, h / max(stemHeight, 1.0));
        float rad   = capTube * 0.95 * taper * (0.35 + 0.65 * sqrt(v)) * (1.0 + 0.30 * wob);
        return vec3(cos(a) * rad, h + capTube * 0.12 * wob, sin(a) * rad);
    }

    // The cap: a torus, thicker under the rim than over it so the silhouette sits rather than
    // floats. The further out a fragment started, the further out on the ring it lands.
    float ring  = mix(0.50, 1.0, (r0 - 0.42) / 0.58);
    float major = capRadius * ring * (1.0 + 0.16 * wob);
    float minor = capTube * (0.35 + 0.65 * sqrt(u)) * (1.0 + 0.28 * wob);
    float phi   = v * 6.2831853;

    return vec3(cos(a) * (major + cos(phi) * minor),
                capHeight + sin(phi) * minor * 0.8 + capTube * 0.18 * wob,
                sin(a) * (major + cos(phi) * minor));
}
#endif

#endif
