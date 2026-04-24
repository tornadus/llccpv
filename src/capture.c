#include "capture.h"

#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <SDL3/SDL.h>

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static int requeue_buffer(struct capture_ctx *ctx, int buf_index)
{
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = buf_index,
    };

    if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("VIDIOC_QBUF failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Capture thread: blocks on poll + DQBUF, publishes to mailbox, signals main thread */
static void *capture_thread_func(void *arg)
{
    struct capture_ctx *ctx = arg;

    while (atomic_load(&ctx->thread_running)) {
        /* Block until a frame is ready (or timeout for clean shutdown) */
        struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 100); /* 100ms timeout for shutdown check */

        if (ret <= 0)
            continue;

        if (!(pfd.revents & POLLIN))
            continue;

        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN)
                continue;
            LOG_ERROR("VIDIOC_DQBUF failed: %s", strerror(errno));
            break;
        }

        /* Write to mailbox: replace the old frame if render hasn't picked it up */
        pthread_mutex_lock(&ctx->mailbox.lock);

        /* If there was an unconsumed frame in the mailbox, requeue its buffer */
        if (atomic_load(&ctx->mailbox.has_frame)) {
            requeue_buffer(ctx, ctx->mailbox.frame.buf_index);
        }

        ctx->mailbox.frame.data = ctx->buffers[buf.index].start;
        ctx->mailbox.frame.size = buf.bytesused;
        ctx->mailbox.frame.width = ctx->width;
        ctx->mailbox.frame.height = ctx->height;
        ctx->mailbox.frame.pixfmt = ctx->pixfmt;
        ctx->mailbox.frame.buf_index = buf.index;
        atomic_store(&ctx->mailbox.has_frame, true);

        pthread_mutex_unlock(&ctx->mailbox.lock);

        /* Signal the main/render thread that a new frame is available */
        SDL_Event event = { .type = ctx->sdl_event_type };
        SDL_PushEvent(&event);
    }

    return NULL;
}

int capture_open(struct capture_ctx *ctx, const char *device,
                 uint32_t req_pixfmt, int req_width, int req_height)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->prev_buf_index = -1;
    pthread_mutex_init(&ctx->mailbox.lock, NULL);

    int fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        LOG_ERROR("Cannot open '%s': %s", device, strerror(errno));
        return -1;
    }

    struct v4l2_capability cap;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("VIDIOC_QUERYCAP failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOG_ERROR("'%s' is not a video capture device", device);
        close(fd);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG_ERROR("'%s' does not support streaming", device);
        close(fd);
        return -1;
    }

    LOG_INFO("Opened capture device: %s (%s)", cap.card, device);

    /* Select pixel format: use requested, or auto-select best */
    uint32_t chosen_fmt = req_pixfmt;
    if (!chosen_fmt) {
        uint32_t preferred[] = {
            V4L2_PIX_FMT_NV12,
            V4L2_PIX_FMT_YUYV,
            V4L2_PIX_FMT_UYVY,
            V4L2_PIX_FMT_MJPEG,
        };
        int best_prio = (int)(sizeof(preferred) / sizeof(preferred[0]));

        struct v4l2_fmtdesc fmtdesc = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        while (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
            LOG_INFO("  Supported format: %s (0x%08x)", fmtdesc.description, fmtdesc.pixelformat);
            for (int i = 0; i < (int)(sizeof(preferred) / sizeof(preferred[0])); i++) {
                if (fmtdesc.pixelformat == preferred[i] && i < best_prio) {
                    best_prio = i;
                    chosen_fmt = preferred[i];
                }
            }
            fmtdesc.index++;
        }
    }

    if (!chosen_fmt) {
        LOG_ERROR("No supported pixel format found on '%s'", device);
        close(fd);
        return -1;
    }

    LOG_INFO("Using pixel format: 0x%08x", chosen_fmt);

    /* Set format and resolution */
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        LOG_ERROR("VIDIOC_G_FMT failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    fmt.fmt.pix.pixelformat = chosen_fmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (req_width > 0 && req_height > 0) {
        fmt.fmt.pix.width = req_width;
        fmt.fmt.pix.height = req_height;
    }

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("VIDIOC_S_FMT failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Re-read to get what the driver actually set */
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        LOG_ERROR("VIDIOC_G_FMT (re-read) failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    ctx->fd = fd;
    ctx->width = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;
    ctx->pixfmt = fmt.fmt.pix.pixelformat;

    LOG_INFO("Capture format: %dx%d, pixfmt=0x%08x", ctx->width, ctx->height, ctx->pixfmt);
    return 0;
}

int capture_start(struct capture_ctx *ctx, uint32_t sdl_event_type)
{
    ctx->sdl_event_type = sdl_event_type;

    struct v4l2_requestbuffers req = {
        .count = CAPTURE_NUM_BUFFERS,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR("VIDIOC_REQBUFS failed: %s", strerror(errno));
        return -1;
    }

    if ((int)req.count < 2) {
        LOG_ERROR("Insufficient buffer memory");
        return -1;
    }

    ctx->num_buffers = (int)req.count;
    LOG_INFO("Allocated %d capture buffers", ctx->num_buffers);

    for (int i = 0; i < ctx->num_buffers; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("VIDIOC_QUERYBUF failed: %s", strerror(errno));
            return -1;
        }

        ctx->buffers[i].length = buf.length;
        ctx->buffers[i].start = mmap(NULL, buf.length,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     ctx->fd, buf.m.offset);

        if (ctx->buffers[i].start == MAP_FAILED) {
            LOG_ERROR("mmap failed: %s", strerror(errno));
            return -1;
        }
    }

    /* Queue all buffers */
    for (int i = 0; i < ctx->num_buffers; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("VIDIOC_QBUF failed: %s", strerror(errno));
            return -1;
        }
    }

    /* Start streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("VIDIOC_STREAMON failed: %s", strerror(errno));
        return -1;
    }

    ctx->streaming = true;

    /* Launch capture thread */
    atomic_store(&ctx->thread_running, true);
    if (pthread_create(&ctx->thread, NULL, capture_thread_func, ctx) != 0) {
        LOG_ERROR("Failed to create capture thread: %s", strerror(errno));
        return -1;
    }

    LOG_INFO("Capture streaming started (threaded)");
    return 0;
}

