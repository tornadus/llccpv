#version 330 core

uniform sampler2D tex_yuyv;
uniform vec2 tex_size; /* actual pixel dimensions (width, height) */
uniform int color_range;  /* 0 = limited (TV, 16-235), 1 = full (PC, 0-255) */
uniform int color_matrix; /* 0 = BT.601, 1 = BT.709 */

in vec2 v_uv;
out vec4 frag_color;

void main()
{
    float pixel_x = v_uv.x * tex_size.x;
    vec4 yuyv = texture(tex_yuyv, v_uv);

    float y_raw, u_raw, v_raw;
    u_raw = yuyv.g;
    v_raw = yuyv.a;

    if (fract(pixel_x * 0.5) < 0.5)
        y_raw = yuyv.r;
    else
        y_raw = yuyv.b;

    /* Apply range conversion */
    float y, u, v;
    if (color_range == 0) {
        /* Limited range: Y 16-235, UV 16-240 */
        y = clamp((y_raw - 16.0/255.0) * (255.0/219.0), 0.0, 1.0);
        u = (u_raw - 128.0/255.0) * (255.0/224.0);
        v = (v_raw - 128.0/255.0) * (255.0/224.0);
    } else {
        /* Full range */
        y = y_raw;
        u = u_raw - 0.5;
        v = v_raw - 0.5;
    }

    vec3 rgb;
    if (color_matrix == 1) {
        /* BT.709 YUV -> RGB */
        rgb = vec3(
            y + 1.5748   * v,
            y - 0.187324 * u - 0.468124 * v,
            y + 1.8556   * u
        );
    } else {
        /* BT.601 YUV -> RGB */
        rgb = vec3(
            y + 1.402    * v,
            y - 0.344136 * u - 0.714136 * v,
            y + 1.772    * u
        );
    }
    frag_color = vec4(rgb, 1.0);
}
