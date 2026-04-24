#ifndef LLCCPV_TEST_GL_HARNESS_H
#define LLCCPV_TEST_GL_HARNESS_H

/* Creates a hidden SDL3 window + OpenGL 4.3 Core context so test binaries
 * can drive the render module exactly like the app does — same context type,
 * same driver. Call once at program start; tear down at exit. */
int  gl_harness_init(void);
void gl_harness_shutdown(void);

/* Read the default framebuffer's back buffer as packed RGB8, rows flipped
 * to top-first order (matches render_readback_fbo() orientation). */
#include <stdint.h>
void gl_harness_read_backbuffer(uint8_t *out_rgb, int w, int h);

#endif
