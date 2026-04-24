#ifndef LLCCPV_CAPTURE_H
#define LLCCPV_CAPTURE_H

#include "util.h"
#include "capture_mailbox.h"

#include <pthread.h>
#include <stdatomic.h>

#define CAPTURE_NUM_BUFFERS 4

struct capture_buffer {
    void *start;
    size_t length;
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
    bool thread_created;             /* set once pthread_create succeeds */
    _Atomic bool thread_running;
    struct capture_mailbox mailbox;
    int prev_buf_index;              /* buffer held by render thread */
    uint32_t frame_event_type;       /* SDL event for a new frame */
    uint32_t reinit_event_type;      /* SDL event for source format change */
};

/* Open a V4L2 capture device and negotiate format.
 * req_pixfmt: requested pixel format (0 = auto-select best).
 * req_width/req_height: requested resolution (0 = auto-detect from source).
 * Returns 0 on success, -1 on failure. */
int capture_open(struct capture_ctx *ctx, const char *device,
                 uint32_t req_pixfmt, int req_width, int req_height);

/* Set up mmap buffers, start streaming, and launch capture thread.
 * frame_event_type: SDL custom event raised when a new frame arrives.
 * reinit_event_type: SDL custom event raised when the source format
 *                    changes (V4L2_EVENT_SOURCE_CHANGE). On receipt the
 *                    main thread should call capture_reinit().
 * Returns 0 on success, -1 on failure. */
int capture_start(struct capture_ctx *ctx,
                  uint32_t frame_event_type,
                  uint32_t reinit_event_type);

/* Get the latest frame from the mailbox (non-blocking).
 * Returns 0 if a new frame is available, -1 if not. */
int capture_get_frame(struct capture_ctx *ctx, struct frame_info *frame);

/* Re-read the source format and rebuild buffers/stream. Call from the
 * main thread after a reinit_event_type is observed. The capture thread
 * has already stopped itself by this point. Updates ctx->width / height
 * / pixfmt to the new values. Returns 0 on success, -1 on failure. */
int capture_reinit(struct capture_ctx *ctx);

/* Stop streaming, join the thread, release resources, and close the device. */
void capture_close(struct capture_ctx *ctx);

#endif
