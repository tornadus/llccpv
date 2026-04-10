#include "render.h"
#include "shader.h"

#include <time.h>
#include <linux/videodev2.h>

static const char *frag_shader_for_pixfmt(uint32_t pixfmt)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_YUYV: return "yuyv.frag";
    case V4L2_PIX_FMT_UYVY: return "uyvy.frag";
    case V4L2_PIX_FMT_NV12: return "nv12.frag";
    default:                 return "passthrough.frag";
    }
}

static const char *frag_shader_for_scale(enum scale_mode mode)
{
    switch (mode) {
    case SCALE_SHARP_BILINEAR: return "sharp_bilinear.frag";
    default:                   return "passthrough.frag";
    }
}

/* --- Source texture setup --- */

static void delete_textures(struct render_ctx *ctx)
{
    for (int i = 0; i < ctx->num_textures; i++) {
        if (ctx->textures[i]) {
            glDeleteTextures(1, &ctx->textures[i]);
            ctx->textures[i] = 0;
        }
    }
    ctx->num_textures = 0;
}

static GLuint create_texture(GLenum min_filter, GLenum mag_filter)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
    return tex;
}

static int setup_source_textures(struct render_ctx *ctx, int width, int height, uint32_t pixfmt)
{
    ctx->src_width = width;
    ctx->src_height = height;
    ctx->pixfmt = pixfmt;

    delete_textures(ctx);

    switch (pixfmt) {
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
        ctx->textures[0] = create_texture(GL_NEAREST, GL_NEAREST);
        ctx->num_textures = 1;
        ctx->tex_width = width / 2;
        ctx->tex_height = height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     ctx->tex_width, ctx->tex_height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        break;

    case V4L2_PIX_FMT_NV12:
        ctx->textures[0] = create_texture(GL_NEAREST, GL_NEAREST);
        ctx->num_textures = 2;
        ctx->tex_width = width;
        ctx->tex_height = height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0,
                     GL_RED, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);

        ctx->textures[1] = create_texture(GL_LINEAR, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width / 2, height / 2, 0,
                     GL_RG, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        break;

    default:
        ctx->textures[0] = create_texture(GL_LINEAR, GL_LINEAR);
        ctx->num_textures = 1;
        ctx->tex_width = width;
        ctx->tex_height = height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        break;
    }

    return 0;
}

/* --- FBO setup --- */

static int setup_fbo(struct render_ctx *ctx, int width, int height)
{
    if (ctx->fbo) {
        glDeleteFramebuffers(1, &ctx->fbo);
        glDeleteTextures(1, &ctx->fbo_texture);
    }

    glGenTextures(1, &ctx->fbo_texture);
    glBindTexture(GL_TEXTURE_2D, ctx->fbo_texture);

    /* FBO texture filter depends on scale mode:
     * - Nearest: GL_NEAREST
     * - Bilinear: GL_LINEAR (hardware bilinear)
     * - Sharp bilinear: GL_LINEAR (shader does the rest) */
    GLenum filter = (ctx->scale_mode == SCALE_NEAREST) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &ctx->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, ctx->fbo_texture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("FBO incomplete: 0x%x", status);
        return -1;
    }

    return 0;
}

/* --- Shader setup --- */

static void setup_conv_uniforms(struct render_ctx *ctx, uint32_t pixfmt)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_YUYV:
        ctx->conv_loc_tex = glGetUniformLocation(ctx->conv_program, "tex_yuyv");
        ctx->conv_loc_tex_uv = -1;
        ctx->conv_loc_tex_size = glGetUniformLocation(ctx->conv_program, "tex_size");
        break;
    case V4L2_PIX_FMT_UYVY:
        ctx->conv_loc_tex = glGetUniformLocation(ctx->conv_program, "tex_uyvy");
        ctx->conv_loc_tex_uv = -1;
        ctx->conv_loc_tex_size = glGetUniformLocation(ctx->conv_program, "tex_size");
        break;
    case V4L2_PIX_FMT_NV12:
        ctx->conv_loc_tex = glGetUniformLocation(ctx->conv_program, "tex_y");
        ctx->conv_loc_tex_uv = glGetUniformLocation(ctx->conv_program, "tex_uv");
        ctx->conv_loc_tex_size = -1;
        break;
    default:
        ctx->conv_loc_tex = glGetUniformLocation(ctx->conv_program, "tex_rgb");
        ctx->conv_loc_tex_uv = -1;
        ctx->conv_loc_tex_size = -1;
        break;
    }
}

