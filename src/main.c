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

int main(int argc, char *argv[])
{
    const char *device = NULL;
    const char *audio_source = NULL;
    bool start_fullscreen = false;
    bool stretch = false;
    bool no_audio = false;
    int vsync_mode = -1;
    enum scale_mode scale = SCALE_BILINEAR;
    enum color_range range = RANGE_LIMITED;
    float sharpness = -1.0f;

    static struct option long_opts[] = {
        {"device",       required_argument, NULL, 'd'},
        {"audio-source", required_argument, NULL, 'a'},
        {"no-audio",     no_argument,       NULL, 'n'},
        {"fullscreen",   no_argument,       NULL, 'f'},
        {"stretch",      no_argument,       NULL, 's'},
        {"scale",        required_argument, NULL, 'S'},
        {"range",        required_argument, NULL, 'r'},
        {"sharpness",    required_argument, NULL, 'P'},
        {"vsync",        required_argument, NULL, 'v'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:a:nfS:r:P:sv:h", long_opts, NULL)) != -1) {
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
        case 'P': sharpness = strtof(optarg, NULL); break;
        case 'v': vsync_mode = atoi(optarg); break;
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
                "  -v, --vsync MODE         0=off, 1=on, -1=adaptive (default)\n"
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
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    /* Open capture device */
    struct capture_ctx cap;
    if (capture_open(&cap, device, pick.pixfmt, pick.width, pick.height) < 0)
        return 1;

    /* Register a custom event type for new-frame signals from the capture thread */
    uint32_t frame_event_type = SDL_RegisterEvents(1);
    if (frame_event_type == 0) {
        LOG_ERROR("SDL_RegisterEvents failed");
        capture_close(&cap);
        SDL_Quit();
        return 1;
    }

    /* Request OpenGL 4.3 Core (required by FSR EASU's textureGather) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_WindowFlags win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    if (start_fullscreen)
        win_flags |= SDL_WINDOW_FULLSCREEN;

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

    if (render_init(&rctx, cap.width, cap.height, cap.pixfmt, scale, range, shader_dir) < 0) {
        SDL_GL_DestroyContext(gl_ctx);
        SDL_DestroyWindow(window);
        capture_close(&cap);
        SDL_Quit();
        return 1;
    }
    if (sharpness >= 0.0f)
        render_set_sharpness(&rctx, sharpness);

    /* Start capture (launches capture thread) */
    if (capture_start(&cap, frame_event_type) < 0) {
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

    /* Main loop — event-driven, renders only when a new frame arrives */
    bool running = true;
    bool have_frame = false;

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
                break;
            }
        } while (SDL_PollEvent(&event));

        if (!running)
            break;

        /* Pick up the latest frame from the mailbox */
        if (have_frame) {
            struct frame_info frame;
            if (capture_get_frame(&cap, &frame) == 0)
                render_upload_frame(&rctx, &frame);
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

        SDL_GL_SwapWindow(window);
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
