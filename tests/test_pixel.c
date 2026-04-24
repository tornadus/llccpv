#include "support/t.h"
#include "support/gl_harness.h"
#include "support/ref_yuv.h"
#include "support/ref_scale.h"
#include "support/fixtures.h"

#include "render.h"
#include "util.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <time.h>
#include <linux/videodev2.h>
#include <string.h>
#include <stdlib.h>

/* ---------- shared helpers ---------- */

#define TOL_CONV   2   /* fp rounding on conversion pass */
#define TOL_STRICT 1   /* boundary values / saturation */

static void fill_rgb_solid(uint8_t *buf, int w, int h,
                           uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < w * h; i++) {
        buf[i * 3 + 0] = r;
        buf[i * 3 + 1] = g;
        buf[i * 3 + 2] = b;
    }
}

/* Run the conversion pass only: upload frame, draw to FBO, read FBO back. */
static int run_conversion(struct render_ctx *ctx,
                          const struct frame_info *frame,
                          uint8_t *out_rgb, int w, int h)
{
    render_upload_frame(ctx, frame);
    glViewport(0, 0, w, h);
    render_draw(ctx, w, h);
    return render_readback_fbo(ctx, out_rgb, w, h);
}

/* Run the full pipeline (both passes) and read the backbuffer. */
static int run_pipeline(struct render_ctx *ctx,
                        const struct frame_info *frame,
                        uint8_t *out_rgb, int out_w, int out_h)
{
    render_upload_frame(ctx, frame);
    glViewport(0, 0, out_w, out_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    render_draw(ctx, out_w, out_h);
    gl_harness_read_backbuffer(out_rgb, out_w, out_h);
    return 0;
}

/* ---------- smoke ---------- */

TEST(smoke_black_yuyv_8x8)
{
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, 8, 8, V4L2_PIX_FMT_YUYV,
                        SCALE_NEAREST, RANGE_FULL, "shaders") == 0,
            "render_init failed");
    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_solid(buf, 8, 8, 0, 128, 128);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = 8, .height = 8, .pixfmt = V4L2_PIX_FMT_YUYV,
    };
    uint8_t out[8 * 8 * 3];
    REQUIRE(run_conversion(&ctx, &f, out, 8, 8) == 0, "readback failed");
    uint8_t expected[8 * 8 * 3] = {0};
    REQUIRE_IMAGE_CLOSE(out, expected, 8, 8, 3, 2);
    render_cleanup(&ctx);
    PASS();
}

/* ---------- YUYV conversion ---------- */

static int check_solid_yuyv(uint8_t y, uint8_t u, uint8_t v,
                            enum color_range range, int tol,
                            const char *label)
{
    const int W = 8, H = 8;
    struct render_ctx ctx;
    if (render_init(&ctx, W, H, V4L2_PIX_FMT_YUYV,
                    SCALE_NEAREST, range, "shaders") != 0) {
        fprintf(stderr, "render_init failed (%s)\n", label);
        return 1;
    }

    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_solid(buf, W, H, y, u, v);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = W, .height = H, .pixfmt = V4L2_PIX_FMT_YUYV,
    };
    uint8_t out[W * H * 3];
    if (run_conversion(&ctx, &f, out, W, H) != 0) {
        render_cleanup(&ctx);
        return 1;
    }

    uint8_t ref[3];
    ref_yuv_to_rgb(y, u, v, range, ref);
    uint8_t expected[W * H * 3];
    fill_rgb_solid(expected, W, H, ref[0], ref[1], ref[2]);

    int rc = t_image_close(out, expected, W, H, 3, tol,
                           __FILE__, __LINE__, label);
    render_cleanup(&ctx);
    return rc;
}

