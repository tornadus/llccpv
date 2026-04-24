#ifndef LLCCPV_TEST_REF_SCALE_H
#define LLCCPV_TEST_REF_SCALE_H

#include <stdint.h>

/* CPU reference for GL_NEAREST texture filtering + passthrough scaling shader.
 * Both src and dst are packed RGB8, row-major, top-first.
 *
 * Matches the GL 4.5 spec nearest-filter rule: texel index =
 * floor(uv * dim), with fragment UVs at pixel centers. */
void ref_scale_nearest(const uint8_t *src_rgb, int src_w, int src_h,
                       uint8_t *dst_rgb,       int dst_w, int dst_h);

#endif
