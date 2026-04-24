#include "ref_yuv.h"
#include <math.h>

static uint8_t unorm8(float f)
{
    if (f <= 0.0f)          return 0;
    if (f >= 1.0f)          return 255;
    int v = (int)lroundf(f * 255.0f);
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

void ref_yuv_to_rgb(uint8_t yy, uint8_t uu, uint8_t vv,
                    enum color_range range, uint8_t out_rgb[3])
{
    float y = yy / 255.0f;
    float u = uu / 255.0f;
    float v = vv / 255.0f;

    if (range == RANGE_LIMITED) {
        y = (y - 16.0f / 255.0f) * (255.0f / 219.0f);
        if (y < 0.0f) y = 0.0f;
        if (y > 1.0f) y = 1.0f;
        u = (u - 128.0f / 255.0f) * (255.0f / 224.0f);
        v = (v - 128.0f / 255.0f) * (255.0f / 224.0f);
    } else {
        u -= 0.5f;
        v -= 0.5f;
    }

    out_rgb[0] = unorm8(y + 1.402f * v);
    out_rgb[1] = unorm8(y - 0.344136f * u - 0.714136f * v);
    out_rgb[2] = unorm8(y + 1.772f * u);
}
