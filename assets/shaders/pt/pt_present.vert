#version 450

layout(location = 0) out vec2 texcoord;

void main() {
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 p = positions[gl_VertexIndex];
    gl_Position = vec4(p, 0.0, 1.0);
    // Native storage images use top-left pixel coordinates.  Flip the
    // Vulkan presentation coordinate once at the fullscreen boundary.
    texcoord = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
}