TEST(yuyv_solid_full_colors)
{
    /* Mix of primary colors (many near RGB saturation) and mid-range
     * colors with off-neutral chroma that exercise all four BT.601
     * coefficients without getting swallowed by the [0,255] clamp. */
    struct { uint8_t y, u, v; const char *lbl; } cases[] = {
        {  0, 128, 128, "black_full"         },
        {255, 128, 128, "white_full"         },
        {128, 128, 128, "gray_full"          },
        { 76,  85, 255, "red_full"           },
        {150,  44,  21, "green_full"         },
        { 29, 255, 107, "blue_full"          },
        {226,   0, 149, "yellow_full"        },
        {179, 171,   1, "cyan_full"          },
        { 80, 115, 180, "mid_warm_full"      }, /* unsaturated reddish */
        {170, 180, 100, "mid_cool_full"      }, /* unsaturated cyan-green */
        {128, 128, 200, "gray_with_v_only"   }, /* isolates V-coef on R */
        {128, 200, 128, "gray_with_u_only"   }, /* isolates U-coef on B */
        {140,  70,  90, "mid_dark_chroma"    }, /* both coefs, mid luma */
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
        if (check_solid_yuyv(cases[i].y, cases[i].u, cases[i].v,
                             RANGE_FULL, TOL_CONV, cases[i].lbl) != 0)
            return 1;
    PASS();
}

TEST(yuyv_solid_limited_colors)
{
    struct { uint8_t y, u, v; const char *lbl; } cases[] = {
        { 16, 128, 128, "black_lim" },
        {235, 128, 128, "white_lim" },
        {126, 128, 128, "gray_lim"  },
        { 82,  90, 240, "red_lim"   },
        {145,  54,  34, "green_lim" },
        { 41, 240, 110, "blue_lim"  },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
        if (check_solid_yuyv(cases[i].y, cases[i].u, cases[i].v,
                             RANGE_LIMITED, TOL_CONV, cases[i].lbl) != 0)
            return 1;
    PASS();
}

TEST(yuyv_limited_saturation)
{
    /* Y<16 clamps to black; Y>235 clamps to white, regardless of UV. */
    if (check_solid_yuyv(  0, 128, 128, RANGE_LIMITED, TOL_STRICT, "y0_clamp_black")   != 0) return 1;
    if (check_solid_yuyv(255, 128, 128, RANGE_LIMITED, TOL_STRICT, "y255_clamp_white") != 0) return 1;
    PASS();
}

TEST(yuyv_luma_ramp_limited)
{
    /* Horizontal Y ramp: Y=0,16,32,...,240 across 16 columns. Neutral UV.
     * Validates per-pixel limited-range expansion across the full domain,
     * including clamp regions (Y<16 and Y>235). */
    const int W = 16, H = 2;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, W, H, V4L2_PIX_FMT_YUYV,
                        SCALE_NEAREST, RANGE_LIMITED, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(16, 2)];
    fixture_yuyv_luma_ramp(buf, W, H, 16, 128, 128);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = W, .height = H, .pixfmt = V4L2_PIX_FMT_YUYV,
    };
    uint8_t out[W * H * 3];
    REQUIRE(run_conversion(&ctx, &f, out, W, H) == 0, "readback failed");

    uint8_t expected[W * H * 3];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int yv = x * 16; if (yv > 255) yv = 255;
            uint8_t rgb[3];
            ref_yuv_to_rgb((uint8_t)yv, 128, 128, RANGE_LIMITED, rgb);
            memcpy(&expected[(y*W + x)*3], rgb, 3);
        }
    REQUIRE_IMAGE_CLOSE(out, expected, W, H, 3, TOL_CONV);
    render_cleanup(&ctx);
    PASS();
}

