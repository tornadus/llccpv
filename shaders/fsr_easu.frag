#version 430 core

/* FSR 1 EASU — Edge-Adaptive Spatial Upscaling
 * Thin wrapper around AMD's official FidelityFX FSR 1.0 algorithm.
 * See shaders/ffx/ffx_fsr1.h for the implementation (MIT license). */

uniform sampler2D tex_rgb;
uniform uvec4 easu_con0;
uniform uvec4 easu_con1;
uniform uvec4 easu_con2;
uniform uvec4 easu_con3;

in vec2 v_uv;
out vec4 frag_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx/ffx_a.h"

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return AF4(textureGather(tex_rgb, p, 0)); }
AF4 FsrEasuGF(AF2 p) { return AF4(textureGather(tex_rgb, p, 1)); }
AF4 FsrEasuBF(AF2 p) { return AF4(textureGather(tex_rgb, p, 2)); }
#include "ffx/ffx_fsr1.h"

void main()
{
    AF3 color;
    FsrEasuF(color, AU2(gl_FragCoord.xy), easu_con0, easu_con1, easu_con2, easu_con3);
    frag_color = vec4(color, 1.0);
}
