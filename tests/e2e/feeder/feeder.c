/*
 * feeder.c — V4L2 frame generator for llccpv e2e tests.
 *
 * Opens /dev/videoN (expected to be a v4l2loopback sink), declares a
 * format via VIDIOC_S_FMT, then writes synthesized frames at a target
 * fps. Supports YUYV, UYVY, NV12. Patterns: solid color, horizontal
 * Y-ramp, SMPTE-like bars, checker.
 *
 * Edge-case hooks (scheduled by frame number):
 *   --pause-at    N:SEC       stop writing for SEC seconds after frame N
 *   --rate-at     N:HZ        switch fps at frame N
 *   --malform-at  N:short|junk write a truncated or random frame at N
 *   --resize-at   N:WxH       STREAMOFF → S_FMT → STREAMON at N (for the
 *                             V4L2_EVENT_SOURCE_CHANGE test path)
 *   --reopen-at   N:SEC       close fd, sleep SEC, reopen at frame N
 *
 * Stderr log lines (one per frame, one per event) use a fixed shape:
 *   feeder: t=<ms> frame=<N> action=<verb>
 * so the Python test runner can grep for specific transitions.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

/* ---------- logging ---------- */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t g_t0_ms;

#define EVT(frame, verb, ...) do { \
    fprintf(stderr, "feeder: t=%lld frame=%d action=" verb "\n", \
            (long long)(now_ms() - g_t0_ms), (frame), ##__VA_ARGS__); \
    fflush(stderr); \
} while (0)

static void die(const char *msg)
{
    fprintf(stderr, "feeder: fatal: %s: %s\n", msg, strerror(errno));
    exit(2);
}

/* ---------- BT.601 RGB → YUV (full-range) ---------- */

static void rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b,
                       uint8_t *y, uint8_t *u, uint8_t *v)
{
    float fr = r / 255.0f, fg = g / 255.0f, fb = b / 255.0f;
    float fy =  0.299f * fr + 0.587f * fg + 0.114f * fb;
    float fu = (fb - fy) / 1.772f + 0.5f;
    float fv = (fr - fy) / 1.402f + 0.5f;
    if (fy < 0) fy = 0;
    if (fy > 1) fy = 1;
    if (fu < 0) fu = 0;
    if (fu > 1) fu = 1;
    if (fv < 0) fv = 0;
    if (fv > 1) fv = 1;
    *y = (uint8_t)(fy * 255.0f + 0.5f);
    *u = (uint8_t)(fu * 255.0f + 0.5f);
    *v = (uint8_t)(fv * 255.0f + 0.5f);
}

/* ---------- pattern generators ---------- */

enum pattern { PAT_SOLID, PAT_GRADIENT, PAT_BARS, PAT_CHECKER };

struct params {
    enum pattern pat;
    uint8_t r, g, b; /* solid */
};

static size_t frame_size(uint32_t pixfmt, int w, int h)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY: return (size_t)w * h * 2;
    case V4L2_PIX_FMT_NV12: return (size_t)w * h * 3 / 2;
    case V4L2_PIX_FMT_MJPEG: return (size_t)w * h; /* placeholder, see mjpeg note */
    }
    return 0;
}

/* SMPTE-like 8-bar palette (white, yellow, cyan, green, magenta, red, blue, black) */
static const uint8_t BARS_RGB[8][3] = {
    {255, 255, 255}, {255, 255,   0}, {  0, 255, 255}, {  0, 255,   0},
    {255,   0, 255}, {255,   0,   0}, {  0,   0, 255}, {  0,   0,   0},
};

/* Fill buffer with YUV encoded via given pattern. For solid: y,u,v are passed
 * directly. For gradient: horizontal Y-ramp, neutral UV. For bars/checker:
 * computed per pixel. */