TEST(yuyv_chroma_share)
{
    /* Different Y at even/odd columns, shared UV. Exercises the
     * fract(pixel_x*0.5) < 0.5 branch that selects Y0 vs Y1. */
    const int W = 4, H = 2;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, W, H, V4L2_PIX_FMT_YUYV,
                        SCALE_NEAREST, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(4, 2)];
    fixture_yuyv_chroma_share(buf, W, H, 64, 128, 200, 128);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = W, .height = H, .pixfmt = V4L2_PIX_FMT_YUYV,
    };
    uint8_t out[W * H * 3];
    REQUIRE(run_conversion(&ctx, &f, out, W, H) == 0, "readback failed");

    uint8_t ref_even[3], ref_odd[3];
    ref_yuv_to_rgb( 64, 128, 128, RANGE_FULL, ref_even);
    ref_yuv_to_rgb(200, 128, 128, RANGE_FULL, ref_odd);

    uint8_t expected[W * H * 3];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *r = (x & 1) ? ref_odd : ref_even;
            memcpy(&expected[(y*W + x)*3], r, 3);
        }
    REQUIRE_IMAGE_CLOSE(out, expected, W, H, 3, TOL_CONV);
    render_cleanup(&ctx);
    PASS();
}

TEST(yuyv_y_flip_preserves_row_order)
{
    /* Input row 0 = white, row 1 = black, alternating. After render + top-first
     * readback, row 0 must still be white. */
    const int W = 8, H = 8;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, W, H, V4L2_PIX_FMT_YUYV,
                        SCALE_NEAREST, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_vstripes(buf, W, H, /*even*/255, /*odd*/0);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = W, .height = H, .pixfmt = V4L2_PIX_FMT_YUYV,
    };
    uint8_t out[W * H * 3];
    REQUIRE(run_conversion(&ctx, &f, out, W, H) == 0, "readback failed");

    for (int y = 0; y < H; y++) {
        int bright = (y & 1) == 0;
        for (int x = 0; x < W; x++) {
            int r = out[(y*W + x)*3 + 0];
            int g = out[(y*W + x)*3 + 1];
            int b = out[(y*W + x)*3 + 2];
            if (bright) {
                REQUIRE(r > 240 && g > 240 && b > 240,
                        "row %d expected white, got (%d,%d,%d) x=%d", y, r, g, b, x);
            } else {
                REQUIRE(r < 15 && g < 15 && b < 15,
                        "row %d expected black, got (%d,%d,%d) x=%d", y, r, g, b, x);
            }
        }
    }
    render_cleanup(&ctx);
    PASS();
}

/* ---------- UYVY conversion ---------- */

static int check_solid_uyvy(uint8_t y, uint8_t u, uint8_t v,
                            enum color_range range, int tol,
                            const char *label)
{
    const int W = 8, H = 8;
    struct render_ctx ctx;
    if (render_init(&ctx, W, H, V4L2_PIX_FMT_UYVY,
                    SCALE_NEAREST, range, "shaders") != 0) {
        fprintf(stderr, "render_init failed (%s)\n", label);
        return 1;
    }
    uint8_t buf[fixture_uyvy_size(8, 8)];
    fixture_uyvy_solid(buf, W, H, y, u, v);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = W, .height = H, .pixfmt = V4L2_PIX_FMT_UYVY,
    };
    uint8_t out[W * H * 3];
    if (run_conversion(&ctx, &f, out, W, H) != 0) {
        render_cleanup(&ctx);
        return 1;
    }

    uint8_t ref[3];
    ref_yuv_to_rgb(y, u, v, range, ref);
    uint8_t expected[W * H * 3];
    fill_rgb_solid(expected, W, H, ref[0], ref[1], ref[2]);

    int rc = t_image_close(out, expected, W, H, 3, tol,
                           __FILE__, __LINE__, label);
    render_cleanup(&ctx);
    return rc;
}

