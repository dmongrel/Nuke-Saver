// The per-frame scene block, shared by every pass.
//
// This mirrors SceneUniforms in src/render/scene_uniforms.h byte for byte. Change one and the
// other must change with it: there is no reflection here to catch a mismatch, and the symptom of
// getting it wrong is a camera that is subtly in the wrong place rather than an error.

#ifndef NUKE_SAVER_SCENE_GLSL
#define NUKE_SAVER_SCENE_GLSL

layout(set = 0, binding = 0, std140) uniform Scene {
    mat4 viewProj;
    mat4 invViewProj;

    vec4 cameraPos;     // xyz world, w = seconds since the cycle began
    vec4 keyDirection;  // xyz towards the light, w = body angular radius
    vec4 keyColor;      // rgb radiance (intensity already applied), w = star brightness
    vec4 zenithColor;   // rgb, w = 1 when the body is the moon
    vec4 horizonColor;  // rgb, w = base exposure
    vec4 groundColor;   // rgb, w = ambient scale
    vec4 bodyColor;     // rgb emissive
    vec4 ambientColor;
    vec4 timing;      // x seconds into growth, y board rise
    vec4 boardLight;  // xyz position, w intensity
    vec4 boardColor;  // rgb amber, w falloff radius  // rgb, w = window emission multiplier
} scene;

// Reconstructs the world-space view ray for a pixel from its NDC position. Uses the far plane
// rather than the near one purely for precision: the difference between two nearly identical
// near-plane points is a much worse conditioned subtraction.
vec3 ViewRay(vec2 ndc) {
    vec4 far = scene.invViewProj * vec4(ndc, 1.0, 1.0);
    return normalize(far.xyz / far.w - scene.cameraPos.xyz);
}

#endif
