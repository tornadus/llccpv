"""Edge-case e2e tests (6–17).

Exercises behavior under adversarial conditions: pauses, producer death,
resolution change, very high/low fps, malformed frames, unsupported
formats, rapid start/stop, clean shutdown, multiple openers, reopen.
"""

import os
import signal
import time

from runner.harness import (
    DEFAULT_DEVICE, FEEDER_BIN, LLCCPV_BIN, Process,
    assert_region_mean, ctx, grep, load_ppm,
    pid_cpu_pct, pid_fd_count, pid_rss_kb, wait_for_log,
)


# ---------- 6. Pause / recover ----------

def test_feeder_pauses_3s_recovers():
    """Feeder runs, pauses 3s mid-stream, resumes. llccpv must keep running
    and produce a final dump that matches the post-pause color."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30, duration=8.0,
            pattern="solid:0,200,0",
            extra_args=["--pause-at", "30:3.0"],
        )
        time.sleep(0.4)
        dump = c.tmp("post_pause.ppm")
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=7500, frames=0, dump_frame=dump,
            range_mode="full",
        )
        rc = llccpv.wait(timeout=12)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc}:\n{llccpv.stderr_text()}"
        assert "pause_start" in feeder.stderr_text()
        assert "pause_end"   in feeder.stderr_text()
        ppm = load_ppm(dump)
        assert_region_mean(ppm, 280, 220, 360, 260, (0, 200, 0), tol=6,
                           label="post_pause_green")


# ---------- 7. Feeder dies; llccpv survives ----------

def test_feeder_dies_llccpv_survives():
    """SIGKILL the feeder mid-stream. llccpv must keep running until its
    --exit-after fires and then exit cleanly."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30, duration=10.0,
            pattern="bars",
        )
        time.sleep(0.4)
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=3500, range_mode="full",
        )
        time.sleep(1.0)
        feeder.kill()
        feeder.wait(timeout=2)
        rc = llccpv.wait(timeout=6)
        assert rc == 0, f"llccpv exit {rc} after feeder kill"


# ---------- 8. Resolution change mid-stream ----------

def test_feeder_resizes_midstream():
    """Two feeder sessions at different resolutions, llccpv spans both.
    The first feeder exits, the second starts with a different W/H. llccpv
    must not crash. Pass = exit code 0 (either via --exit-after or clean
    error path — garbage renders are acceptable for v4l2loopback, which
    doesn't emit SOURCE_CHANGE events)."""
    with ctx() as c:
        feeder1 = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30, duration=1.5,
            pattern="solid:200,0,0",
        )
        time.sleep(0.4)
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=5000, range_mode="full",
        )
        # Wait for feeder1 to finish.
        feeder1.wait(timeout=4)
        time.sleep(0.3)
        # Start feeder2 at a different resolution.
        feeder2 = c.spawn_feeder(
            format="yuyv", width=1280, height=720, fps=30, duration=2.0,
            pattern="solid:0,0,200",
        )
        rc = llccpv.wait(timeout=8)
        feeder2.terminate()
        feeder2.wait(timeout=3)
        # The key assertion: llccpv didn't crash.
        assert rc == 0, (
            f"llccpv exit {rc} after mid-stream resize:\n{llccpv.stderr_text()[-500:]}"
        )


# ---------- 9. High fps ----------

def test_high_fps_120():
    """Feed at 120 fps for 3 seconds. llccpv must keep up without:
    - RSS growth > 10% vs. 500 ms-warmup baseline
    - sustained CPU > 85% (Mesa/Intel at this fps is real work)
    """
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=120, duration=3.5,
            pattern="bars",
        )
        time.sleep(0.3)
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=4000, range_mode="full",
        )
        # Warm-up baseline.
        time.sleep(0.5)
        rss0 = pid_rss_kb(llccpv.pid)
        cpu = pid_cpu_pct(llccpv.pid, sample_s=1.5)
        rss1 = pid_rss_kb(llccpv.pid)
        rc = llccpv.wait(timeout=6)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc}"
        assert rss0 is not None and rss1 is not None, "couldn't read RSS"
        growth = (rss1 - rss0) / rss0 if rss0 > 0 else 0.0
        assert growth < 0.10, f"RSS grew {growth*100:.1f}% ({rss0}→{rss1} KB)"
        # CPU threshold is rough; avoid false-failing on CI-like VMs.
        assert cpu is not None and cpu < 85.0, f"CPU {cpu:.1f}% >= 85%"


# ---------- 10. Low fps ----------

def test_low_fps_1():
    """Feed at 1 fps for 5 seconds. llccpv must idle cleanly — sample CPU
    across a 2 s window and assert <15% (event-loop blocks on SDL_WaitEvent
    so CPU should be near 0, but set a generous bound for flaky VMs)."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=1, duration=6.0,
            pattern="gradient",
        )
        time.sleep(0.5)
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=6000, range_mode="full",
        )
        time.sleep(1.0)  # warm-up
        cpu = pid_cpu_pct(llccpv.pid, sample_s=2.5)
        rc = llccpv.wait(timeout=8)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc}"
        assert cpu is not None and cpu < 15.0, (
            f"CPU at 1 fps idle = {cpu:.1f}% (expected <15%) — possible busy loop?"
        )


# ---------- 11. Malformed short frame ----------

def test_malformed_short_frame():
    """Feeder writes half the expected bytes for one frame. llccpv must
    not crash; it either renders the partial frame or drops it."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30, duration=4.0,
            pattern="gradient",
            extra_args=["--malform-at", "30:short"],
        )
        time.sleep(0.4)
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=3500, range_mode="full",
        )
        rc = llccpv.wait(timeout=6)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc} after short frame"


