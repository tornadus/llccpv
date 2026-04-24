#include "ring.h"

void ring_write(struct ring_buf *r, const float *src, uint32_t count)
{
    uint32_t wp = atomic_load(&r->write_pos);
    for (uint32_t i = 0; i < count; i++)
        r->data[(wp + i) & RING_MASK] = src[i];
    atomic_store(&r->write_pos, wp + count);
}

void ring_read(struct ring_buf *r, float *dst, uint32_t count)
{
    uint32_t wp = atomic_load(&r->write_pos);
    uint32_t rp = atomic_load(&r->read_pos);
    uint32_t avail = wp - rp;

    /* If the writer has lapped us, the oldest samples have been overwritten.
     * Snap forward and discard the backlog. SPSC requires that only the
     * reader mutates read_pos, so overflow handling lives here, not in the
     * writer. */
    if (avail > RING_SAMPLES) {
        rp = wp - (RING_SAMPLES - 256);
        avail = wp - rp;
    }

    uint32_t to_copy = count < avail ? count : avail;
    for (uint32_t i = 0; i < to_copy; i++)
        dst[i] = r->data[(rp + i) & RING_MASK];
    for (uint32_t i = to_copy; i < count; i++)
        dst[i] = 0.0f; /* underrun: silence */

    atomic_store(&r->read_pos, rp + to_copy);
}