static int load_conv_shader(struct render_ctx *ctx, uint32_t pixfmt, const char *shader_dir)
{
    if (ctx->conv_program)
        glDeleteProgram(ctx->conv_program);

    char vert_path[512], frag_path[512];
    snprintf(vert_path, sizeof(vert_path), "%s/quad.vert", shader_dir);
    snprintf(frag_path, sizeof(frag_path), "%s/%s", shader_dir, frag_shader_for_pixfmt(pixfmt));

    ctx->conv_program = shader_load_program(vert_path, frag_path);
    if (!ctx->conv_program)
        return -1;

    setup_conv_uniforms(ctx, pixfmt);
    ctx->conv_loc_y_flip = glGetUniformLocation(ctx->conv_program, "y_flip");
    ctx->conv_loc_color_range = glGetUniformLocation(ctx->conv_program, "color_range");
    return 0;
}

static int load_scale_shader(struct render_ctx *ctx, enum scale_mode mode, const char *shader_dir)
{
    if (ctx->scale_program)
        glDeleteProgram(ctx->scale_program);

    char vert_path[512], frag_path[512];
    snprintf(vert_path, sizeof(vert_path), "%s/quad.vert", shader_dir);
    snprintf(frag_path, sizeof(frag_path), "%s/%s", shader_dir, frag_shader_for_scale(mode));

    ctx->scale_program = shader_load_program(vert_path, frag_path);
    if (!ctx->scale_program)
        return -1;

    ctx->scale_loc_tex = glGetUniformLocation(ctx->scale_program, "tex_rgb");
    ctx->scale_loc_src_size = glGetUniformLocation(ctx->scale_program, "src_size");
    ctx->scale_loc_output_size = glGetUniformLocation(ctx->scale_program, "output_size");
    ctx->scale_loc_y_flip = glGetUniformLocation(ctx->scale_program, "y_flip");
    ctx->scale_mode = mode;

    return 0;
}

/* --- Public API --- */

int render_init(struct render_ctx *ctx, int width, int height,
                uint32_t pixfmt, enum scale_mode mode,
                enum color_range range, const char *shader_dir)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->conv_loc_tex_uv = -1;
    ctx->conv_loc_tex_size = -1;
    ctx->scale_mode = mode;
    ctx->color_range = range;

    if (load_conv_shader(ctx, pixfmt, shader_dir) < 0)
        return -1;

    if (load_scale_shader(ctx, mode, shader_dir) < 0)
        return -1;

    glGenVertexArrays(1, &ctx->vao);

    setup_source_textures(ctx, width, height, pixfmt);

    if (setup_fbo(ctx, width, height) < 0)
        return -1;

    LOG_INFO("Renderer initialized: %dx%d, pixfmt=0x%08x, scale=%d",
             width, height, pixfmt, mode);
    return 0;
}