static void fill_frame(uint8_t *buf, uint32_t pixfmt, int w, int h,
                       const struct params *p)
{
    /* Packed YUYV/UYVY layouts share a 4-byte-per-2-pixel shape. */
    uint8_t y, u, v;

    if (pixfmt == V4L2_PIX_FMT_YUYV || pixfmt == V4L2_PIX_FMT_UYVY) {
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col += 2) {
                uint8_t y0, y1, uu, vv;
                switch (p->pat) {
                case PAT_SOLID:
                    rgb_to_yuv(p->r, p->g, p->b, &y, &u, &v);
                    y0 = y1 = y; uu = u; vv = v;
                    break;
                case PAT_GRADIENT: {
                    uint8_t yv0 = (uint8_t)(col * 255 / (w - 1));
                    uint8_t yv1 = (uint8_t)((col + 1) * 255 / (w - 1));
                    y0 = yv0; y1 = yv1; uu = 128; vv = 128;
                    break;
                }
                case PAT_BARS: {
                    int bar = (col * 8) / w;
                    if (bar > 7) bar = 7;
                    rgb_to_yuv(BARS_RGB[bar][0], BARS_RGB[bar][1],
                               BARS_RGB[bar][2], &y, &u, &v);
                    y0 = y1 = y; uu = u; vv = v;
                    break;
                }
                case PAT_CHECKER: {
                    int cell = ((col / 8) + (row / 8)) & 1;
                    uint8_t shade = cell ? 230 : 30;
                    rgb_to_yuv(shade, shade, shade, &y, &u, &v);
                    y0 = y1 = y; uu = u; vv = v;
                    break;
                }
                default:
                    y0 = y1 = 0; uu = vv = 128;
                }

                uint8_t *p4 = buf + ((size_t)row * w + col) * 2;
                if (pixfmt == V4L2_PIX_FMT_YUYV) {
                    p4[0] = y0; p4[1] = uu; p4[2] = y1; p4[3] = vv;
                } else { /* UYVY */
                    p4[0] = uu; p4[1] = y0; p4[2] = vv; p4[3] = y1;
                }
            }
        }
        return;
    }

    if (pixfmt == V4L2_PIX_FMT_NV12) {
        uint8_t *yplane = buf;
        uint8_t *uvplane = buf + (size_t)w * h;
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                switch (p->pat) {
                case PAT_SOLID:
                    rgb_to_yuv(p->r, p->g, p->b, &y, &u, &v); break;
                case PAT_GRADIENT:
                    y = (uint8_t)(col * 255 / (w - 1)); u = v = 128; break;
                case PAT_BARS: {
                    int bar = (col * 8) / w; if (bar > 7) bar = 7;
                    rgb_to_yuv(BARS_RGB[bar][0], BARS_RGB[bar][1],
                               BARS_RGB[bar][2], &y, &u, &v);
                    break;
                }
                case PAT_CHECKER: {
                    int cell = ((col / 8) + (row / 8)) & 1;
                    uint8_t shade = cell ? 230 : 30;
                    rgb_to_yuv(shade, shade, shade, &y, &u, &v);
                    break;
                }
                default: y = 0; u = v = 128;
                }
                yplane[(size_t)row * w + col] = y;
                if ((row & 1) == 0 && (col & 1) == 0) {
                    size_t uvidx = ((size_t)(row / 2) * (w / 2) + (col / 2)) * 2;
                    uvplane[uvidx + 0] = u;
                    uvplane[uvidx + 1] = v;
                }
            }
        }
        return;
    }
}

/* ---------- scheduled events ---------- */

struct schedule {
    int   pause_at_frame;
    double pause_seconds;
    int   rate_at_frame;
    double rate_hz;
    int   malform_at_frame;
    char  malform_kind[16]; /* "short" or "junk" */
    int   resize_at_frame;
    int   resize_w, resize_h;
    int   reopen_at_frame;
    double reopen_seconds;
};

static void parse_colon(const char *s, int *a, double *b)
{
    const char *colon = strchr(s, ':');
    if (!colon) { fprintf(stderr, "feeder: bad arg '%s' (expected N:VAL)\n", s); exit(2); }
    *a = atoi(s);
    *b = atof(colon + 1);
}

static void parse_malform(const char *s, int *frame, char *kind, size_t klen)
{
    const char *colon = strchr(s, ':');
    if (!colon) { fprintf(stderr, "feeder: bad --malform-at '%s'\n", s); exit(2); }
    *frame = atoi(s);
    snprintf(kind, klen, "%s", colon + 1);
}

static void parse_resize(const char *s, int *frame, int *w, int *h)
{
    const char *colon = strchr(s, ':');
    const char *x     = colon ? strchr(colon + 1, 'x') : NULL;
    if (!colon || !x) { fprintf(stderr, "feeder: bad --resize-at '%s' (expected N:WxH)\n", s); exit(2); }
    *frame = atoi(s);
    *w = atoi(colon + 1);
    *h = atoi(x + 1);
}

/* ---------- device I/O ---------- */

static int open_and_configure(const char *path, uint32_t pixfmt, int w, int h)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) die(path);

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .fmt.pix = {
            .width = w, .height = h,
            .pixelformat = pixfmt,
            .field = V4L2_FIELD_NONE,
        }
    };
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) die("VIDIOC_S_FMT");
    return fd;
}

