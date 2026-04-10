#version 330 core

uniform sampler2D tex_y;
uniform sampler2D tex_uv;
uniform int color_range; /* 0 = limited (TV), 1 = full (PC) */

in vec2 v_uv;
out vec4 frag_color;

void main()
{
    float y_raw = texture(tex_y, v_uv).r;
    float u_raw = texture(tex_uv, v_uv).r;
    float v_raw = texture(tex_uv, v_uv).g;

    float y, u, v;
    if (color_range == 0) {
        y = clamp((y_raw - 16.0/255.0) * (255.0/219.0), 0.0, 1.0);
        u = (u_raw - 128.0/255.0) * (255.0/224.0);
        v = (v_raw - 128.0/255.0) * (255.0/224.0);
    } else {
        y = y_raw;
        u = u_raw - 0.5;
        v = v_raw - 0.5;
    }

    frag_color = vec4(
        y + 1.402 * v,
        y - 0.344136 * u - 0.714136 * v,
        y + 1.772 * u,
        1.0
    );
}
