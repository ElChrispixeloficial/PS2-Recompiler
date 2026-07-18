#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec4 frag_color;
void main() {
    frag_color = in_color * texture(tex, in_uv);
}
