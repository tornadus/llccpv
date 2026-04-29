#include "util.h"
#include "capture.h"
#include "render.h"
#include "audio.h"
#include "picker.h"

#include <unistd.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <getopt.h>

static void toggle_fullscreen(SDL_Window *window)
{
    SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    SDL_SetWindowFullscreen(window, !(flags & SDL_WINDOW_FULLSCREEN));
}

static const char *find_shader_dir(const char *argv0)
{
    static char buf[512];

    if (access("shaders/quad.vert", R_OK) == 0)
        return "shaders";

    const char *slash = strrchr(argv0, '/');
    if (slash) {
        size_t dirlen = (size_t)(slash - argv0);
        snprintf(buf, sizeof(buf), "%.*s/../shaders", (int)dirlen, argv0);
        if (access(buf, R_OK) == 0)
            return buf;
    }

    snprintf(buf, sizeof(buf), "/usr/local/share/llccpv/shaders");
    if (access(buf, R_OK) == 0)
        return buf;

    return "shaders";
}

/* Test/debug: dump backbuffer as P6 PPM, rows flipped to top-first. */
static int dump_backbuffer_ppm(const char *path, int w, int h)
{
    uint8_t *buf = malloc((size_t)w * h * 3);
    if (!buf) return -1;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);

    size_t row = (size_t)w * 3;
    uint8_t *tmp = malloc(row);
    if (tmp) {
        for (int y = 0; y < h / 2; y++) {
            uint8_t *t = buf + (size_t)y * row;
            uint8_t *b = buf + (size_t)(h - 1 - y) * row;
            memcpy(tmp, t, row);
            memcpy(t, b, row);
            memcpy(b, tmp, row);
        }
        free(tmp);
    }

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); LOG_ERROR("dump_frame: fopen %s: %s", path, strerror(errno)); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(buf, 1, (size_t)w * h * 3, f);
    fclose(f);
    free(buf);
    return 0;
}

static Uint32 SDLCALL exit_timer_cb(void *userdata, SDL_TimerID tid, Uint32 interval)
{
    (void)userdata; (void)tid; (void)interval;
    SDL_Event ev = { .type = SDL_EVENT_QUIT };
    SDL_PushEvent(&ev);
    return 0; /* one-shot */
}

