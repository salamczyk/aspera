#version 450

layout(push_constant) uniform PtImGuiPush { mat4 projection; } imgui;
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 0) out vec2 texcoord;
layout(location = 1) out vec4 vertexColor;

void main() {
    gl_Position = imgui.projection * vec4(position, 0.0, 1.0);
    texcoord = uv;
    vertexColor = color;
}
