#version 450
layout(push_constant) uniform PC { vec2 scale; vec2 offset; } pc;
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_uv;
void main() {
    gl_Position = vec4(in_pos.xy * pc.scale + pc.offset, 0.0, 1.0);
    out_color = in_color;
    out_uv = in_uv;
}
