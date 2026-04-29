# llccpv

Low Latency Capture Card Preview for Linux

## Features

- Low-latency GPU-accelerated capture card preview
- Support for YUV 4:2:2 and 4:2:0 inputs
- FSR 1 spatial upscaling (and other more traditional upscaling modes)
- Qt-based GUI settings dialog

## Requirements

- Linux
- A GPU with OpenGL 4.3 support or higher
- A supported capture card (see below)

### Dependencies

- SDL3
- OpenGL 4.3+
- PipeWire
- Qt6 (picker)
- meson, gcc/g++

## Build

```bash
meson setup build
meson compile -C build
```

### Fedora Dependencies

```bash
sudo dnf install -y SDL3-devel mesa-libGL-devel pipewire-devel qt6-qtbase-devel meson gcc g++
```

## Usage

```bash
./build/llccpv                                      # launch picker dialog
./build/llccpv -d /dev/video0                       # bypass picker
./build/llccpv -d /dev/video0 -S fsr -P 0.2 -r limited -f
```

### Command-line options


| Flag | Long form | Argument | Description |
|------|-----------|----------|-------------|
| `-d` | `--device` | `PATH` | V4L2 device path (default: `/dev/video0`) |
| `-a` | `--audio-source` | `NAME` | PipeWire audio source name (auto-detects capture card audio if omitted) |
| `-n` | `--no-audio` | | Disable audio passthrough |
| `-f` | `--fullscreen` | | Start in fullscreen |
| `-s` | `--stretch` | | Stretch to fill window, ignoring aspect ratio |
| `-S` | `--scale` | `MODE` | Scale mode: `nearest`, `bilinear` (default), `sharp`, `fsr` |
| `-P` | `--sharpness` | `VALUE` | FSR sharpness: `0.0` (max) to `2.0` (soft), default `0.2` |
| `-r` | `--range` | `MODE` | Color range: `limited` (default, TV/16-235) or `full` (PC/0-255) |
| `-c` | `--matrix` | `MODE` | YUV→RGB matrix: `auto` (default, detected from V4L2), `bt601`, or `bt709` |
| `-v` | `--vsync` | `MODE` | Vsync: `0` off, `1` on, `-1` adaptive (default) |
| `-h` | `--help` | | Show help and exit |


## Supported capture cards


- Elgato HD60 X (Verified)
- Most UVC cards (subject to format support)
- Anything else that happens to work via V4L2 and outputs the right format


## Roadmap


- MJPEG support (May as well cover everything a user might encounter, no matter how crappy...)


## License

MIT