void render_upload_frame(struct render_ctx *ctx, const struct frame_info *frame)
{
    switch (frame->pixfmt) {
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
        glBindTexture(GL_TEXTURE_2D, ctx->textures[0]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        ctx->tex_width, ctx->tex_height,
                        GL_RGBA, GL_UNSIGNED_BYTE, frame->data);
        break;

    case V4L2_PIX_FMT_NV12:
        glBindTexture(GL_TEXTURE_2D, ctx->textures[0]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        ctx->src_width, ctx->src_height,
                        GL_RED, GL_UNSIGNED_BYTE, frame->data);

        glBindTexture(GL_TEXTURE_2D, ctx->textures[1]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        ctx->src_width / 2, ctx->src_height / 2,
                        GL_RG, GL_UNSIGNED_BYTE,
                        (const uint8_t *)frame->data + ctx->src_width * ctx->src_height);
        break;

    default:
        glBindTexture(GL_TEXTURE_2D, ctx->textures[0]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        ctx->tex_width, ctx->tex_height,
                        GL_RGB, GL_UNSIGNED_BYTE, frame->data);
        break;
    }
}

void render_draw(struct render_ctx *ctx, int out_w, int out_h)
{
    /* Save the viewport set by main.c (includes letterbox/pillarbox offsets) */
    GLint saved_viewport[4];
    glGetIntegerv(GL_VIEWPORT, saved_viewport);

    /* --- Pass 1: Color conversion → FBO at source resolution --- */
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
    glViewport(0, 0, ctx->src_width, ctx->src_height);

    glUseProgram(ctx->conv_program);
    glUniform1f(ctx->conv_loc_y_flip, 1.0f); /* flip: source texture is top-row-first */
    if (ctx->conv_loc_color_range >= 0)
        glUniform1i(ctx->conv_loc_color_range, ctx->color_range);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->textures[0]);
    glUniform1i(ctx->conv_loc_tex, 0);

    if (ctx->conv_loc_tex_uv >= 0 && ctx->num_textures > 1) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx->textures[1]);
        glUniform1i(ctx->conv_loc_tex_uv, 1);
    }

    if (ctx->conv_loc_tex_size >= 0)
        glUniform2f(ctx->conv_loc_tex_size,
                    (float)ctx->src_width, (float)ctx->src_height);

    glBindVertexArray(ctx->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* --- Pass 2: Scaling → screen at output resolution --- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(saved_viewport[0], saved_viewport[1],
               saved_viewport[2], saved_viewport[3]);

    glUseProgram(ctx->scale_program);
    glUniform1f(ctx->scale_loc_y_flip, 0.0f); /* no flip: FBO is in GL orientation */

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->fbo_texture);
    glUniform1i(ctx->scale_loc_tex, 0);

    if (ctx->scale_loc_src_size >= 0)
        glUniform2f(ctx->scale_loc_src_size,
                    (float)ctx->src_width, (float)ctx->src_height);
    if (ctx->scale_loc_output_size >= 0)
        glUniform2f(ctx->scale_loc_output_size,
                    (float)out_w, (float)out_h);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glUseProgram(0);
}

int render_resize(struct render_ctx *ctx, int width, int height,
                  uint32_t pixfmt, const char *shader_dir)
{
    if (pixfmt != ctx->pixfmt) {
        if (load_conv_shader(ctx, pixfmt, shader_dir) < 0)
            return -1;
    }

    setup_source_textures(ctx, width, height, pixfmt);
    setup_fbo(ctx, width, height);
    LOG_INFO("Renderer resized: %dx%d, pixfmt=0x%08x", width, height, pixfmt);
    return 0;
}

void render_cleanup(struct render_ctx *ctx)
{
    if (ctx->conv_program) {
        glDeleteProgram(ctx->conv_program);
        ctx->conv_program = 0;
    }
    if (ctx->scale_program) {
        glDeleteProgram(ctx->scale_program);
        ctx->scale_program = 0;
    }
    delete_textures(ctx);
    if (ctx->fbo) {
        glDeleteFramebuffers(1, &ctx->fbo);
        glDeleteTextures(1, &ctx->fbo_texture);
        ctx->fbo = 0;
        ctx->fbo_texture = 0;
    }
    if (ctx->vao) {
        glDeleteVertexArrays(1, &ctx->vao);
        ctx->vao = 0;
    }
}
