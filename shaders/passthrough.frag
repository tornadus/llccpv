#version 330 core

uniform sampler2D tex_rgb;

in vec2 v_uv;
out vec4 frag_color;

void main()
{
    frag_color = texture(tex_rgb, v_uv);
}