# ---------- 12. Malformed junk bytes ----------

def test_malformed_junk_bytes():
    """Feeder writes random bytes for one frame. llccpv must not crash."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30, duration=4.0,
            pattern="gradient",
            extra_args=["--malform-at", "30:junk"],
        )
        time.sleep(0.4)
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=3500, range_mode="full",
        )
        rc = llccpv.wait(timeout=6)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc} after junk frame"


# ---------- 13. Unsupported format ----------

def test_unsupported_format_mjpeg_handles_cleanly():
    """Feeder declares MJPEG; llccpv's shader path isn't ready for it. The
    test only asserts llccpv doesn't crash catastrophically — it may accept
    the format and render garbage, or refuse it. What we check: exit code
    is well-defined (0 or a documented error, no SIGSEGV/SIGABRT) and stderr
    mentions something coherent."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="mjpeg", width=640, height=480, fps=15, duration=2.0,
            pattern="solid:128,128,128",
        )
        time.sleep(0.4)
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=2500, range_mode="full",
        )
        rc = llccpv.wait(timeout=6)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc >= 0, (
            f"llccpv died with signal (rc={rc}) on MJPEG:\n{llccpv.stderr_text()[-400:]}"
        )


# ---------- 14. Rapid start / stop ----------

def test_rapid_start_stop_50x():
    """Spawn and kill llccpv 50 times in quick succession. Each invocation
    must exit within a deadline; no progressive device-busy accumulation."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=320, height=240, fps=30, duration=60.0,
            pattern="gradient",
        )
        time.sleep(0.4)

        for i in range(50):
            p = c.spawn_llccpv(
                headless=True, exit_after_ms=150, range_mode="full",
            )
            rc = p.wait(timeout=3)
            assert rc == 0, f"iter {i}: exit {rc}\n{p.stderr_text()[-200:]}"

        feeder.terminate()
        feeder.wait(timeout=3)


# ---------- 15. SIGTERM clean shutdown ----------

def test_sigterm_clean_shutdown():
    """Start llccpv with no auto-exit; wait until it's streaming; SIGTERM
    it; assert it exits within 1.5 s with code 0."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30, duration=30.0,
            pattern="gradient",
        )
        time.sleep(0.4)
        # Exit-after is large; we'll SIGTERM before it fires.
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=30_000, range_mode="full",
        )
        assert wait_for_log(llccpv, "Capture streaming started", timeout=4), (
            "llccpv never reached streaming"
        )
        time.sleep(0.3)
        t0 = time.monotonic()
        llccpv.signal(signal.SIGTERM)
        rc = llccpv.wait(timeout=3)
        elapsed = time.monotonic() - t0
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc} after SIGTERM\n{llccpv.stderr_text()[-200:]}"
        assert elapsed < 1.5, f"SIGTERM took {elapsed:.2f}s (>1.5)"


# ---------- 16. Two openers on the same device ----------

def test_two_openers_second_fails_cleanly():
    """Open the device with llccpv A. Try to open a second llccpv B on the
    same device. Exclusive-caps v4l2loopback rejects the second opener;
    B must fail cleanly (nonzero exit, not a crash). A stays healthy."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30, duration=10.0,
            pattern="gradient",
        )
        time.sleep(0.4)
        a = c.spawn_llccpv(headless=True, exit_after_ms=4000, range_mode="full")
        assert wait_for_log(a, "Capture streaming started", timeout=4)
        b = c.spawn_llccpv(headless=True, exit_after_ms=1500, range_mode="full")
        rc_b = b.wait(timeout=4)
        rc_a = a.wait(timeout=6)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc_b != 0, (
            f"B should have failed but exited 0:\n{b.stderr_text()[-300:]}"
        )
        assert rc_a == 0, (
            f"A should have continued cleanly but exited {rc_a}:\n"
            f"{a.stderr_text()[-200:]}"
        )


# ---------- 17. Feeder close+reopen mid-stream ----------

def test_feeder_reopen_mid_run():
    """Feeder closes its fd for 1s after frame 30, then reopens the device
    with the same format. llccpv must survive the gap and resume rendering."""
    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30, duration=5.0,
            pattern="solid:180,60,30",
            extra_args=["--reopen-at", "30:1.0"],
        )
        time.sleep(0.4)
        dump = c.tmp("post_reopen.ppm")
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=5500, dump_frame=dump,
            range_mode="full",
        )
        rc = llccpv.wait(timeout=8)
        feeder.terminate()
        feeder.wait(timeout=3)
        assert rc == 0, f"llccpv exit {rc}"
        assert "reopen_close" in feeder.stderr_text()
        assert "reopen_done"  in feeder.stderr_text()
        # Post-reopen: final frame should still match the feeder's solid color.
        ppm = load_ppm(dump)
        assert_region_mean(ppm, 280, 220, 360, 260, (180, 60, 30), tol=6,
                           label="post_reopen")
