#include "support/t.h"
#include "capture_mailbox.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- single-threaded correctness ---------- */

static struct frame_info make_frame(int buf_index, int width)
{
    struct frame_info f = {
        .data = (void *)(uintptr_t)(buf_index * 0x10000), /* unique sentinel */
        .size = 1024,
        .width = width,
        .height = 480,
        .pixfmt = 0x56595559, /* 'YUYV' */
        .buf_index = buf_index,
    };
    return f;
}

TEST(publish_then_consume)
{
    struct capture_mailbox mb;
    capture_mailbox_init(&mb);

    struct frame_info in = make_frame(2, 1920);
    int displaced = 42;
    capture_mailbox_publish(&mb, &in, &displaced);
    REQUIRE(displaced == -1, "first publish displaced %d, expected -1", displaced);

    struct frame_info out;
    REQUIRE(capture_mailbox_consume(&mb, &out) == 0, "consume after publish failed");
    REQUIRE(out.buf_index == 2, "buf_index mismatch: got %d", out.buf_index);
    REQUIRE(out.width     == 1920, "width mismatch");
    REQUIRE(out.data      == in.data, "data ptr mismatch");

    /* Second consume must return -1 (slot drained). */
    REQUIRE(capture_mailbox_consume(&mb, &out) == -1,
            "second consume did not return -1");

    capture_mailbox_destroy(&mb);
    PASS();
}

TEST(double_publish_displaces_older)
{
    struct capture_mailbox mb;
    capture_mailbox_init(&mb);

    struct frame_info a = make_frame(1, 1280);
    struct frame_info b = make_frame(3, 1280);

    int d1 = -99, d2 = -99;
    capture_mailbox_publish(&mb, &a, &d1);
    capture_mailbox_publish(&mb, &b, &d2);

    REQUIRE(d1 == -1, "first publish displaced %d, expected -1", d1);
    REQUIRE(d2 == 1,  "second publish did not displace buf_index=1 (got %d)", d2);

    /* Consume must return b (the newer frame). */
    struct frame_info out;
    REQUIRE(capture_mailbox_consume(&mb, &out) == 0, "consume failed");
    REQUIRE(out.buf_index == 3, "consumed stale frame: buf_index=%d", out.buf_index);

    capture_mailbox_destroy(&mb);
    PASS();
}

TEST(consume_empty_returns_neg1)
{
    struct capture_mailbox mb;
    capture_mailbox_init(&mb);
    struct frame_info out;
    memset(&out, 0xAB, sizeof(out));
    REQUIRE(capture_mailbox_consume(&mb, &out) == -1,
            "consume on empty mailbox returned success");
    capture_mailbox_destroy(&mb);
    PASS();
}

TEST(drain_returns_unclaimed)
{
    struct capture_mailbox mb;
    capture_mailbox_init(&mb);

    struct frame_info a = make_frame(5, 640);
    int discard;
    capture_mailbox_publish(&mb, &a, &discard);

    int drained = -99;
    REQUIRE(capture_mailbox_drain(&mb, &drained) == 0, "drain returned -1");
    REQUIRE(drained == 5, "drained wrong buf_index: %d", drained);

    /* Drain again: nothing to drain. */
    REQUIRE(capture_mailbox_drain(&mb, &drained) == -1, "second drain succeeded");
    /* Consume also empty. */
    struct frame_info out;
    REQUIRE(capture_mailbox_consume(&mb, &out) == -1, "consume after drain");

    capture_mailbox_destroy(&mb);
    PASS();
}

TEST(drain_empty_returns_neg1)
{
    struct capture_mailbox mb;
    capture_mailbox_init(&mb);
    int drained;
    REQUIRE(capture_mailbox_drain(&mb, &drained) == -1,
            "drain on empty mailbox returned success");
    capture_mailbox_destroy(&mb);
    PASS();
}

/* ---------- producer/consumer soak ---------- */

struct mbox_soak_state {
    struct capture_mailbox *mb;
    volatile int writer_done;
    volatile int reader_done;
    long long consumed;
    long long displaced;
    long long published;
};

static void *mbox_soak_producer(void *arg)
{
    struct mbox_soak_state *s = arg;
    int counter = 0;
    while (counter < 200000) {
        struct frame_info f = make_frame((counter & 3) + 1, 800);
        int d;
        capture_mailbox_publish(s->mb, &f, &d);
        if (d >= 0) __atomic_add_fetch(&s->displaced, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s->published, 1, __ATOMIC_RELAXED);
        counter++;
    }
    s->writer_done = 1;
    return NULL;
}

static void *mbox_soak_consumer(void *arg)
{
    struct mbox_soak_state *s = arg;
    while (!s->writer_done) {
        struct frame_info out;
        if (capture_mailbox_consume(s->mb, &out) == 0) {
            if (out.buf_index < 1 || out.buf_index > 4) {
                fprintf(stderr, "soak: buf_index out of range: %d\n",
                        out.buf_index);
                exit(2);
            }
            __atomic_add_fetch(&s->consumed, 1, __ATOMIC_RELAXED);
        }
    }
    /* Drain any trailing frame. */
    struct frame_info out;
    while (capture_mailbox_consume(s->mb, &out) == 0)
        __atomic_add_fetch(&s->consumed, 1, __ATOMIC_RELAXED);

    s->reader_done = 1;
    return NULL;
}

TEST(soak_publish_consume_invariants)
{
    struct capture_mailbox mb;
    capture_mailbox_init(&mb);

    struct mbox_soak_state state = {.mb = &mb};
    pthread_t w, r;
    pthread_create(&w, NULL, mbox_soak_producer, &state);
    pthread_create(&r, NULL, mbox_soak_consumer, &state);
    pthread_join(w, NULL);
    pthread_join(r, NULL);

    fprintf(stderr,
            "   mbox soak: published=%lld displaced=%lld consumed=%lld\n",
            state.published, state.displaced, state.consumed);

    /* Every published frame was either consumed or displaced (but we count
     * displaced-from-the-slot; the currently-held frame at the end is also
     * consumed by the drain loop). Invariant: published == consumed + displaced. */
    REQUIRE(state.published == state.consumed + state.displaced,
            "accounting: %lld != %lld + %lld",
            state.published, state.consumed, state.displaced);
    /* Sanity: the consumer saw at least one frame. */
    REQUIRE(state.consumed > 0, "consumer never saw a frame");

    capture_mailbox_destroy(&mb);
    PASS();
}

/* ---------- dispatcher ---------- */

typedef struct { const char *name; int (*fn)(void); } test_entry_t;

static const test_entry_t tests[] = {
    {"publish_then_consume",         run_publish_then_consume},
    {"double_publish_displaces_older", run_double_publish_displaces_older},
    {"consume_empty_returns_neg1",   run_consume_empty_returns_neg1},
    {"drain_returns_unclaimed",      run_drain_returns_unclaimed},
    {"drain_empty_returns_neg1",     run_drain_empty_returns_neg1},
    {"soak_publish_consume_invariants", run_soak_publish_consume_invariants},
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
