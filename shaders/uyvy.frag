#version 330 core

uniform sampler2D tex_uyvy;
uniform vec2 tex_size;
uniform int color_range; /* 0 = limited (TV), 1 = full (PC) */

in vec2 v_uv;
out vec4 frag_color;

void main()
{
    float pixel_x = v_uv.x * tex_size.x;
    vec4 uyvy = texture(tex_uyvy, v_uv);

    float y_raw, u_raw, v_raw;
    u_raw = uyvy.r;
    v_raw = uyvy.b;

    if (fract(pixel_x * 0.5) < 0.25)
        y_raw = uyvy.g;
    else
        y_raw = uyvy.a;

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
