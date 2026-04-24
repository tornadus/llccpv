#include "support/t.h"
#include "ring.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- single-threaded correctness ---------- */

TEST(roundtrip_basic)
{
    struct ring_buf r;
    memset(&r, 0, sizeof(r));

    float in[128], out[128];
    for (int i = 0; i < 128; i++) in[i] = (float)(i + 1);

    ring_write(&r, in, 128);
    ring_read(&r, out, 128);

    for (int i = 0; i < 128; i++)
        REQUIRE(out[i] == in[i], "out[%d]=%f expected %f", i, out[i], in[i]);
    PASS();
}

TEST(underrun_zero_fills)
{
    struct ring_buf r;
    memset(&r, 0, sizeof(r));

    float in[10];
    for (int i = 0; i < 10; i++) in[i] = (float)(i + 1);
    ring_write(&r, in, 10);

    float out[20];
    for (int i = 0; i < 20; i++) out[i] = -1.0f; /* sentinel */
    ring_read(&r, out, 20);

    for (int i = 0; i < 10; i++)
        REQUIRE(out[i] == in[i], "in-range out[%d]=%f expected %f",
                i, out[i], in[i]);
    for (int i = 10; i < 20; i++)
        REQUIRE(out[i] == 0.0f, "underrun slot %d = %f, expected 0", i, out[i]);
    PASS();
}

TEST(wraparound_crosses_mask_boundary)
{
    struct ring_buf r;
    memset(&r, 0, sizeof(r));

    /* Drive write_pos near the end of the ring so that the next write crosses
     * the mask boundary. Do this by writing and reading RING_SAMPLES-100
     * samples to advance both pointers without actually wrapping. */
    float scratch[RING_SAMPLES];
    for (int i = 0; i < RING_SAMPLES; i++) scratch[i] = (float)i;

    uint32_t initial_advance = RING_SAMPLES - 100;
    ring_write(&r, scratch, initial_advance);
    float sink[RING_SAMPLES];
    ring_read(&r, sink, initial_advance);

    /* Now write 200 distinct values — this crosses the RING_MASK boundary. */
    float in[200];
    for (int i = 0; i < 200; i++) in[i] = 10000.0f + (float)i;
    ring_write(&r, in, 200);

    float out[200];
    ring_read(&r, out, 200);
    for (int i = 0; i < 200; i++)
        REQUIRE(out[i] == in[i], "wrap slot %d: got %f, expected %f",
                i, out[i], in[i]);
    PASS();
}

TEST(overflow_snap_forward)
{
    /* Write RING_SAMPLES + 500 without reading. The reader snaps forward,
     * discarding the oldest 500+256 samples, and reads (RING_SAMPLES - 256). */
    struct ring_buf r;
    memset(&r, 0, sizeof(r));

    uint32_t total = RING_SAMPLES + 500;
    float *in = malloc(total * sizeof(float));
    for (uint32_t i = 0; i < total; i++) in[i] = (float)(i + 1);

    /* ring_write writes in batches that fit in the buffer — an SPSC writer
     * is not expected to write more than RING_SAMPLES in one go. Do it in
     * ~1024-sample chunks. */
    uint32_t pos = 0;
    while (pos < total) {
        uint32_t chunk = total - pos;
        if (chunk > 1024) chunk = 1024;
        ring_write(&r, in + pos, chunk);
        pos += chunk;
    }

    float *out = malloc(RING_SAMPLES * sizeof(float));
    ring_read(&r, out, RING_SAMPLES);

    /* The snap keeps the most recent (RING_SAMPLES - 256) samples; the rest
     * of the read buffer gets zero-fill from underrun. */
    uint32_t kept = RING_SAMPLES - 256;
    uint32_t first_kept_value = total - kept; /* values are 1-indexed shifted */
    for (uint32_t i = 0; i < kept; i++) {
        float expected = (float)(first_kept_value + i + 1);
        REQUIRE(out[i] == expected,
                "snap slot %u: got %f, expected %f", i, out[i], expected);
    }
    for (uint32_t i = kept; i < RING_SAMPLES; i++)
        REQUIRE(out[i] == 0.0f, "post-snap underrun %u = %f, expected 0",
                i, out[i]);

    free(in);
    free(out);
    PASS();
}

