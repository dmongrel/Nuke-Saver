#version 450

// Particle shading (spec 8.3).
//
// Only the soft round edge. The colour arrives finished from the vertex stage: an emitter's own
// radiance, or the dust as lit there (particle.vert, DustColor), which is why this includes no
// lighting at all.

layout(location = 0) in vec2  vUv;
layout(location = 1) in vec3  vColor;
layout(location = 2) in float vAlpha;

layout(location = 0) out vec4 outColor;

void main() {
    float r2 = dot(vUv, vUv);
    if (r2 >= 1.0) discard;

    // A soft round falloff rather than a texture. Squared, so the edge is gradual enough that a
    // 60 m dust puff has no visible boundary against the one behind it.
    float soft  = 1.0 - r2;
    float alpha = vAlpha * soft * soft;
    if (alpha <= 0.002) discard;

    outColor = vec4(vColor, alpha);
}
