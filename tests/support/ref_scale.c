#include "ref_scale.h"

void ref_scale_nearest(const uint8_t *src_rgb, int src_w, int src_h,
                       uint8_t *dst_rgb,       int dst_w, int dst_h)
{
    for (int oy = 0; oy < dst_h; oy++) {
        int sy = (oy * 2 + 1) * src_h / (dst_h * 2);
        if (sy >= src_h) sy = src_h - 1;
        if (sy < 0)      sy = 0;
        for (int ox = 0; ox < dst_w; ox++) {
            int sx = (ox * 2 + 1) * src_w / (dst_w * 2);
            if (sx >= src_w) sx = src_w - 1;
            if (sx < 0)      sx = 0;
            const uint8_t *s = src_rgb + (sy * src_w + sx) * 3;
            uint8_t *d       = dst_rgb + (oy * dst_w + ox) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    }
}