TEST(uyvy_solid_colors)
{
    if (check_solid_uyvy(  0, 128, 128, RANGE_FULL,    TOL_CONV,   "uyvy_black_full")  != 0) return 1;
    if (check_solid_uyvy(255, 128, 128, RANGE_FULL,    TOL_CONV,   "uyvy_white_full")  != 0) return 1;
    if (check_solid_uyvy( 76,  85, 255, RANGE_FULL,    TOL_CONV,   "uyvy_red_full")    != 0) return 1;
    if (check_solid_uyvy( 29, 255, 107, RANGE_FULL,    TOL_CONV,   "uyvy_blue_full")   != 0) return 1;
    if (check_solid_uyvy( 16, 128, 128, RANGE_LIMITED, TOL_STRICT, "uyvy_black_lim")   != 0) return 1;
    if (check_solid_uyvy(235, 128, 128, RANGE_LIMITED, TOL_STRICT, "uyvy_white_lim")   != 0) return 1;
    PASS();
}

/* ---------- NV12 conversion ---------- */

static int check_solid_nv12(uint8_t y, uint8_t u, uint8_t v,
                            enum color_range range, int tol,
                            const char *label)
{
    const int W = 8, H = 8;
    struct render_ctx ctx;
    if (render_init(&ctx, W, H, V4L2_PIX_FMT_NV12,
                    SCALE_NEAREST, range, "shaders") != 0) {
        fprintf(stderr, "render_init failed (%s)\n", label);
        return 1;
    }
    uint8_t buf[fixture_nv12_size(8, 8)];
    fixture_nv12_solid(buf, W, H, y, u, v);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = W, .height = H, .pixfmt = V4L2_PIX_FMT_NV12,
    };
    uint8_t out[W * H * 3];
    if (run_conversion(&ctx, &f, out, W, H) != 0) {
        render_cleanup(&ctx);
        return 1;
    }

    uint8_t ref[3];
    ref_yuv_to_rgb(y, u, v, range, ref);
    uint8_t expected[W * H * 3];
    fill_rgb_solid(expected, W, H, ref[0], ref[1], ref[2]);

    int rc = t_image_close(out, expected, W, H, 3, tol,
                           __FILE__, __LINE__, label);
    render_cleanup(&ctx);
    return rc;
}

TEST(nv12_solid_colors)
{
    /* Solid UV makes the bilinear UV-plane filter a no-op — per-pixel test
     * of the BT.601 math across both ranges and a spread of colors. */
    if (check_solid_nv12(  0, 128, 128, RANGE_FULL,    TOL_CONV,   "nv12_black_full")  != 0) return 1;
    if (check_solid_nv12(255, 128, 128, RANGE_FULL,    TOL_CONV,   "nv12_white_full")  != 0) return 1;
    if (check_solid_nv12( 76,  85, 255, RANGE_FULL,    TOL_CONV,   "nv12_red_full")    != 0) return 1;
    if (check_solid_nv12(150,  44,  21, RANGE_FULL,    TOL_CONV,   "nv12_green_full")  != 0) return 1;
    if (check_solid_nv12( 29, 255, 107, RANGE_FULL,    TOL_CONV,   "nv12_blue_full")   != 0) return 1;
    if (check_solid_nv12( 16, 128, 128, RANGE_LIMITED, TOL_STRICT, "nv12_black_lim")   != 0) return 1;
    if (check_solid_nv12(235, 128, 128, RANGE_LIMITED, TOL_STRICT, "nv12_white_lim")   != 0) return 1;
    if (check_solid_nv12(126, 128, 128, RANGE_LIMITED, TOL_CONV,   "nv12_gray_lim")    != 0) return 1;
    PASS();
}

/* ---------- scaling ---------- */

TEST(scale_nearest_identity_bit_exact)
{
    const int W = 8;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, W, W, V4L2_PIX_FMT_YUYV,
                        SCALE_NEAREST, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_chroma_share(buf, W, W, 64, 100, 200, 200);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = W, .height = W, .pixfmt = V4L2_PIX_FMT_YUYV,
    };

    uint8_t conv[W * W * 3], actual[W * W * 3];
    REQUIRE(run_conversion(&ctx, &f, conv, W, W) == 0, "conv failed");
    REQUIRE(run_pipeline(&ctx, &f, actual, W, W) == 0, "pipeline failed");

    REQUIRE_IMAGE_CLOSE(actual, conv, W, W, 3, 0);
    render_cleanup(&ctx);
    PASS();
}

