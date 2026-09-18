#version 450

// Fullscreen triangle for the sky pass. Emits NDC rather than a UV because the fragment shader
// unprojects it into a world-space view ray; a UV would only have to be converted back.
//
// On the far plane, so that with the depth test on it lands only where nothing opaque was drawn:
// the sky pass runs after the opaque geometry and never shades a pixel something will cover.

layout(location = 0) out vec2 vNdc;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vNdc = uv * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);
}
