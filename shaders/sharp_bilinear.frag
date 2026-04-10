#version 330 core

uniform sampler2D tex_rgb;
uniform vec2 src_size;    /* source texture dimensions */
uniform vec2 output_size; /* output viewport dimensions */

in vec2 v_uv;
out vec4 frag_color;

void main()
{
    /* Sharp bilinear: nearest-neighbor within each source texel,
     * with a smooth transition at texel boundaries.
     * Gives sharp pixels with anti-aliased edges when upscaling. */

    vec2 texel = v_uv * src_size;
    vec2 texel_floor = floor(texel);
    vec2 frac = texel - texel_floor;

    /* Compute the size of one source pixel in output pixels */
    vec2 scale = output_size / src_size;

    /* Smoothstep transition zone at texel boundaries.
     * The transition width is 1 output pixel mapped back to source space. */
    vec2 transition = 1.0 / scale;
    vec2 blend = smoothstep(0.5 - transition, 0.5 + transition, frac);

    /* Reconstruct UV from the blended texel position */
    vec2 uv = (texel_floor + 0.5 + blend - 0.5) / src_size;

    frag_color = texture(tex_rgb, uv);
}
