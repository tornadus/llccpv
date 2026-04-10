# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Dependencies (Fedora)
kdesu dnf install -y SDL3-devel mesa-libGL-devel pipewire-devel qt6-qtbase-devel meson gcc g++

# Build
meson setup build
meson compile -C build

# Rebuild after changes (incremental)
meson compile -C build

# Full clean rebuild
rm -rf build && meson setup build && meson compile -C build

# Run
./build/llccpv                              # picker dialog
./build/llccpv -d /dev/video0               # bypass picker
./build/llccpv -d /dev/video0 -S fsr -P 0.2 -r limited -f  # all options
```

No test suite exists yet.

## Architecture

llccpv is a low-latency V4L2 capture card preview with three independent subsystems connected through main.c's event loop:

**Capture (C, pthreads)** → **Render (C, OpenGL 4.3)** → **Display (SDL3)**
**Audio (C, PipeWire)** runs independently alongside.
**Picker (C++, Qt6)** runs once before the main loop, then exits.

### Threading Model

- **Main thread**: SDL3 event loop + all OpenGL calls. Blocks on `SDL_WaitEvent()`.
- **Capture thread**: Polls V4L2, dequeues frames, publishes to an atomic mailbox, wakes main via `SDL_PushEvent()`.
- **Audio thread**: PipeWire-managed. Process callbacks copy between capture/playback streams through a lock-free ring buffer.

No thread touches another's resources — capture never touches GL, render never touches V4L2 buffers directly.

### Mailbox Pattern (capture → render)

The capture thread writes the latest frame to an atomic slot, silently dropping unconsumed frames. This bounds latency to at most one frame period. The render thread holds exactly one V4L2 buffer (the one being displayed) and requeues it only when a newer frame arrives.

### Render Pipeline

The pipeline has 2 or 3 passes depending on scale mode:

**Standard modes (nearest, bilinear, sharp bilinear):**
1. **Pass 1 — Color conversion**: Raw YUV texture(s) → FBO at source resolution. Shader selected by pixel format (yuyv.frag, nv12.frag, etc.). `y_flip=1.0` corrects for CPU memory layout (top-row-first).
2. **Pass 2 — Scaling**: FBO texture → screen at viewport resolution. Shader selected by scale mode. `y_flip=0.0` (FBO is already in GL orientation). Viewport is saved/restored around Pass 1 to preserve aspect ratio letterboxing.

**FSR mode (3-pass):**
1. **Pass 1 — Color conversion**: Same as above → source-resolution FBO.
2. **Pass 2 — EASU**: Source FBO → output-resolution FSR FBO. Uses AMD's official `FsrEasuF()` from vendored `ffx_fsr1.h`. Edge-adaptive spatial upscaling with modified Lanczos-2 kernel.
3. **Pass 3 — RCAS**: FSR FBO → screen. Uses AMD's `FsrRcasF()`. Contrast-adaptive sharpening. Note: RCAS uses `v_uv * textureSize` instead of `gl_FragCoord` to avoid viewport offset issues with letterboxing.

The FSR FBO is lazily allocated/resized when output dimensions change. EASU constants (4× uvec4, packed floats-as-uint-bits) are recomputed on resize via `fsr_easu_con()`.

### Texture Upload Strategy by Format

| Format | Textures | Upload | GL_NEAREST required |
|--------|----------|--------|---------------------|
| YUYV/UYVY | 1 RGBA8 (w/2 × h) | Packed 2-pixel-per-texel | Yes (packed components) |
| NV12 | Y: R8 (w×h) + UV: RG8 (w/2 × h/2) | Two uploads | Y: yes, UV: GL_LINEAR (chroma upsampling) |

### Audio Passthrough

PipeWire `pw_stream` capture from the card's ALSA node → lock-free SPSC ring buffer (16384 samples, ~170ms) → `pw_stream` playback to default output. Auto-detects capture card audio by searching PipeWire registry for nodes matching "Elgato", "Game_Capture", "AVerMedia", or "Cam_Link".

## Key Implementation Details

- **Mixed C/C++**: Everything is C17 except `picker.cpp` (Qt6). The picker exposes a C interface via `extern "C"` in `picker.h`.
- **OpenGL 4.3 Core**: Bumped from 3.3 for `textureGather` (required by FSR EASU). All non-FSR shaders remain GLSL 330; FSR shaders use GLSL 430.
- **Shader `#include` preprocessing**: `shader_preprocess()` in shader.c resolves `#include "file"` directives on the CPU before passing assembled source to `glShaderSource`. Used by FSR shaders to include AMD's `ffx_a.h` and `ffx_fsr1.h`. Existing shaders don't use includes and are loaded via the simpler `shader_load_program()`.
- **Vendored AMD headers**: `shaders/ffx/ffx_a.h` (type/math portability) and `shaders/ffx/ffx_fsr1.h` (EASU+RCAS algorithms) from AMD GPUOpen, MIT licensed. FSR wrapper shaders are thin (~30 lines): define `A_GPU`/`A_GLSL`/`FSR_EASU_F`, declare texture callbacks, include the headers, and call the AMD entry points.
- **FSR constants**: Packed as uvec4 (float bits stored as uint via `f2bits()`). Computed on CPU matching AMD's `FsrEasuCon`/`FsrRcasCon`. Uploaded with `glUniform4uiv`.
- **PipeWire headers**: Require `#define _GNU_SOURCE` and `#pragma GCC diagnostic ignored "-Wpedantic"` due to GNU C extensions in SPA macros.
- **GL function loading**: Uses `GL_GLEXT_PROTOTYPES` + `-lGL` (works on Mesa, no need for glad/glew).
- **Shader discovery**: `find_shader_dir()` in main.c searches `./shaders/` → relative to binary → `/usr/local/share/llccpv/shaders`.
- **Format preference**: NV12 > YUYV > UYVY > MJPEG. NV12 allows higher resolutions on USB (lower bandwidth than 4:2:2).
- **Color range**: BT.601 YUV→RGB conversion with configurable limited (TV, 16-235) or full (PC, 0-255) range. Most HDMI sources use limited.
- **Use `kdesu` instead of `sudo`** for privileged commands (user runs KDE, terminal sudo unavailable in this environment).