TEST(scale_nearest_2x_upscale_bit_exact)
{
    /* 4x4 → 8x8 nearest upscale. Each input pixel must become a 2x2 output
     * block with identical RGB. */
    const int SRC = 4, OUT = 8;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, SRC, SRC, V4L2_PIX_FMT_YUYV,
                        SCALE_NEAREST, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(4, 4)];
    fixture_yuyv_chroma_share(buf, SRC, SRC, 40, 128, 210, 128);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = SRC, .height = SRC, .pixfmt = V4L2_PIX_FMT_YUYV,
    };

    uint8_t conv[SRC * SRC * 3];
    REQUIRE(run_conversion(&ctx, &f, conv, SRC, SRC) == 0, "conv failed");

    uint8_t expected[OUT * OUT * 3];
    ref_scale_nearest(conv, SRC, SRC, expected, OUT, OUT);

    uint8_t actual[OUT * OUT * 3];
    REQUIRE(run_pipeline(&ctx, &f, actual, OUT, OUT) == 0, "pipeline failed");

    REQUIRE_IMAGE_CLOSE(actual, expected, OUT, OUT, 3, 0);
    render_cleanup(&ctx);
    PASS();
}

TEST(scale_bilinear_identity)
{
    /* At output == source, bilinear samples exactly at texel centers → FBO. */
    const int W = 8;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, W, W, V4L2_PIX_FMT_YUYV,
                        SCALE_BILINEAR, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_chroma_share(buf, W, W, 64, 100, 200, 200);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = W, .height = W, .pixfmt = V4L2_PIX_FMT_YUYV,
    };

    uint8_t conv[W * W * 3], actual[W * W * 3];
    REQUIRE(run_conversion(&ctx, &f, conv, W, W) == 0, "conv failed");
    REQUIRE(run_pipeline(&ctx, &f, actual, W, W) == 0, "pipeline failed");

    REQUIRE_IMAGE_CLOSE(actual, conv, W, W, 3, 2);
    render_cleanup(&ctx);
    PASS();
}

TEST(scale_sharp_bilinear_identity)
{
    const int W = 8;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, W, W, V4L2_PIX_FMT_YUYV,
                        SCALE_SHARP_BILINEAR, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_chroma_share(buf, W, W, 64, 100, 200, 200);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = W, .height = W, .pixfmt = V4L2_PIX_FMT_YUYV,
    };

    uint8_t conv[W * W * 3], actual[W * W * 3];
    REQUIRE(run_conversion(&ctx, &f, conv, W, W) == 0, "conv failed");
    REQUIRE(run_pipeline(&ctx, &f, actual, W, W) == 0, "pipeline failed");

    REQUIRE_IMAGE_CLOSE(actual, conv, W, W, 3, 2);
    render_cleanup(&ctx);
    PASS();
}

TEST(scale_sharp_bilinear_solid_in_solid_out)
{
    const int SRC = 8, OUT = 24;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, SRC, SRC, V4L2_PIX_FMT_YUYV,
                        SCALE_SHARP_BILINEAR, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_solid(buf, SRC, SRC, 128, 128, 128);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = SRC, .height = SRC, .pixfmt = V4L2_PIX_FMT_YUYV,
    };

    uint8_t out[OUT * OUT * 3];
    REQUIRE(run_pipeline(&ctx, &f, out, OUT, OUT) == 0, "pipeline failed");

    uint8_t ref[3];
    ref_yuv_to_rgb(128, 128, 128, RANGE_FULL, ref);
    for (int i = 0; i < OUT * OUT; i++)
        for (int c = 0; c < 3; c++) {
            int d = (int)out[i*3+c] - (int)ref[c];
            if (d < 0) d = -d;
            REQUIRE(d <= 2, "pixel %d ch %d = %d, ref %d",
                    i, c, out[i*3+c], ref[c]);
        }
    render_cleanup(&ctx);
    PASS();
}

