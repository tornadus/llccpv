#ifndef LLCCPV_CAPTURE_H
#define LLCCPV_CAPTURE_H

#include "util.h"

#include <pthread.h>
#include <stdatomic.h>

#define CAPTURE_NUM_BUFFERS 4

struct capture_buffer {
    void *start;
    size_t length;
};

/* Mailbox slot: holds the latest frame info for the render thread. */
struct capture_mailbox {
    struct frame_info frame;
    _Atomic bool has_frame;
    pthread_mutex_t lock;
};

struct capture_ctx {
    int fd;
    int width;
    int height;
    uint32_t pixfmt;
    struct capture_buffer buffers[CAPTURE_NUM_BUFFERS];
    int num_buffers;
    bool streaming;

    /* Threading */
    pthread_t thread;
    _Atomic bool thread_running;
    struct capture_mailbox mailbox;
    int prev_buf_index; /* buffer held by render thread, to be requeued */
    uint32_t sdl_event_type; /* registered SDL custom event type */
};

/* Open a V4L2 capture device and negotiate format.
 * req_pixfmt: requested pixel format (0 = auto-select best).
 * req_width/req_height: requested resolution (0 = auto-detect from source).
 * Returns 0 on success, -1 on failure. */
int capture_open(struct capture_ctx *ctx, const char *device,
                 uint32_t req_pixfmt, int req_width, int req_height);

/* Set up mmap buffers, start streaming, and launch capture thread.
 * sdl_event_type is a registered SDL custom event type for new-frame signals.
 * Returns 0 on success, -1 on failure. */
int capture_start(struct capture_ctx *ctx, uint32_t sdl_event_type);

/* Get the latest frame from the mailbox (non-blocking).
 * Returns 0 if a new frame is available, -1 if not. */
int capture_get_frame(struct capture_ctx *ctx, struct frame_info *frame);

/* Release the previously held frame buffer back to V4L2. */
void capture_release_frame(struct capture_ctx *ctx);

/* Stop streaming, join thread, and release resources. */
void capture_stop(struct capture_ctx *ctx);

/* Close the device. */
void capture_close(struct capture_ctx *ctx);

#endif
