#ifndef LLCCPV_CAPTURE_MAILBOX_H
#define LLCCPV_CAPTURE_MAILBOX_H

#include "util.h"

#include <pthread.h>
#include <stdatomic.h>

/* Single-slot mailbox: holds the most recent frame for the render thread.
 * Writes replace the previous value; unclaimed frames are dropped. Pure C,
 * no V4L2 — the capture module handles V4L2 side-effects (buffer requeue)
 * around calls to these primitives. */
struct capture_mailbox {
    struct frame_info frame;
    _Atomic bool      has_frame;
    pthread_mutex_t   lock;
};

void capture_mailbox_init(struct capture_mailbox *mb);
void capture_mailbox_destroy(struct capture_mailbox *mb);

/* Publish `f` to the mailbox, replacing any previous unconsumed frame.
 * If a frame was displaced, its buf_index is returned via *displaced_out
 * (set to -1 if nothing was displaced). */
void capture_mailbox_publish(struct capture_mailbox *mb,
                             const struct frame_info *f,
                             int *displaced_out);

/* Non-blocking consume. On success (0), *out holds the frame and the slot
 * is cleared. Returns -1 if the slot was empty. */
int  capture_mailbox_consume(struct capture_mailbox *mb,
                             struct frame_info *out);

/* Drain any pending frame. If one was present, its buf_index goes to
 * *drained_out and the slot is cleared. Returns 0 if drained, -1 if empty. */
int  capture_mailbox_drain(struct capture_mailbox *mb, int *drained_out);

#endif
