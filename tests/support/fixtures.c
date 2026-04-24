#include "fixtures.h"
#include <stddef.h>

size_t fixture_yuyv_size(int w, int h) { return (size_t)w * (size_t)h * 2; }
size_t fixture_uyvy_size(int w, int h) { return (size_t)w * (size_t)h * 2; }
size_t fixture_nv12_size(int w, int h) {
    return (size_t)w * (size_t)h + (size_t)(w / 2) * (size_t)(h / 2) * 2;
}

/* YUYV byte layout per 2-pixel block: Y0 U Y1 V */
void fixture_yuyv_solid(uint8_t *buf, int w, int h, uint8_t y, uint8_t u, uint8_t v)
{
    for (int row = 0; row < h; row++) {
        uint8_t *p = buf + (size_t)row * w * 2;
        for (int c = 0; c < w / 2; c++) {
            p[c * 4 + 0] = y;
            p[c * 4 + 1] = u;
            p[c * 4 + 2] = y;
            p[c * 4 + 3] = v;
        }
    }
}

/* UYVY byte layout per 2-pixel block: U Y0 V Y1 */
void fixture_uyvy_solid(uint8_t *buf, int w, int h, uint8_t y, uint8_t u, uint8_t v)
{
    for (int row = 0; row < h; row++) {
        uint8_t *p = buf + (size_t)row * w * 2;
        for (int c = 0; c < w / 2; c++) {
            p[c * 4 + 0] = u;
            p[c * 4 + 1] = y;
            p[c * 4 + 2] = v;
            p[c * 4 + 3] = y;
        }
    }
}

/* NV12: full-res Y plane, then (w/2 x h/2) interleaved UV plane. */
void fixture_nv12_solid(uint8_t *buf, int w, int h, uint8_t y, uint8_t u, uint8_t v)
{
    for (int i = 0; i < w * h; i++)
        buf[i] = y;
    uint8_t *uv = buf + w * h;
    int uv_pairs = (w / 2) * (h / 2);
    for (int i = 0; i < uv_pairs; i++) {
        uv[i * 2 + 0] = u;
        uv[i * 2 + 1] = v;
    }
}

void fixture_yuyv_chroma_share(uint8_t *buf, int w, int h,
                               uint8_t y_even, uint8_t u,
                               uint8_t y_odd,  uint8_t v)
{
    for (int row = 0; row < h; row++) {
        uint8_t *p = buf + (size_t)row * w * 2;
        for (int c = 0; c < w / 2; c++) {
            p[c * 4 + 0] = y_even;
            p[c * 4 + 1] = u;
            p[c * 4 + 2] = y_odd;
            p[c * 4 + 3] = v;
        }
    }
}

void fixture_yuyv_luma_ramp(uint8_t *buf, int w, int h,
                            int ramp_step, uint8_t u, uint8_t v)
{
    for (int row = 0; row < h; row++) {
        uint8_t *p = buf + (size_t)row * w * 2;
        for (int x = 0; x < w; x++) {
            int yv = x * ramp_step;
            if (yv > 255) yv = 255;
            if (yv < 0)   yv = 0;
            uint8_t Y = (uint8_t)yv;
            /* YUYV packs two pixels per 4 bytes: Y0 U Y1 V. */
            int block = x / 2;
            int is_odd = x & 1;
            p[block * 4 + (is_odd ? 2 : 0)] = Y;
            p[block * 4 + 1] = u;
            p[block * 4 + 3] = v;
        }
    }
}

void fixture_yuyv_vstripes(uint8_t *buf, int w, int h,
                           uint8_t y_even_row, uint8_t y_odd_row)
{
    for (int row = 0; row < h; row++) {
        uint8_t Y = (row & 1) ? y_odd_row : y_even_row;
        uint8_t *p = buf + (size_t)row * w * 2;
        for (int c = 0; c < w / 2; c++) {
            p[c * 4 + 0] = Y;
            p[c * 4 + 1] = 128; /* neutral chroma */
            p[c * 4 + 2] = Y;
            p[c * 4 + 3] = 128;
        }
    }
}
