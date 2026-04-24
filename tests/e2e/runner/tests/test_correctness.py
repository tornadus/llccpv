"""Correctness e2e tests (1–5).

Happy-path frames flow through feeder → v4l2loopback → llccpv → PPM dump.
Each test checks either a per-pixel color or a region mean against the
expected RGB, with tolerance that absorbs BT.601 fp rounding and any
driver-side clamping.
"""

import time

from runner.harness import (
    PPM, assert_pixel, assert_region_mean, ctx, load_ppm,
)


def _run_solid(format_name, expected_rgb):
    """Feed a 640×480 solid color in `format_name`, dump, assert region mean."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format=format_name, width=640, height=480, fps=30,
            duration=3.0,
            pattern=f"solid:{expected_rgb[0]},{expected_rgb[1]},{expected_rgb[2]}",
        )
        time.sleep(0.4)  # let feeder set format + write a few frames
        dump = c.tmp("solid.ppm")
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=2500, frames=15, dump_frame=dump,
            range_mode="full",
        )
        rc = llccpv.wait(timeout=6)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc}:\n{llccpv.stderr_text()}"
        ppm = load_ppm(dump)
        assert (ppm.w, ppm.h) == (640, 480), f"dims {ppm.w}×{ppm.h}"
        # Center region (avoid any edge artifacts).
        assert_region_mean(ppm, 280, 220, 360, 260, expected_rgb, tol=5,
                           label=f"{format_name}_solid")


def test_solid_red_yuyv_640x480():
    _run_solid("yuyv", (255, 0, 0))


def test_solid_green_uyvy_640x480():
    _run_solid("uyvy", (0, 255, 0))


def test_solid_blue_nv12_640x480():
    _run_solid("nv12", (0, 0, 255))


def test_gradient_yuyv_monotonic():
    """Horizontal Y ramp: left edge ≈ black, right edge ≈ white, and the
    row mean brightness must increase monotonically left→right."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30, duration=3.0,
            pattern="gradient",
        )
        time.sleep(0.4)
        dump = c.tmp("gradient.ppm")
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=2500, frames=15, dump_frame=dump,
            range_mode="full",
        )
        rc = llccpv.wait(timeout=6)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc}:\n{llccpv.stderr_text()}"

        ppm = load_ppm(dump)
        # Column means of the center row band: monotone increasing.
        band_y0, band_y1 = 200, 280
        cols = 16
        col_w = ppm.w // cols
        means = []
        for ci in range(cols):
            x0 = ci * col_w
            x1 = x0 + col_w
            s = 0
            for y in range(band_y0, band_y1):
                for x in range(x0, x1):
                    r, g, b = ppm.pixel(x, y)
                    s += (r + g + b) // 3
            means.append(s / ((band_y1 - band_y0) * col_w))
        # Must be strictly non-decreasing column to column.
        for i in range(1, len(means)):
            assert means[i] >= means[i - 1] - 2, (
                f"gradient broke monotonicity at col {i}: {means}")
        # Left edge dark, right edge bright.
        assert means[0]  < 40,  f"left edge not dark: {means[0]}"
        assert means[-1] > 215, f"right edge not bright: {means[-1]}"


def _run_res(w, h):
    """Happy path at a given resolution with a bars pattern; just sanity
    check that the dump has the right dims and is non-uniform."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=w, height=h, fps=30, duration=3.0,
            pattern="bars",
        )
        time.sleep(0.4)
        dump = c.tmp(f"res_{w}x{h}.ppm")
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=2500, frames=15, dump_frame=dump,
            range_mode="full",
        )
        rc = llccpv.wait(timeout=8)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc}:\n{llccpv.stderr_text()}"
        ppm = load_ppm(dump)
        assert (ppm.w, ppm.h) == (w, h), f"dims {ppm.w}×{ppm.h} != {w}×{h}"
        # Bars pattern has 8 bars; first and last should differ substantially.
        left  = ppm.pixel(w // 16, h // 2)   # inside bar 0 (white)
        right = ppm.pixel(w - w // 16, h // 2) # inside bar 7 (black)
        left_lum  = sum(left)  // 3
        right_lum = sum(right) // 3
        assert left_lum > 200 and right_lum < 40, (
            f"bars not discriminated: left={left}({left_lum}) right={right}({right_lum})")


def test_res_320x240():
    _run_res(320, 240)


def test_res_1280x720():
    _run_res(1280, 720)


def test_res_1920x1080():
    _run_res(1920, 1080)
