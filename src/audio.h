#ifndef LLCCPV_AUDIO_H
#define LLCCPV_AUDIO_H

#include "util.h"

struct audio_ctx;

/* Initialize PipeWire audio passthrough.
 * source_match: substring to match against PipeWire node names (e.g., "Elgato").
 *               If NULL, tries to auto-detect a capture card audio source.
 * Returns an allocated context on success, NULL on failure. */
struct audio_ctx *audio_init(const char *source_match);

/* Start audio passthrough. Returns 0 on success, -1 on failure. */
int audio_start(struct audio_ctx *ctx);

/* Stop and clean up audio. */
void audio_stop(struct audio_ctx *ctx);

#endif
