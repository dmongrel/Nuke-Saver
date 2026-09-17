// The building transform (spec 6.4, 6.5, 7.3).
//
// Shared by the scene pass and the shadow pass. It lives here rather than in building.vert
// because the two passes MUST agree, to the frame, on which buildings are standing and how far
// each has grown: a building the shadow map still has but the scene does not is a shadow cast by
// nothing, and one the scene has but the map does not is a building standing in its own light.
//
// Needs scene.glsl for the growth clock and the blast shell, so include that first.
#ifndef NUKE_SAVER_BUILDING_COMMON_GLSL
#define NUKE_SAVER_BUILDING_COMMON_GLSL

struct BuildingVertex {
    vec3  world;
    vec3  normal;
    float facadeU;  // metres along the face, for window columns
    bool  hidden;   // not yet grown, or already blown to fragments
};

BuildingVertex BuildingPlace(vec3 inPosition, vec3 inNormal, vec2 inUv, vec4 inCenterRotation,
                             vec4 inExtentGrowth) {
    BuildingVertex b;
    b.world   = vec3(0.0);
    b.normal  = vec3(0.0, 1.0, 0.0);
    b.facadeU = 0.0;
    b.hidden  = true;

    float start    = inExtentGrowth.z;
    float duration = max(inExtentGrowth.w, 1e-3);

    // scene.timing.x is seconds into the growth phase, which the timeline of spec 4 places
    // wherever it likes in the cycle. Negative before the phase begins, which is what keeps the
    // desert empty through phase 0.
    float progress = (scene.timing.x - start) / duration;

    // Spec 7.3: when the shell reaches this building it is replaced by its fragments in the same
    // frame. Measured to the box's middle, exactly as shaders/fragment_sim.comp measures it, so
    // the two can never disagree about whether this thing is still standing.
    vec3 middle = vec3(inCenterRotation.x, inCenterRotation.y * 0.5, inCenterRotation.z);
    bool shattered = scene.blast.w > 0.0 && length(middle - scene.blast.xyz) <= scene.blast.w;

    if (progress <= 0.0 || shattered) return b;

    // Spec 6.5: ease out, and MUST NOT overshoot or bounce. A cubic ease-out is monotonic and
    // reaches exactly 1, which a spring or a back-ease would not.
    float t     = clamp(progress, 0.0, 1.0);
    float eased = 1.0 - pow(1.0 - t, 3.0);

    vec2  halfExtent = inExtentGrowth.xy;
    float height     = inCenterRotation.y * eased;

    vec3 local = vec3(inPosition.x * halfExtent.x, inPosition.y * height,
                      inPosition.z * halfExtent.y);

    float c = cos(inCenterRotation.w);
    float s = sin(inCenterRotation.w);

    // The normal rotates with the building but is not scaled: the transform is a rotation and a
    // per-axis scale, and for an axis-aligned box the face normals are the scale's own axes, so
    // they survive it unchanged.
    b.world  = vec3(local.x * c - local.z * s + inCenterRotation.x, local.y,
                    local.x * s + local.z * c + inCenterRotation.z);
    b.normal = vec3(inNormal.x * c - inNormal.z * s, inNormal.y, inNormal.x * s + inNormal.z * c);

    // Metres along the facade. Which half-extent that is depends on which pair of faces this is:
    // the faces normal to x run along z, and vice versa. Taken from the *unrotated* normal, since
    // that is the one still aligned to the extents.
    float faceWidth = abs(inNormal.x) > 0.5 ? halfExtent.y : halfExtent.x;
    b.facadeU = inUv.x * faceWidth * 2.0;
    b.hidden  = false;
    return b;
}

#endif
