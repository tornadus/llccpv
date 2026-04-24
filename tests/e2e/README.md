# llccpv end-to-end test suite

Drives the full capture → render → dump pipeline against a `v4l2loopback`
device. Exercises the real `capture.c` + main event loop under adversarial
conditions (pauses, producer death, mid-stream resolution changes,
malformed frames, high / low fps, rapid spawn, stress-soak).

## One-time setup

### Prerequisites

- `v4l2loopback` kernel module (Fedora: `dnf install v4l2loopback`).
- `v4l2loopback-ctl` userspace tool (same package).
- `python3` (stdlib only — no pip deps).

### Create the test device

```bash
tests/e2e/setup_device.sh       # uses kdesu (root-required)
```

This creates `/dev/video42` named `llccpv-test` with exclusive capture /
output capabilities. Persists until reboot or explicit removal.

To tear down:

```bash
kdesu v4l2loopback-ctl delete /dev/video42
```

## Running

Quick run (~90 s, correctness + edge cases):

```bash
tests/e2e/run.sh
```

Include stress / soak (~12 min added — 10 min RSS/fd soak + 2 min feeder
churn):

```bash
tests/e2e/run.sh --stress
```

Filter:

```bash
tests/e2e/run.sh -k solid         # only tests whose name contains 'solid'
tests/e2e/run.sh --list            # list without running
```

## What's covered

| Group        | Tests                                                                        | What fails if broken                                |
|--------------|------------------------------------------------------------------------------|-----------------------------------------------------|
| Correctness  | solid R/G/B in YUYV/UYVY/NV12, gradient monotonicity, 320/720/1080 resolutions | YUV→RGB math, format wiring, scaling defaults     |
| Edge cases   | 3 s pause/recover, feeder SIGKILL, resolution change, 120 fps, 1 fps, short/junk frames, MJPEG, 50× spawn, SIGTERM, two-opener contention, reopen | capture thread lifecycle, event loop, buffer mgmt |
| Stress       | 10 min @ 60 fps (RSS + fd), 20× feeder restart over 2 min                    | memory / fd leaks, churn resilience                 |

## Meson integration

`meson test` includes an `e2e` TAP test:

- **Device present** — runs the full quick e2e suite (19 tests, ~70 s).
  Each individual test appears as a subtest in meson's output.
- **Device missing** — emits a single `ok # SKIP` with a pointer to this
  README. Non-fatal; the rest of `meson test` is unaffected.

Stress tests (10-min soak, 2-min restart) remain opt-in via
`tests/e2e/run.sh --stress` — they take too long to belong in a default
`meson test` run.

## Architecture

```
                                                 llccpv
 tests/e2e/feeder/feeder.c  ──▶  /dev/video42  ──▶  (capture)
 (C, write()-based V4L2                             (render)
  OUTPUT producer)                                  (dump PPM)
                                                     │
                                 tests/e2e/runner/main.py  ◀─┘
                                 (Python, discovers test_*
                                  in runner/tests/*, spawns
                                  feeder + llccpv, asserts)
```

**Feeder** (`tests/e2e/feeder/feeder.c`):
- Opens the loopback sink with `O_WRONLY`, calls `VIDIOC_S_FMT` to
  negotiate format, then `write()`s synthesized frames at the target fps
  via `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, …)`.
- Supports YUYV, UYVY, NV12, MJPEG (declared only).
- Patterns: solid color, horizontal Y-ramp, SMPTE-like bars, checker.
- Scheduled hooks: `--pause-at`, `--rate-at`, `--malform-at`, `--reopen-at`.
- Emits machine-parseable stderr events:
  `feeder: t=<ms> frame=<N> action=<verb>`.

**llccpv test-mode flags** (`src/main.c`):
- `--exit-after <ms>` — schedule an `SDL_EVENT_QUIT` timer.
- `--frames <N>` — exit after `N` frames rendered.
- `--headless` — create the SDL window with `SDL_WINDOW_HIDDEN`.
- `--dump-frame <path>` — on each new frame, `glReadPixels` the
  backbuffer into a P6 PPM (rows top-first). Final file = last frame.

**Python harness** (`tests/e2e/runner/harness.py`):
- `Process` — subprocess.Popen wrapper with background stderr draining.
- `PPM` / `load_ppm` — P6 parser.
- `assert_pixel`, `assert_region_mean` — tolerance-based checks.
- `pid_rss_kb`, `pid_fd_count`, `pid_cpu_pct` — `/proc`-based sampling.
- `ctx()` — tmpdir + process bookkeeping; cleans up on exit.

## Known limitation: SOURCE_CHANGE events

`llccpv` subscribes to `V4L2_EVENT_SOURCE_CHANGE` so real capture cards
can push a resolution change mid-stream and have `llccpv` reinitialize
transparently. `v4l2loopback` doesn't implement this event (it logs
`VIDIOC_SUBSCRIBE_EVENT failed: Invalid argument` at startup).

The `test_feeder_resizes_midstream` test therefore only asserts that
`llccpv` **doesn't crash** when a producer at one resolution quits and a
producer at a different resolution takes over on the same loopback device.
Transparent reinit (without restart) is only testable against a real
capture card.

## Adding a new test

1. Drop a `test_<name>` function in
   `tests/e2e/runner/tests/test_correctness.py`,
   `test_edge_cases.py`, or `test_stress.py`.
2. Use `ctx()` for lifecycle; spawn via `c.spawn_feeder(...)` /
   `c.spawn_llccpv(...)`.
3. Raise `AssertionError` on failure; return cleanly on success.
4. Run `tests/e2e/run.sh --list` to confirm it's picked up.

No test discovery beyond `runner.tests.*` + `test_*` name prefix — no
pytest, no fixtures. Keep them simple and self-contained.

## CPU / RSS thresholds

Stress thresholds are set generously to avoid false-failing on CI-like
VMs:

- `high_fps_120`: RSS growth < 10%, CPU < 85%
- `low_fps_1`: CPU < 15% (event loop must be event-driven, not polling)
- `soak_10min_60fps`: RSS growth < 10% from 60 s-warmup baseline; fd
  count ≤ baseline + 1 throughout

If a real regression is introduced and these thresholds don't catch it,
tighten them in `tests/e2e/runner/tests/test_edge_cases.py` or
`test_stress.py`.
