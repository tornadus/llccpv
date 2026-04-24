#ifndef LLCCPV_TEST_REF_YUV_H
#define LLCCPV_TEST_REF_YUV_H

#include <stdint.h>
#include "render.h" /* for enum color_range */

/* BT.601 YUV → RGB8, matching the exact math in shaders/yuyv.frag etc.
 * Inputs y/u/v are unsigned 8-bit (0..255). Output is packed RGB8.
 *
 * Limited range: Y is clamped to [0,1] after range expansion; U/V are NOT
 * clamped (negative values produce valid in-gamut colors). RGB is clamped
 * to [0,255] by the framebuffer's unorm8 storage. */
void ref_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v,
                    enum color_range range, uint8_t out_rgb[3]);

#endif
