#ifndef LLCCPV_RING_H
#define LLCCPV_RING_H

#include <stdatomic.h>
#include <stdint.h>

/* Lock-free single-producer/single-consumer ring buffer for float samples.
 *
 * Sized for ~170 ms at 48 kHz stereo float32 — enough to absorb scheduling
 * jitter between the PipeWire capture and playback callbacks. */

#define RING_SAMPLES (16384)
#define RING_MASK    (RING_SAMPLES - 1)

struct ring_buf {
    float             data[RING_SAMPLES];
    _Atomic uint32_t  read_pos;
    _Atomic uint32_t  write_pos;
};

void ring_write(struct ring_buf *r, const float *src, uint32_t count);

/* Reads up to `count` samples. On overflow (writer lapped reader), the reader
 * snaps forward and the oldest samples are discarded. On underrun (fewer
 * available than requested), the tail of `dst` is zero-filled. */
void ring_read (struct ring_buf *r, float *dst,       uint32_t count);

#endif