/* ---------- two-thread soak ---------- *
 *
 * Production use: PipeWire calls the capture and playback process callbacks
 * at matching rates (~480 samples each, every ~10ms). The ring never fills
 * in that regime. This test models that: the writer backs off when the ring
 * is half-full, so overflow-snap is never triggered. Under those conditions
 * the reader must see a strictly monotonically increasing sequence of
 * non-zero values (no out-of-order). Overflow-snap itself is covered by the
 * single-threaded overflow_snap_forward test. */

struct soak_state {
    struct ring_buf *ring;
    volatile int writer_done;
    uint64_t n_written;
    uint64_t n_out_of_order;
    uint64_t n_nonzero_read;
    uint64_t total_to_write;
};

static void *soak_writer(void *arg)
{
    struct soak_state *s = arg;
    uint64_t counter = 0;
    uint32_t seed = 0xC0FFEE;
    while (counter < s->total_to_write) {
        /* Back off if the ring is half-full — keeps us out of the overflow
         * regime the audio passthrough is designed to avoid. */
        uint32_t wp = atomic_load(&s->ring->write_pos);
        uint32_t rp = atomic_load(&s->ring->read_pos);
        if ((uint32_t)(wp - rp) > RING_SAMPLES / 2) {
            sched_yield();
            continue;
        }
        uint32_t chunk = (seed % 512) + 1;
        seed = seed * 1664525u + 1013904223u;
        if (counter + chunk > s->total_to_write)
            chunk = (uint32_t)(s->total_to_write - counter);
        float buf[512];
        for (uint32_t i = 0; i < chunk; i++)
            buf[i] = (float)(++counter);
        ring_write(s->ring, buf, chunk);
    }
    s->n_written = counter;
    atomic_signal_fence(memory_order_seq_cst);
    s->writer_done = 1;
    return NULL;
}

static void *soak_reader(void *arg)
{
    struct soak_state *s = arg;
    uint32_t seed = 0xDEADBEEF;
    float prev = 0.0f;
    for (;;) {
        uint32_t chunk = (seed % 512) + 1;
        seed = seed * 1103515245u + 12345u;
        float buf[512];
        ring_read(s->ring, buf, chunk);
        for (uint32_t i = 0; i < chunk; i++) {
            float v = buf[i];
            if (v == 0.0f) continue;
            s->n_nonzero_read++;
            if (v <= prev) s->n_out_of_order++;
            prev = v;
        }
        /* Exit when writer is done AND ring is drained. */
        if (s->writer_done) {
            uint32_t wp = atomic_load(&s->ring->write_pos);
            uint32_t rp = atomic_load(&s->ring->read_pos);
            if (wp == rp) break;
        }
    }
    return NULL;
}

TEST(soak_two_thread_monotonic)
{
    struct ring_buf *r = calloc(1, sizeof(*r));
    struct soak_state state = {.ring = r, .total_to_write = 2000000};

    pthread_t w, rd;
    pthread_create(&w,  NULL, soak_writer, &state);
    pthread_create(&rd, NULL, soak_reader, &state);

    pthread_join(w,  NULL);
    pthread_join(rd, NULL);

    fprintf(stderr, "   soak: wrote=%lu read=%lu out-of-order=%lu\n",
            (unsigned long)state.n_written,
            (unsigned long)state.n_nonzero_read,
            (unsigned long)state.n_out_of_order);

    REQUIRE(state.n_out_of_order == 0,
            "reader observed %lu out-of-order samples",
            (unsigned long)state.n_out_of_order);
    REQUIRE(state.n_nonzero_read == state.n_written,
            "read %lu samples, expected %lu",
            (unsigned long)state.n_nonzero_read,
            (unsigned long)state.n_written);

    free(r);
    PASS();
}

/* ---------- dispatcher ---------- */

typedef struct { const char *name; int (*fn)(void); } test_entry_t;

static const test_entry_t tests[] = {
    {"roundtrip_basic",             run_roundtrip_basic},
    {"underrun_zero_fills",         run_underrun_zero_fills},
    {"wraparound_crosses_mask_boundary", run_wraparound_crosses_mask_boundary},
    {"overflow_snap_forward",       run_overflow_snap_forward},
    {"soak_two_thread_monotonic",   run_soak_two_thread_monotonic},
};

int main(void)
{
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
    fprintf(stderr, "\n%zu test(s), %d failure(s)\n", n, failures);
    return failures ? 1 : 0;
}