int main(int argc, char *argv[])
{
    const char *device = NULL;
    const char *audio_source = NULL;
    bool start_fullscreen = false;
    bool stretch = false;
    bool no_audio = false;
    bool headless = false;
    int vsync_mode = -1;
    enum scale_mode scale = SCALE_BILINEAR;
    enum color_range range = RANGE_LIMITED;
    int matrix_override = -1; /* -1 = auto, else MATRIX_BT601/MATRIX_BT709 */
    float sharpness = -1.0f;
    int exit_after_ms = 0;
    int frames_limit = 0;
    const char *dump_frame_path = NULL;

    enum {
        OPT_EXIT_AFTER = 256,
        OPT_FRAMES,
        OPT_HEADLESS,
        OPT_DUMP_FRAME,
    };

    static struct option long_opts[] = {
        {"device",       required_argument, NULL, 'd'},
        {"audio-source", required_argument, NULL, 'a'},
        {"no-audio",     no_argument,       NULL, 'n'},
        {"fullscreen",   no_argument,       NULL, 'f'},
        {"stretch",      no_argument,       NULL, 's'},
        {"scale",        required_argument, NULL, 'S'},
        {"range",        required_argument, NULL, 'r'},
        {"matrix",       required_argument, NULL, 'c'},
        {"sharpness",    required_argument, NULL, 'P'},
        {"vsync",        required_argument, NULL, 'v'},
        {"exit-after",   required_argument, NULL, OPT_EXIT_AFTER},
        {"frames",       required_argument, NULL, OPT_FRAMES},
        {"headless",     no_argument,       NULL, OPT_HEADLESS},
        {"dump-frame",   required_argument, NULL, OPT_DUMP_FRAME},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:a:nfS:r:c:P:sv:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 'a': audio_source = optarg; break;
        case 'n': no_audio = true; break;
        case 'f': start_fullscreen = true; break;
        case 's': stretch = true; break;
        case 'S':
            if (strcmp(optarg, "nearest") == 0)       scale = SCALE_NEAREST;
            else if (strcmp(optarg, "bilinear") == 0)  scale = SCALE_BILINEAR;
            else if (strcmp(optarg, "sharp") == 0)     scale = SCALE_SHARP_BILINEAR;
            else if (strcmp(optarg, "fsr") == 0)       scale = SCALE_FSR;
            else {
                LOG_ERROR("Unknown scale mode: %s (use nearest, bilinear, sharp, fsr)", optarg);
                return 1;
            }
            break;
        case 'r':
            if (strcmp(optarg, "limited") == 0)      range = RANGE_LIMITED;
            else if (strcmp(optarg, "full") == 0)     range = RANGE_FULL;
            else {
                LOG_ERROR("Unknown range: %s (use limited, full)", optarg);
                return 1;
            }
            break;
        case 'c':
            if (strcmp(optarg, "auto") == 0)        matrix_override = -1;
            else if (strcmp(optarg, "bt601") == 0)   matrix_override = MATRIX_BT601;
            else if (strcmp(optarg, "bt709") == 0)   matrix_override = MATRIX_BT709;
            else {
                LOG_ERROR("Unknown matrix: %s (use auto, bt601, bt709)", optarg);
                return 1;
            }
            break;
        case 'P': sharpness = strtof(optarg, NULL); break;
        case 'v': vsync_mode = atoi(optarg); break;
        case OPT_EXIT_AFTER: exit_after_ms  = atoi(optarg); break;
        case OPT_FRAMES:     frames_limit   = atoi(optarg); break;
        case OPT_HEADLESS:   headless       = true; break;
        case OPT_DUMP_FRAME: dump_frame_path = optarg; break;
        case 'h':
            fprintf(stderr,
                "Usage: %s [options]\n"
                "  -d, --device PATH        V4L2 device (default: /dev/video0)\n"
                "  -a, --audio-source NAME  PipeWire audio source (auto-detect if omitted)\n"
                "  -n, --no-audio           Disable audio passthrough\n"
                "  -f, --fullscreen         Start in fullscreen\n"
                "  -s, --stretch            Stretch to fill window (ignore aspect ratio)\n"
                "  -S, --scale MODE         nearest, bilinear (default), sharp, fsr\n"
                "  -P, --sharpness VALUE    FSR sharpness: 0.0 (max) to 2.0 (soft), default 0.2\n"
                "  -r, --range MODE         limited (default, TV), full (PC)\n"
                "  -c, --matrix MODE        YUV->RGB matrix: auto (default), bt601, bt709\n"
                "  -v, --vsync MODE         0=off, 1=on, -1=adaptive (default)\n"
                "      --exit-after MS      Auto-quit after MS milliseconds\n"
                "      --frames N           Auto-quit after rendering N frames\n"
                "      --headless           Create window hidden (SDL_WINDOW_HIDDEN)\n"
                "      --dump-frame PATH    Write last rendered frame as PPM on exit\n"
                "  -h, --help               Show this help\n",
                argv[0]);
            return 0;
        default:
            return 1;
        }
    }

    /* Device picker: show Qt dialog if no device specified on CLI */
    struct picker_result pick = { .scale_mode = -1, .color_range = -1, .sharpness = -1.0f };
    if (!device) {
        if (picker_show(&pick, false) < 0) {
            LOG_INFO("No device selected, exiting");
            return 0;
        }
        device = pick.device_path;
    }

    /* Initialize SDL */
    if (!SDL_Init(SDL_INIT_VIDEO | (exit_after_ms > 0 ? SDL_INIT_EVENTS : 0))) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    /* Open capture device */
    struct capture_ctx cap;
    if (capture_open(&cap, device, pick.pixfmt, pick.width, pick.height) < 0)
        return 1;

    /* Register custom event types: new-frame and source-format-change. */
    uint32_t frame_event_type = SDL_RegisterEvents(2);
    if (frame_event_type == 0) {
        LOG_ERROR("SDL_RegisterEvents failed");
        capture_close(&cap);
        SDL_Quit();
        return 1;
    }
    uint32_t reinit_event_type = frame_event_type + 1;

    /* Request OpenGL 4.3 Core (required by FSR EASU's textureGather) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_WindowFlags win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    if (start_fullscreen)
        win_flags |= SDL_WINDOW_FULLSCREEN;
    if (headless)
        win_flags |= SDL_WINDOW_HIDDEN;

    SDL_Window *window = SDL_CreateWindow("llccpv", cap.width, cap.height, win_flags);
    if (!window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        capture_close(&cap);
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        LOG_ERROR("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        capture_close(&cap);
        SDL_Quit();
        return 1;
    }

    /* Set vsync: try adaptive first, fall back to off */
    if (!SDL_GL_SetSwapInterval(vsync_mode)) {
        LOG_WARN("Swap interval %d not supported, trying 0", vsync_mode);
        SDL_GL_SetSwapInterval(0);
    }

    LOG_INFO("OpenGL: %s", glGetString(GL_VERSION));
    LOG_INFO("Renderer: %s", glGetString(GL_RENDERER));

    /* Initialize renderer */
    const char *shader_dir = find_shader_dir(argv[0]);
    struct render_ctx rctx;
    /* Use picker's settings if available, otherwise CLI defaults */
    if (pick.scale_mode >= 0)
        scale = pick.scale_mode;
    if (pick.color_range >= 0)
        range = pick.color_range;
    if (pick.sharpness >= 0.0f)
        sharpness = pick.sharpness;

    enum color_matrix matrix = matrix_override >= 0 ? (enum color_matrix)matrix_override
                                                    : (enum color_matrix)cap.color_matrix;
    if (matrix_override >= 0)
        LOG_INFO("YUV->RGB matrix: %s (CLI override)",
                 matrix == MATRIX_BT709 ? "BT.709" : "BT.601");

    if (render_init(&rctx, cap.width, cap.height, cap.pixfmt, scale, range, matrix, shader_dir) < 0) {
        SDL_GL_DestroyContext(gl_ctx);
        SDL_DestroyWindow(window);
        capture_close(&cap);
        SDL_Quit();
        return 1;
    }
    if (sharpness >= 0.0f)
        render_set_sharpness(&rctx, sharpness);

    /* Start capture (launches capture thread) */
    if (capture_start(&cap, frame_event_type, reinit_event_type) < 0) {
        render_cleanup(&rctx);
        SDL_GL_DestroyContext(gl_ctx);
        SDL_DestroyWindow(window);
        capture_close(&cap);
        SDL_Quit();
        return 1;
    }

    /* Initialize audio passthrough */
    struct audio_ctx *audio = NULL;
    if (!no_audio) {
        audio = audio_init(audio_source);
        if (audio) {
            if (audio_start(audio) < 0) {
                audio_stop(audio);
                audio = NULL;
            }
        }
        /* Audio failure is non-fatal — continue without audio */
    }

    /* Test-mode: schedule auto-quit */
    if (exit_after_ms > 0)
        SDL_AddTimer((Uint32)exit_after_ms, exit_timer_cb, NULL);

    /* Main loop — event-driven, renders only when a new frame arrives */
    bool running = true;
    bool have_frame = false;
    bool need_reinit = false;
    int frames_rendered = 0;

    while (running) {
        SDL_Event event;

        /* Wait for events (blocks until something happens — saves CPU when idle).
         * The capture thread pushes a custom event whenever a new frame is ready. */
        if (!SDL_WaitEvent(&event))
            continue;

        /* Process this event and any others that have queued up */
        do {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q)
                    running = false;
                else if (event.key.key == SDLK_F11)
                    toggle_fullscreen(window);
                else if (event.key.key == SDLK_RETURN &&
                         (event.key.mod & SDL_KMOD_ALT))
                    toggle_fullscreen(window);
                break;
            default:
                if (event.type == frame_event_type)
                    have_frame = true;
                else if (event.type == reinit_event_type)
                    need_reinit = true;
                break;
            }
        } while (SDL_PollEvent(&event));

        if (!running)
            break;

        /* Source format changed — tear down render, reinit capture + render
         * at the new dims. Viewport math below handles any aspect change. */
        if (need_reinit) {
            render_cleanup(&rctx);
            if (capture_reinit(&cap) < 0) {
                LOG_ERROR("capture_reinit failed; exiting");
                running = false;
                break;
            }
            enum color_matrix new_matrix = matrix_override >= 0
                ? (enum color_matrix)matrix_override
                : (enum color_matrix)cap.color_matrix;
            if (render_init(&rctx, cap.width, cap.height, cap.pixfmt,
                            scale, range, new_matrix, shader_dir) < 0) {
                LOG_ERROR("render_init after reinit failed; exiting");
                running = false;
                break;
            }
            if (sharpness >= 0.0f)
                render_set_sharpness(&rctx, sharpness);
            LOG_INFO("Source format change: reinitialized %dx%d pixfmt=0x%08x",
                     cap.width, cap.height, cap.pixfmt);
            need_reinit = false;
            have_frame = false; /* mailbox was drained during reinit */
            continue; /* wait for the first frame at the new format */
        }

        /* Pick up the latest frame from the mailbox */
        bool rendered_new_frame = false;
        if (have_frame) {
            struct frame_info frame;
            if (capture_get_frame(&cap, &frame) == 0) {
                render_upload_frame(&rctx, &frame);
                rendered_new_frame = true;
            }
            have_frame = false;
        }

        /* Compute viewport with aspect ratio correction */
        int win_w, win_h;
        SDL_GetWindowSizeInPixels(window, &win_w, &win_h);

        /* Clear entire window to black (visible as letterbox/pillarbox bars) */
        glViewport(0, 0, win_w, win_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        int vp_w = win_w, vp_h = win_h;
        if (stretch) {
            glViewport(0, 0, win_w, win_h);
        } else {
            float src_aspect = (float)rctx.src_width / (float)rctx.src_height;
            float win_aspect = (float)win_w / (float)win_h;
            int vp_x, vp_y;

            if (src_aspect > win_aspect) {
                vp_w = win_w;
                vp_h = (int)(win_w / src_aspect);
                vp_x = 0;
                vp_y = (win_h - vp_h) / 2;
            } else {
                vp_h = win_h;
                vp_w = (int)(win_h * src_aspect);
                vp_x = (win_w - vp_w) / 2;
                vp_y = 0;
            }

            glViewport(vp_x, vp_y, vp_w, vp_h);
        }

        render_draw(&rctx, vp_w, vp_h);
        glFlush();

        /* Test-mode: capture the latest *new* frame before swap. Skips
         * iterations with no new frame so we don't dump uninit textures. */
        if (dump_frame_path && rendered_new_frame)
            dump_backbuffer_ppm(dump_frame_path, win_w, win_h);

        SDL_GL_SwapWindow(window);

        if (rendered_new_frame) {
            frames_rendered++;
            if (frames_limit > 0 && frames_rendered >= frames_limit)
                running = false;
        }
    }

    /* Cleanup */
    audio_stop(audio);
    render_cleanup(&rctx);
    capture_close(&cap);
    SDL_GL_DestroyContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    LOG_INFO("Exiting cleanly");
    return 0;
}
