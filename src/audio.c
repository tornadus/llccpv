#define _GNU_SOURCE
#include "audio.h"

#include <stdatomic.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#pragma GCC diagnostic pop

/* Lock-free SPSC ring buffer for audio samples.
 * Sized for ~170ms at 48kHz stereo float32 — enough to absorb scheduling jitter. */
#define RING_SAMPLES (16384)
#define RING_MASK    (RING_SAMPLES - 1)

struct ring_buf {
    float data[RING_SAMPLES];
    _Atomic uint32_t read_pos;
    _Atomic uint32_t write_pos;
};

static void ring_write(struct ring_buf *r, const float *src, uint32_t count)
{
    uint32_t wp = atomic_load(&r->write_pos);
    for (uint32_t i = 0; i < count; i++)
        r->data[(wp + i) & RING_MASK] = src[i];
    atomic_store(&r->write_pos, wp + count);
}

static void ring_read(struct ring_buf *r, float *dst, uint32_t count)
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

struct audio_ctx {
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook core_listener;
    struct spa_hook registry_listener;

    struct pw_stream *capture;
    struct pw_stream *playback;
    struct spa_hook capture_listener;
    struct spa_hook playback_listener;

    struct ring_buf ring;

    int channels;

    char source_name[256]; /* matched source node name */
    bool source_found;
};

/* --- Capture stream callbacks --- */

static void on_capture_process(void *data)
{
    struct audio_ctx *ctx = data;
    struct pw_buffer *buf = pw_stream_dequeue_buffer(ctx->capture);
    if (!buf)
        return;

    struct spa_buffer *spa_buf = buf->buffer;
    float *samples = spa_buf->datas[0].data;
    if (!samples) {
        pw_stream_queue_buffer(ctx->capture, buf);
        return;
    }

    uint32_t n_bytes = spa_buf->datas[0].chunk->size;
    uint32_t n_samples = n_bytes / sizeof(float);

    ring_write(&ctx->ring, samples, n_samples);
    pw_stream_queue_buffer(ctx->capture, buf);
}

static void on_capture_state_changed(void *data, enum pw_stream_state old,
                                     enum pw_stream_state state, const char *error)
{
    (void)data;
    (void)old;
    LOG_INFO("Audio capture stream: %s%s%s",
             pw_stream_state_as_string(state),
             error ? " — " : "", error ? error : "");
}

static const struct pw_stream_events capture_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_capture_state_changed,
    .process = on_capture_process,
};

/* --- Playback stream callbacks --- */

static void on_playback_process(void *data)
{
    struct audio_ctx *ctx = data;
    struct pw_buffer *buf = pw_stream_dequeue_buffer(ctx->playback);
    if (!buf)
        return;

    struct spa_buffer *spa_buf = buf->buffer;
    float *samples = spa_buf->datas[0].data;
    if (!samples) {
        pw_stream_queue_buffer(ctx->playback, buf);
        return;
    }

    int stride = sizeof(float) * ctx->channels;
    uint32_t max_frames = spa_buf->datas[0].maxsize / stride;

    /* buf->requested is in frames (1 frame = channels samples) */
    uint32_t n_frames = max_frames;
    if (buf->requested && buf->requested < n_frames)
        n_frames = buf->requested;

    uint32_t n_samples = n_frames * ctx->channels;
    ring_read(&ctx->ring, samples, n_samples);

    spa_buf->datas[0].chunk->offset = 0;
    spa_buf->datas[0].chunk->size = n_frames * stride;
    spa_buf->datas[0].chunk->stride = stride;

    pw_stream_queue_buffer(ctx->playback, buf);
}

static void on_playback_state_changed(void *data, enum pw_stream_state old,
                                      enum pw_stream_state state, const char *error)
{
    (void)data;
    (void)old;
    LOG_INFO("Audio playback stream: %s%s%s",
             pw_stream_state_as_string(state),
             error ? " — " : "", error ? error : "");
}

static const struct pw_stream_events playback_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_playback_state_changed,
    .process = on_playback_process,
};

/* --- Registry: find the capture card's audio source --- */

static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props)
{
    struct audio_ctx *ctx = data;
    (void)permissions;
    (void)version;

    if (ctx->source_found)
        return;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class || strcmp(media_class, "Audio/Source") != 0)
        return;

    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!name)
        return;

    /* Match against the user-provided substring or common capture card keywords */
    const char *match = ctx->source_name;
    bool matched = false;

    if (match[0]) {
        matched = (strstr(name, match) != NULL);
    } else {
        /* Auto-detect: look for common capture card identifiers */
        matched = (strstr(name, "Elgato") != NULL ||
                   strstr(name, "Game_Capture") != NULL ||
                   strstr(name, "AVerMedia") != NULL ||
                   strstr(name, "Cam_Link") != NULL);
    }

    if (matched) {
        LOG_INFO("Audio source found: %s (id=%u)", name, id);
        snprintf(ctx->source_name, sizeof(ctx->source_name), "%s", name);
        ctx->source_found = true;
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
};

/* --- Core sync to know when registry enumeration is done --- */

