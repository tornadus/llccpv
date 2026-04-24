# llccpv test suite

Three unit-test binaries run under `meson test`, plus an opt-in
end-to-end suite that drives the full pipeline via v4l2loopback:

| Suite            | Covers                                                      | GL required |
|------------------|-------------------------------------------------------------|-------------|
| `pixel`          | Render pipeline: color conversion, scaling, FSR invariants  | yes         |
| `audio`          | SPSC ring buffer (ring.c)                                   | no          |
| `capture_mailbox`| Capture mailbox protocol                                    | no          |
| `e2e` (separate) | Full capture → render → dump via v4l2loopback; edge cases + stress | yes  |

```
meson compile -C build
meson test    -C build                      # unit + e2e (skips e2e cleanly if no loopback)
meson test    -C build --verbose pixel      # single unit suite
meson test    -C build --verbose e2e        # just the e2e suite, per-test output

tests/e2e/run.sh --stress                   # + soak/stress (~12 min; not in meson test)
```

See [`tests/e2e/README.md`](e2e/README.md) for e2e setup (requires
`v4l2loopback` + one-time device creation). Stress tests are separate
because they take ~12 min — too long to belong in default `meson test`.

`test_pixel` opens a hidden SDL3 window to create a real OpenGL 4.3 Core
context, then drives the production render code with synthetic YUV frames.
Expected values come from a CPU reference implementation of the same BT.601
math used by the shaders (`tests/support/ref_yuv.c`).

## Tolerances

| Pass                               | Tolerance |
|------------------------------------|-----------|
| Conversion (mid-range colors)      | `|d| <= 2` per channel (fp rounding) |
| Conversion (clamp-boundary values) | `|d| <= 1` |
| Nearest scaling                    | bit-exact |
| Bilinear / sharp bilinear identity | `|d| <= 2` |
| FSR                                | invariants only — no per-pixel compare |

FSR is not bit-exact across GPUs. `test_pixel` asserts only:
dimensions match, solid-in/solid-out holds, nothing spills outside the
viewport, and RCAS sharpness 2.0 is strictly softer than sharpness 0.0.

## Convincing yourself the suite catches regressions

Walk these five deliberate breakages to verify the suite isn't asleep.
Each one should cause exactly the listed failures, and the suite must go
back green after you revert.

### 1. Wrong BT.601 coefficient

File: `shaders/yuyv.frag` — change `1.402` to `1.500`.

Expected: `yuyv_solid_full_colors` fails on `mid_warm_full` (R=159 vs
154, diff 5). UYVY and NV12 tests still pass (separate shaders).

### 2. Wrong limited-range offset

File: `shaders/nv12.frag` — change `16.0/255.0` to `0.0/255.0`.

Expected: `nv12_solid_colors` fails on `nv12_black_lim` (Y=16 no longer
maps to black, gets 19 instead of 0). YUYV and UYVY limited-range tests
still pass.

### 3. Y-flip disabled

File: `src/render.c` — in `render_draw`, change the conv-pass call to
`glUniform1f(ctx->conv_loc_y_flip, 0.0f)`.

Expected: `yuyv_y_flip_preserves_row_order` fails — row 0 of the
readback is now the bottom of the source image, not the top.

### 4. Nearest filter swapped for linear

File: `src/render.c`, `setup_fbo()` — force `GLenum filter = GL_LINEAR`.

Expected: `scale_nearest_2x_upscale_bit_exact` fails at column 1 (R=83
vs 41). Identity case still passes because GL_LINEAR at texel centers
produces the exact texel value.

### 5. Sharp bilinear kernel broken

File: `shaders/sharp_bilinear.frag` — replace the `smoothstep(...)`
with `vec2 blend = vec2(1.0)` to force a one-texel shift.

Expected: `scale_sharp_bilinear_identity` fails (actual=210 vs conv=166,
diff 44).

## Known gap

Swapping `smoothstep(...)` for `step(0.5, frac)` in `shaders/sharp_bilinear.frag`
currently slips past the suite. GPU fp interpolation evaluates `frac` at
pixel centers as a value *slightly below* 0.5, so `step(0.5, frac)`
returns 0 (matching `smoothstep`'s identity behavior at frac≈0.5). A
dedicated "blend-region" test at a non-integer upscale ratio — where
`frac` lands clearly away from 0.5 — would close this gap. Fault #5
above (`blend = vec2(1.0)`) confirms the suite catches the broader
kernel-shape regression.

## Regenerating fixtures

All fixtures are generated at runtime from plain C (`tests/support/fixtures.c`).
No binary assets are checked in.