/* ---------- FSR invariants ----------
 *
 * FSR output is not bit-exact across GPUs, so we test only what must hold
 * everywhere: dimensions, solid-in/solid-out, no garbage channels, and a
 * monotonic relationship between RCAS sharpness and "softness". */

TEST(fsr_solid_color_stays_solid)
{
    const int SRC = 8, OUT = 32;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, SRC, SRC, V4L2_PIX_FMT_YUYV,
                        SCALE_FSR, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_solid(buf, SRC, SRC, 180, 128, 128); /* light gray */
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = SRC, .height = SRC, .pixfmt = V4L2_PIX_FMT_YUYV,
    };
    uint8_t out[OUT * OUT * 3];
    REQUIRE(run_pipeline(&ctx, &f, out, OUT, OUT) == 0, "pipeline failed");

    uint8_t ref[3];
    ref_yuv_to_rgb(180, 128, 128, RANGE_FULL, ref);
    for (int i = 0; i < OUT * OUT; i++)
        for (int c = 0; c < 3; c++) {
            int d = (int)out[i*3+c] - (int)ref[c];
            if (d < 0) d = -d;
            REQUIRE(d <= 3, "FSR changed solid color at pixel %d ch %d: "
                    "got %d, ref %d", i, c, out[i*3+c], ref[c]);
        }
    render_cleanup(&ctx);
    PASS();
}

TEST(fsr_output_size_matches_viewport)
{
    /* Probe the border of the claimed output region: render into a 24x24
     * viewport on the 64x64 backbuffer. Pixels outside the viewport should
     * remain at the clear color (black); pixels inside should be near gray. */
    const int SRC = 8, OUT = 24;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, SRC, SRC, V4L2_PIX_FMT_YUYV,
                        SCALE_FSR, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_solid(buf, SRC, SRC, 200, 128, 128);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = SRC, .height = SRC, .pixfmt = V4L2_PIX_FMT_YUYV,
    };

    /* Clear full backbuffer to black, render FSR into a 24x24 viewport. */
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    render_upload_frame(&ctx, &f);
    glViewport(0, 0, OUT, OUT);
    render_draw(&ctx, OUT, OUT);

    uint8_t fb[64 * 64 * 3];
    gl_harness_read_backbuffer(fb, 64, 64);

    /* Pixel inside the viewport (center): should be ~gray. */
    int cx = OUT / 2, cy = OUT / 2;
    /* Convert to top-first index: gl_harness_read_backbuffer already flipped. */
    int center_y_top = 64 - 1 - cy;
    const uint8_t *center = &fb[(center_y_top * 64 + cx) * 3];
    REQUIRE(center[0] > 150 && center[1] > 150 && center[2] > 150,
            "viewport interior not rendered: center = (%d,%d,%d)",
            center[0], center[1], center[2]);

    /* Pixel well outside the viewport (50, 50): should still be black. */
    int outside_y_top = 64 - 1 - 50;
    const uint8_t *outside = &fb[(outside_y_top * 64 + 50) * 3];
    REQUIRE(outside[0] < 10 && outside[1] < 10 && outside[2] < 10,
            "FSR wrote outside viewport: (50,50) = (%d,%d,%d)",
            outside[0], outside[1], outside[2]);

    render_cleanup(&ctx);
    PASS();
}

