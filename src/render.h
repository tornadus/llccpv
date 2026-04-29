#ifndef LLCCPV_RENDER_H
#define LLCCPV_RENDER_H

#include "util.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#define RENDER_MAX_TEXTURES 2

enum scale_mode {
    SCALE_NEAREST = 0,
    SCALE_BILINEAR,
    SCALE_SHARP_BILINEAR,
    SCALE_FSR,
};

enum color_range {
    RANGE_LIMITED = 0, /* TV: Y 16-235, UV 16-240 */
    RANGE_FULL = 1,    /* PC: 0-255 */
};

enum color_matrix {
    MATRIX_BT601 = 0, /* SDTV (480i/576i) */
    MATRIX_BT709 = 1, /* HDTV (720p+) */
};

struct render_ctx {
    /* Color conversion pass */
    GLuint conv_program;
    GLuint textures[RENDER_MAX_TEXTURES];
    int num_textures;
    GLint conv_loc_tex;
    GLint conv_loc_tex_uv;
    GLint conv_loc_tex_size;
    GLint conv_loc_y_flip;
    GLint conv_loc_color_range;
    enum color_range color_range;
    GLint conv_loc_color_matrix;
    enum color_matrix color_matrix;
    int tex_width;
    int tex_height;

    /* FBO: color conversion renders here at source resolution */
    GLuint fbo;
    GLuint fbo_texture;

    /* Scaling pass */
    GLuint scale_program;
    GLint scale_loc_tex;
    GLint scale_loc_src_size;
    GLint scale_loc_output_size;
    GLint scale_loc_y_flip;
    enum scale_mode scale_mode;

    /* FSR pass (only used when scale_mode == SCALE_FSR) */
    GLuint easu_program;
    GLuint rcas_program;
    GLuint fsr_fbo;
    GLuint fsr_fbo_texture;
    int fsr_out_w;
    int fsr_out_h;
    GLint easu_loc_tex;
    GLint easu_loc_con0;
    GLint easu_loc_con1;
    GLint easu_loc_con2;
    GLint easu_loc_con3;
    GLint easu_loc_y_flip;
    uint32_t easu_con0[4];
    uint32_t easu_con1[4];
    uint32_t easu_con2[4];
    uint32_t easu_con3[4];
    GLint rcas_loc_tex;
    GLint rcas_loc_con;
    GLint rcas_loc_y_flip;
    uint32_t rcas_con[4];
    float rcas_sharpness;

    /* Shared */
    GLuint vao;
    int src_width;
    int src_height;
    uint32_t pixfmt;
};

/* Initialize the renderer for a given source format and scale mode.
 * shader_dir is the path to the directory containing GLSL files.
 * Returns 0 on success, -1 on failure. */
int render_init(struct render_ctx *ctx, int width, int height,
                uint32_t pixfmt, enum scale_mode mode,
                enum color_range range, enum color_matrix matrix,
                const char *shader_dir);

/* Upload a new frame to the GPU texture(s). */
void render_upload_frame(struct render_ctx *ctx, const struct frame_info *frame);

/* Draw: 2 or 3 passes depending on scale mode.
 * Standard: pass 1 (conversion → FBO), pass 2 (scaling → screen).
 * FSR:      pass 1 (conversion → FBO), pass 2 (EASU → FSR FBO), pass 3 (RCAS → screen).
 * out_w/out_h is the viewport size for the final pass. */
void render_draw(struct render_ctx *ctx, int out_w, int out_h);

/* Set FSR RCAS sharpness (0.0 = maximum, 2.0 = soft). Only effective in FSR mode. */
void render_set_sharpness(struct render_ctx *ctx, float sharpness);

/* Read back the conversion-pass FBO as packed RGB8 (w*h*3 bytes).
 * w/h must match the render_init() source dimensions. Output rows are
 * top-first (row 0 is the top of the source image, matching the input
 * frame's memory layout). Returns 0 on success, -1 on error. */
int render_readback_fbo(struct render_ctx *ctx, uint8_t *out_rgb, int w, int h);

/* Clean up GL resources. */
void render_cleanup(struct render_ctx *ctx);

#endif