/* ---------- main ---------- */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s --device PATH --format FMT --width W --height H [options]\n"
        "  --device PATH       v4l2loopback sink (e.g. /dev/video42)\n"
        "  --format FMT        yuyv | uyvy | nv12 | mjpeg (mjpeg: declares only)\n"
        "  --width W --height H\n"
        "  --fps HZ            default 30\n"
        "  --duration SEC      default 2.0\n"
        "  --frames N          alternate to --duration (exact frame count)\n"
        "  --pattern P         solid:R,G,B | gradient | bars | checker (default gradient)\n"
        "  --pause-at N:SEC    pause writing for SEC seconds after frame N\n"
        "  --rate-at  N:HZ     switch fps at frame N\n"
        "  --malform-at N:KIND short or junk (single malformed write at frame N)\n"
        "  --resize-at N:WxH   (unimplemented hook — V4L2 output resize is fiddly)\n"
        "  --reopen-at N:SEC   close fd for SEC seconds, then reopen\n",
        argv0);
}

int main(int argc, char *argv[])
{
    g_t0_ms = now_ms();

    const char *device = NULL;
    uint32_t pixfmt = 0;
    int W = 0, H = 0;
    double fps = 30.0;
    double duration = 2.0;
    int frames_exact = 0;
    struct params pat = {.pat = PAT_GRADIENT};
    struct schedule sched = {
        .pause_at_frame = -1, .rate_at_frame = -1,
        .malform_at_frame = -1, .resize_at_frame = -1,
        .reopen_at_frame = -1,
    };

    enum {
        OPT_DEVICE = 256, OPT_FORMAT, OPT_WIDTH, OPT_HEIGHT,
        OPT_FPS, OPT_DURATION, OPT_FRAMES, OPT_PATTERN,
        OPT_PAUSE_AT, OPT_RATE_AT, OPT_MALFORM_AT, OPT_RESIZE_AT,
        OPT_REOPEN_AT, OPT_HELP,
    };
    static struct option longs[] = {
        {"device",      required_argument, NULL, OPT_DEVICE},
        {"format",      required_argument, NULL, OPT_FORMAT},
        {"width",       required_argument, NULL, OPT_WIDTH},
        {"height",      required_argument, NULL, OPT_HEIGHT},
        {"fps",         required_argument, NULL, OPT_FPS},
        {"duration",    required_argument, NULL, OPT_DURATION},
        {"frames",      required_argument, NULL, OPT_FRAMES},
        {"pattern",     required_argument, NULL, OPT_PATTERN},
        {"pause-at",    required_argument, NULL, OPT_PAUSE_AT},
        {"rate-at",     required_argument, NULL, OPT_RATE_AT},
        {"malform-at",  required_argument, NULL, OPT_MALFORM_AT},
        {"resize-at",   required_argument, NULL, OPT_RESIZE_AT},
        {"reopen-at",   required_argument, NULL, OPT_REOPEN_AT},
        {"help",        no_argument,       NULL, OPT_HELP},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "", longs, NULL)) != -1) {
        switch (c) {
        case OPT_DEVICE: device = optarg; break;
        case OPT_FORMAT:
            if      (strcmp(optarg, "yuyv") == 0) pixfmt = V4L2_PIX_FMT_YUYV;
            else if (strcmp(optarg, "uyvy") == 0) pixfmt = V4L2_PIX_FMT_UYVY;
            else if (strcmp(optarg, "nv12") == 0) pixfmt = V4L2_PIX_FMT_NV12;
            else if (strcmp(optarg, "mjpeg") == 0) pixfmt = V4L2_PIX_FMT_MJPEG;
            else { fprintf(stderr, "feeder: unknown format: %s\n", optarg); return 2; }
            break;
        case OPT_WIDTH:    W = atoi(optarg); break;
        case OPT_HEIGHT:   H = atoi(optarg); break;
        case OPT_FPS:      fps = atof(optarg); break;
        case OPT_DURATION: duration = atof(optarg); break;
        case OPT_FRAMES:   frames_exact = atoi(optarg); break;
        case OPT_PATTERN:
            if (strncmp(optarg, "solid:", 6) == 0) {
                int r, g, b;
                if (sscanf(optarg + 6, "%d,%d,%d", &r, &g, &b) != 3) {
                    fprintf(stderr, "feeder: bad solid:R,G,B: %s\n", optarg); return 2;
                }
                pat.pat = PAT_SOLID;
                pat.r = (uint8_t)r; pat.g = (uint8_t)g; pat.b = (uint8_t)b;
            } else if (strcmp(optarg, "gradient") == 0) pat.pat = PAT_GRADIENT;
            else if (strcmp(optarg, "bars") == 0)       pat.pat = PAT_BARS;
            else if (strcmp(optarg, "checker") == 0)    pat.pat = PAT_CHECKER;
            else { fprintf(stderr, "feeder: unknown pattern: %s\n", optarg); return 2; }
            break;
        case OPT_PAUSE_AT:   parse_colon(optarg, &sched.pause_at_frame, &sched.pause_seconds); break;
        case OPT_RATE_AT:    parse_colon(optarg, &sched.rate_at_frame, &sched.rate_hz); break;
        case OPT_MALFORM_AT: parse_malform(optarg, &sched.malform_at_frame, sched.malform_kind, sizeof(sched.malform_kind)); break;
        case OPT_RESIZE_AT:  parse_resize(optarg, &sched.resize_at_frame, &sched.resize_w, &sched.resize_h); break;
        case OPT_REOPEN_AT:  parse_colon(optarg, &sched.reopen_at_frame, &sched.reopen_seconds); break;
        case OPT_HELP: usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }

    if (!device || !pixfmt || !W || !H) {
        usage(argv[0]); return 2;
    }

    int fd = open_and_configure(device, pixfmt, W, H);
    EVT(0, "opened device=%s fmt=0x%08x res=%dx%d", device, pixfmt, W, H);

    size_t fsize = frame_size(pixfmt, W, H);
    uint8_t *frame_buf = malloc(fsize);
    if (!frame_buf) die("malloc");

    int total_frames = frames_exact > 0 ? frames_exact
                                        : (int)(duration * fps + 0.5);
    double period_s = 1.0 / fps;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    uint32_t junk_seed = 0xC0FFEE;

    for (int n = 0; n < total_frames; n++) {
        /* Scheduled events */
        if (n == sched.rate_at_frame) {
            fps = sched.rate_hz;
            period_s = 1.0 / fps;
            EVT(n, "rate_change fps=%.1f", fps);
        }
        if (n == sched.pause_at_frame) {
            EVT(n, "pause_start sec=%.1f", sched.pause_seconds);
            struct timespec ps = {
                .tv_sec = (time_t)sched.pause_seconds,
                .tv_nsec = (long)((sched.pause_seconds - (time_t)sched.pause_seconds) * 1e9)
            };
            nanosleep(&ps, NULL);
            EVT(n, "pause_end");
            /* Re-anchor the deadline so we don't burst after the pause. */
            clock_gettime(CLOCK_MONOTONIC, &deadline);
        }
        if (n == sched.reopen_at_frame) {
            EVT(n, "reopen_close");
            close(fd);
            struct timespec rs = {
                .tv_sec = (time_t)sched.reopen_seconds,
                .tv_nsec = (long)((sched.reopen_seconds - (time_t)sched.reopen_seconds) * 1e9)
            };
            nanosleep(&rs, NULL);
            fd = open_and_configure(device, pixfmt, W, H);
            EVT(n, "reopen_done");
            clock_gettime(CLOCK_MONOTONIC, &deadline);
        }

        /* Build and write the frame. */
        fill_frame(frame_buf, pixfmt, W, H, &pat);

        ssize_t wrote;
        if (n == sched.malform_at_frame && strcmp(sched.malform_kind, "short") == 0) {
            wrote = write(fd, frame_buf, fsize / 2);
            EVT(n, "malform_short bytes=%zd", wrote);
        } else if (n == sched.malform_at_frame && strcmp(sched.malform_kind, "junk") == 0) {
            for (size_t i = 0; i < fsize; i++) {
                junk_seed = junk_seed * 1664525u + 1013904223u;
                frame_buf[i] = (uint8_t)(junk_seed >> 24);
            }
            wrote = write(fd, frame_buf, fsize);
            EVT(n, "malform_junk bytes=%zd", wrote);
        } else {
            wrote = write(fd, frame_buf, fsize);
        }

        if (wrote < 0) {
            fprintf(stderr, "feeder: write failed at frame %d: %s\n", n, strerror(errno));
            /* Keep going — v4l2loopback may reject some frames during
             * resize / reopen events; soldier on rather than fail fast. */
        }

        if ((n & 31) == 0)
            EVT(n, "wrote bytes=%zd fps=%.1f", wrote, fps);

        /* Pace to the deadline. */
        deadline.tv_nsec += (long)(period_s * 1e9);
        while (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }

    EVT(total_frames, "done total=%d", total_frames);
    free(frame_buf);
    close(fd);
    return 0;
}