static void on_core_done(void *data, uint32_t id, int seq)
{
    struct audio_ctx *ctx = data;
    (void)id;
    (void)seq;
    /* Signal that initial enumeration is complete */
    pw_thread_loop_signal(ctx->loop, false);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
};

/* --- Public API --- */

struct audio_ctx *audio_init(const char *source_match)
{
    pw_init(NULL, NULL);

    struct audio_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    if (source_match)
        snprintf(ctx->source_name, sizeof(ctx->source_name), "%s", source_match);

    ctx->loop = pw_thread_loop_new("llccpv-audio", NULL);
    if (!ctx->loop) {
        LOG_ERROR("Failed to create PipeWire thread loop");
        free(ctx);
        return NULL;
    }

    ctx->context = pw_context_new(pw_thread_loop_get_loop(ctx->loop), NULL, 0);
    if (!ctx->context) {
        LOG_ERROR("Failed to create PipeWire context");
        pw_thread_loop_destroy(ctx->loop);
        free(ctx);
        return NULL;
    }

    /* Start the loop so we can connect */
    pw_thread_loop_start(ctx->loop);
    pw_thread_loop_lock(ctx->loop);

    ctx->core = pw_context_connect(ctx->context, NULL, 0);
    if (!ctx->core) {
        LOG_ERROR("Failed to connect to PipeWire: %s", strerror(errno));
        pw_thread_loop_unlock(ctx->loop);
        pw_thread_loop_stop(ctx->loop);
        pw_context_destroy(ctx->context);
        pw_thread_loop_destroy(ctx->loop);
        free(ctx);
        return NULL;
    }

    pw_core_add_listener(ctx->core, &ctx->core_listener, &core_events, ctx);

    /* Enumerate nodes to find the audio source */
    ctx->registry = pw_core_get_registry(ctx->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(ctx->registry, &ctx->registry_listener,
                             &registry_events, ctx);

    /* Trigger a sync to know when enumeration is done */
    pw_core_sync(ctx->core, PW_ID_CORE, 0);
    pw_thread_loop_wait(ctx->loop);

    if (!ctx->source_found) {
        LOG_WARN("No matching audio source found — audio passthrough disabled");
        pw_proxy_destroy((struct pw_proxy *)ctx->registry);
        spa_hook_remove(&ctx->core_listener);
        pw_core_disconnect(ctx->core);
        pw_thread_loop_unlock(ctx->loop);
        pw_thread_loop_stop(ctx->loop);
        pw_context_destroy(ctx->context);
        pw_thread_loop_destroy(ctx->loop);
        free(ctx);
        return NULL;
    }

    pw_thread_loop_unlock(ctx->loop);
    return ctx;
}

int audio_start(struct audio_ctx *ctx)
{
    pw_thread_loop_lock(ctx->loop);

    /* Audio format: 48kHz stereo float32 interleaved */
    ctx->channels = 2;
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = 48000,
        .channels = 2,
    };
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    /* Create capture stream targeting the matched source */
    ctx->capture = pw_stream_new(ctx->core, "llccpv-capture",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Game",
            PW_KEY_TARGET_OBJECT, ctx->source_name,
            PW_KEY_NODE_LATENCY, "256/48000",
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            NULL));

    if (!ctx->capture) {
        LOG_ERROR("Failed to create capture stream");
        pw_thread_loop_unlock(ctx->loop);
        return -1;
    }

    pw_stream_add_listener(ctx->capture, &ctx->capture_listener,
                           &capture_events, ctx);

    pw_stream_connect(ctx->capture, PW_DIRECTION_INPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    /* Create playback stream targeting default output */
    b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    ctx->playback = pw_stream_new(ctx->core, "llccpv-playback",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Game",
            PW_KEY_NODE_LATENCY, "256/48000",
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            NULL));

    if (!ctx->playback) {
        LOG_ERROR("Failed to create playback stream");
        pw_thread_loop_unlock(ctx->loop);
        return -1;
    }

    pw_stream_add_listener(ctx->playback, &ctx->playback_listener,
                           &playback_events, ctx);

    pw_stream_connect(ctx->playback, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    pw_thread_loop_unlock(ctx->loop);

    LOG_INFO("Audio passthrough started: %s -> default output", ctx->source_name);
    return 0;
}

void audio_stop(struct audio_ctx *ctx)
{
    if (!ctx)
        return;

    pw_thread_loop_lock(ctx->loop);

    if (ctx->playback) {
        pw_stream_destroy(ctx->playback);
        ctx->playback = NULL;
    }
    if (ctx->capture) {
        pw_stream_destroy(ctx->capture);
        ctx->capture = NULL;
    }
    if (ctx->registry) {
        pw_proxy_destroy((struct pw_proxy *)ctx->registry);
        ctx->registry = NULL;
    }

    spa_hook_remove(&ctx->core_listener);
    pw_core_disconnect(ctx->core);
    ctx->core = NULL;

    pw_thread_loop_unlock(ctx->loop);
    pw_thread_loop_stop(ctx->loop);

    pw_context_destroy(ctx->context);
    pw_thread_loop_destroy(ctx->loop);

    free(ctx);

    LOG_INFO("Audio passthrough stopped");
}
