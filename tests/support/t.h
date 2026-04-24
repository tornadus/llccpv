#ifndef LLCCPV_TEST_T_H
#define LLCCPV_TEST_T_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* A test is just a function returning 0 on pass, 1 on fail. */
#define TEST(name) static int run_##name(void)

#define REQUIRE(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: REQUIRE(%s)\n  ", __FILE__, __LINE__, #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        return 1; \
    } \
} while (0)

#define PASS() do { fprintf(stderr, "ok  %s\n", __func__); return 0; } while (0)

/* Compare two RGB (or RGBA) image buffers pixel-by-pixel with a tolerance.
 * Prints the first failing pixel's coords and both values, then returns 1. */
static inline int t_image_close(const uint8_t *actual, const uint8_t *expected,
                                int w, int h, int channels, int tol,
                                const char *file, int line, const char *tag)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < channels; c++) {
                int ai = actual  [(y * w + x) * channels + c];
                int ei = expected[(y * w + x) * channels + c];
                int d = ai - ei;
                if (d < 0) d = -d;
                if (d > tol) {
                    fprintf(stderr,
                        "FAIL %s:%d: %s mismatch at (%d,%d) ch%d: "
                        "actual=%d expected=%d |d|=%d > tol=%d\n",
                        file, line, tag, x, y, c, ai, ei, d, tol);
                    return 1;
                }
            }
        }
    }
    return 0;
}

#define REQUIRE_IMAGE_CLOSE(actual, expected, w, h, channels, tol) do { \
    if (t_image_close(actual, expected, w, h, channels, tol, \
                      __FILE__, __LINE__, #actual " vs " #expected) != 0) \
        return 1; \
} while (0)

#endif