int capture_get_frame(struct capture_ctx *ctx, struct frame_info *frame)
{
    if (!atomic_load(&ctx->mailbox.has_frame))
        return -1;

    pthread_mutex_lock(&ctx->mailbox.lock);

    if (!atomic_load(&ctx->mailbox.has_frame)) {
        pthread_mutex_unlock(&ctx->mailbox.lock);
        return -1;
    }

    /* Release the buffer the render thread was holding from the previous frame */
    if (ctx->prev_buf_index >= 0)
        requeue_buffer(ctx, ctx->prev_buf_index);

    *frame = ctx->mailbox.frame;
    ctx->prev_buf_index = frame->buf_index;
    atomic_store(&ctx->mailbox.has_frame, false);

    pthread_mutex_unlock(&ctx->mailbox.lock);
    return 0;
}

static void capture_release_frame(struct capture_ctx *ctx)
{
    if (ctx->prev_buf_index >= 0) {
        requeue_buffer(ctx, ctx->prev_buf_index);
        ctx->prev_buf_index = -1;
    }
}

static void capture_stop(struct capture_ctx *ctx)
{
    /* Stop the capture thread */
    if (atomic_load(&ctx->thread_running)) {
        atomic_store(&ctx->thread_running, false);
        pthread_join(ctx->thread, NULL);
    }

    /* Release any held buffer */
    capture_release_frame(ctx);

    /* Drain mailbox */
    pthread_mutex_lock(&ctx->mailbox.lock);
    if (atomic_load(&ctx->mailbox.has_frame)) {
        requeue_buffer(ctx, ctx->mailbox.frame.buf_index);
        atomic_store(&ctx->mailbox.has_frame, false);
    }
    pthread_mutex_unlock(&ctx->mailbox.lock);

    if (ctx->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        ctx->streaming = false;
        LOG_INFO("Capture streaming stopped");
    }

    for (int i = 0; i < ctx->num_buffers; i++) {
        if (ctx->buffers[i].start && ctx->buffers[i].start != MAP_FAILED) {
            munmap(ctx->buffers[i].start, ctx->buffers[i].length);
            ctx->buffers[i].start = NULL;
        }
    }
    ctx->num_buffers = 0;

    pthread_mutex_destroy(&ctx->mailbox.lock);
}

void capture_close(struct capture_ctx *ctx)
{
    capture_stop(ctx);
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}
