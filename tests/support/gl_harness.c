#include "gl_harness.h"
#include "util.h"

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

static SDL_Window   *g_window;
static SDL_GLContext g_ctx;

int gl_harness_init(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    g_window = SDL_CreateWindow("llccpv-tests", 64, 64,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!g_window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    g_ctx = SDL_GL_CreateContext(g_window);
    if (!g_ctx) {
        LOG_ERROR("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        SDL_Quit();
        return -1;
    }

    LOG_INFO("GL harness: %s (%s)",
             glGetString(GL_VERSION), glGetString(GL_RENDERER));
    return 0;
}

void gl_harness_shutdown(void)
{
    if (g_ctx) { SDL_GL_DestroyContext(g_ctx); g_ctx = NULL; }
    if (g_window) { SDL_DestroyWindow(g_window); g_window = NULL; }
    SDL_Quit();
}

void gl_harness_read_backbuffer(uint8_t *out_rgb, int w, int h)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, out_rgb);

    size_t row = (size_t)w * 3;
    uint8_t *tmp = (uint8_t *)malloc(row);
    if (!tmp) return;
    for (int y = 0; y < h / 2; y++) {
        uint8_t *t = out_rgb + (size_t)y * row;
        uint8_t *b = out_rgb + (size_t)(h - 1 - y) * row;
        memcpy(tmp, t, row);
        memcpy(t, b, row);
        memcpy(b, tmp, row);
    }
    free(tmp);
}