TEST(fsr_rcas_sharpness_monotonic)
{
    /* RCAS softens with higher sharpness values (the uniform is exp2f(-s)).
     * Build a high-contrast edge (alternating columns) and compare the
     * variance (sum of squared column differences) at sharpness 2.0 vs 0.0.
     * At sharpness=2.0 RCAS should be softer → lower variance. */
    const int SRC = 8, OUT = 32;
    struct render_ctx ctx;
    REQUIRE(render_init(&ctx, SRC, SRC, V4L2_PIX_FMT_YUYV,
                        SCALE_FSR, RANGE_FULL, "shaders") == 0,
            "render_init failed");

    uint8_t buf[fixture_yuyv_size(8, 8)];
    fixture_yuyv_chroma_share(buf, SRC, SRC, 30, 128, 225, 128);
    struct frame_info f = {
        .data = buf, .size = sizeof(buf),
        .width = SRC, .height = SRC, .pixfmt = V4L2_PIX_FMT_YUYV,
    };

    uint8_t out_sharp[OUT * OUT * 3], out_soft[OUT * OUT * 3];

    render_set_sharpness(&ctx, 0.0f);
    REQUIRE(run_pipeline(&ctx, &f, out_sharp, OUT, OUT) == 0, "sharp pipeline failed");

    render_set_sharpness(&ctx, 2.0f);
    REQUIRE(run_pipeline(&ctx, &f, out_soft, OUT, OUT) == 0, "soft pipeline failed");

    long long var_sharp = 0, var_soft = 0;
    for (int y = 0; y < OUT; y++) {
        for (int x = 1; x < OUT; x++) {
            for (int c = 0; c < 3; c++) {
                int ds = (int)out_sharp[(y*OUT + x)*3 + c]
                       - (int)out_sharp[(y*OUT + x - 1)*3 + c];
                int dm = (int)out_soft[(y*OUT + x)*3 + c]
                       - (int)out_soft[(y*OUT + x - 1)*3 + c];
                var_sharp += (long long)ds * ds;
                var_soft  += (long long)dm * dm;
            }
        }
    }
    REQUIRE(var_soft < var_sharp,
            "RCAS soft (%lld) not less than sharp (%lld)", var_soft, var_sharp);
    render_cleanup(&ctx);
    PASS();
}

/* ---------- dispatcher ---------- */

typedef struct {
    const char *name;
    int (*fn)(void);
} test_entry_t;

static const test_entry_t tests[] = {
    {"smoke_black_yuyv_8x8",                run_smoke_black_yuyv_8x8},
    {"yuyv_solid_full_colors",              run_yuyv_solid_full_colors},
    {"yuyv_solid_limited_colors",           run_yuyv_solid_limited_colors},
    {"yuyv_limited_saturation",             run_yuyv_limited_saturation},
    {"yuyv_luma_ramp_limited",              run_yuyv_luma_ramp_limited},
    {"yuyv_chroma_share",                   run_yuyv_chroma_share},
    {"yuyv_y_flip_preserves_row_order",     run_yuyv_y_flip_preserves_row_order},
    {"uyvy_solid_colors",                   run_uyvy_solid_colors},
    {"nv12_solid_colors",                   run_nv12_solid_colors},
    {"scale_nearest_identity_bit_exact",    run_scale_nearest_identity_bit_exact},
    {"scale_nearest_2x_upscale_bit_exact",  run_scale_nearest_2x_upscale_bit_exact},
    {"scale_bilinear_identity",             run_scale_bilinear_identity},
    {"scale_sharp_bilinear_identity",       run_scale_sharp_bilinear_identity},
    {"scale_sharp_bilinear_solid_in_solid_out", run_scale_sharp_bilinear_solid_in_solid_out},
    {"fsr_solid_color_stays_solid",         run_fsr_solid_color_stays_solid},
    {"fsr_output_size_matches_viewport",    run_fsr_output_size_matches_viewport},
    {"fsr_rcas_sharpness_monotonic",        run_fsr_rcas_sharpness_monotonic},
};

int main(void)
{
    if (gl_harness_init() < 0)
        return 2;

    int failures = 0;
    size_t n = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < n; i++) {
        fprintf(stderr, "-- %s\n", tests[i].name);
        int rc = tests[i].fn();
        if (rc) {
            fprintf(stderr, "   FAILED\n");
            failures++;
        }
    }

    gl_harness_shutdown();

    fprintf(stderr, "\n%zu test(s), %d failure(s)\n", n, failures);
    return failures ? 1 : 0;
}
