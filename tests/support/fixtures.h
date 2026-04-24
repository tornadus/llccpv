#ifndef LLCCPV_TEST_FIXTURES_H
#define LLCCPV_TEST_FIXTURES_H

#include <stdint.h>
#include <stddef.h>

/* Fill YUYV buffer (w*h*2 bytes, w even) with a uniform (Y, U, V). */
void fixture_yuyv_solid(uint8_t *buf, int w, int h, uint8_t y, uint8_t u, uint8_t v);

/* Fill UYVY buffer (w*h*2 bytes, w even) with a uniform (Y, U, V). */
void fixture_uyvy_solid(uint8_t *buf, int w, int h, uint8_t y, uint8_t u, uint8_t v);

/* Fill NV12 buffer (Y plane w*h followed by UV plane (w/2)*(h/2)*2) uniformly. */
void fixture_nv12_solid(uint8_t *buf, int w, int h, uint8_t y, uint8_t u, uint8_t v);

/* Returns size in bytes for a YUYV/UYVY/NV12 buffer of given dimensions. */
size_t fixture_yuyv_size(int w, int h);
size_t fixture_uyvy_size(int w, int h);
size_t fixture_nv12_size(int w, int h);

/* YUYV: at even column `col_even_x` set Y0 = y_even, at odd column set Y1 = y_odd.
 * UV is shared across each 2-pixel block. Sets every 2-pixel block in every row
 * to (y_even, u, y_odd, v). Tests the fract(pixel_x*0.5) branch in yuyv.frag. */
void fixture_yuyv_chroma_share(uint8_t *buf, int w, int h,
                               uint8_t y_even, uint8_t u,
                               uint8_t y_odd,  uint8_t v);

/* YUYV luma ramp: pixel x gets Y = x*ramp_step (clamped to 255), UV held
 * constant at u/v across the whole image. */
void fixture_yuyv_luma_ramp(uint8_t *buf, int w, int h,
                            int ramp_step, uint8_t u, uint8_t v);

/* YUYV vertical B/W alternation: row 0 white, row 1 black, row 2 white...
 * Used to verify Y-flip: after rendering, row 0 of readback must still be
 * white (top-first order preserved). */
void fixture_yuyv_vstripes(uint8_t *buf, int w, int h,
                           uint8_t y_even_row, uint8_t y_odd_row);

#endif
