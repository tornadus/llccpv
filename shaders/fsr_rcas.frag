#version 430 core

/* FSR 1 RCAS — Robust Contrast-Adaptive Sharpening
 * Thin wrapper around AMD's official FidelityFX FSR 1.0 algorithm.
 * See shaders/ffx/ffx_fsr1.h for the implementation (MIT license).
 *
 * Note: We use v_uv * textureSize instead of gl_FragCoord.xy because
 * the RCAS pass renders to the screen with a viewport offset (letterbox/
 * pillarbox), but the input texture starts at (0,0). */

uniform sampler2D tex_rgb;
uniform uvec4 rcas_con;

in vec2 v_uv;
out vec4 frag_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx/ffx_a.h"

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(tex_rgb, p, 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) { }
#include "ffx/ffx_fsr1.h"

void main()
{
    AF1 r, g, b;
    AU2 pos = AU2(v_uv * vec2(textureSize(tex_rgb, 0)));
    FsrRcasF(r, g, b, pos, rcas_con);
    frag_color = vec4(r, g, b, 1.0);
}
