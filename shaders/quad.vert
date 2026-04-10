#version 330 core

uniform float y_flip; /* 1.0 = flip Y (for CPU-uploaded textures), 0.0 = no flip (for FBO) */

const vec2 pos[4] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2(-1.0,  1.0),
    vec2( 1.0,  1.0)
);

out vec2 v_uv;

void main()
{
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);

    /* Map clip-space position to UV [0,1] */
    vec2 uv = pos[gl_VertexID] * 0.5 + 0.5;

    /* Flip Y for source textures (row 0 = top in CPU memory, but bottom in GL) */
    if (y_flip > 0.5)
        uv.y = 1.0 - uv.y;

    v_uv = uv;
}
