"""Stress / soak tests (18–20).

Opt-in via `main.py --stress`. Each of these is long-running (minutes).
"""

import os
import time

from runner.harness import (
    ctx, pid_fd_count, pid_rss_kb, wait_for_log,
)


# ---------- 18+19. Long soak: RSS and fd-leak check in one run ----------

def test_soak_10min_60fps_rss_and_fd_stable():
    """Feed 60 fps for 10 minutes. Sample VmRSS and /proc/PID/fd count every
    10 s. After a 60 s warmup, final RSS must be within 10% of the warmup
    baseline and fd count must be stable (no leak).
    """
    DURATION_S = 600
    WARMUP_S   = 60
    SAMPLE_S   = 10
    RSS_GROWTH_LIMIT = 0.10  # 10%

    with ctx() as c:
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=60,
            duration=DURATION_S + 30, pattern="bars",
        )
        time.sleep(0.5)
        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=(DURATION_S + 10) * 1000,
            range_mode="full",
        )
        assert wait_for_log(llccpv, "Capture streaming started", timeout=5)

        rss_baseline = None
        fd_baseline = None
        samples = []

        start = time.monotonic()
        while True:
            t = time.monotonic() - start
            if t >= DURATION_S:
                break
            if t >= WARMUP_S and rss_baseline is None:
                rss_baseline = pid_rss_kb(llccpv.pid)
                fd_baseline  = pid_fd_count(llccpv.pid)
                print(f"   baseline t={t:.0f}s RSS={rss_baseline} KB fd={fd_baseline}",
                      flush=True)
            rss = pid_rss_kb(llccpv.pid)
            fd  = pid_fd_count(llccpv.pid)
            samples.append((t, rss, fd))
            if rss is None:
                break
            if len(samples) % 6 == 0:
                print(f"   t={t:.0f}s RSS={rss} KB fd={fd}", flush=True)
            time.sleep(SAMPLE_S)

        rc = llccpv.wait(timeout=30)
        feeder.terminate()
        feeder.wait(timeout=5)

        assert rc == 0, f"llccpv exit {rc}"
        assert rss_baseline is not None, "didn't capture baseline"
        final_rss = samples[-1][1]
        growth = (final_rss - rss_baseline) / rss_baseline
        print(f"   final RSS={final_rss} KB baseline={rss_baseline} "
              f"growth={growth*100:+.2f}%", flush=True)
        assert growth < RSS_GROWTH_LIMIT, (
            f"RSS grew {growth*100:.2f}% ({rss_baseline}→{final_rss} KB)"
        )

        # fd count: allow +1 over baseline for noise; must not keep growing.
        max_fd = max(s[2] for s in samples if s[2] is not None)
        assert max_fd <= fd_baseline + 1, (
            f"fd leak: baseline={fd_baseline} max={max_fd}"
        )


# ---------- 20. Feeder restart churn ----------

def test_feeder_restart_20x_over_2min():
    """Kill + respawn the feeder 20 times over 2 minutes. llccpv runs the
    entire time and must not crash; it should pick up each new stream.

    v4l2loopback requires a producer to establish the format before any
    consumer can S_FMT, so we start feeder #0 first and only spawn llccpv
    after the device has a negotiated format. Subsequent feeder churn
    happens while llccpv is live."""
    with ctx() as c:
        total_s = 120
        n_cycles = 20
        cycle_s = total_s / n_cycles  # 6 s per cycle

        # Feeder #0 — establish the format for llccpv's S_FMT.
        feeder = c.spawn_feeder(
            format="yuyv", width=640, height=480, fps=30,
            duration=cycle_s + 0.5, pattern="gradient",
        )
        time.sleep(0.4)

        llccpv = c.spawn_llccpv(
            headless=True, exit_after_ms=(total_s + 10) * 1000,
            range_mode="full",
        )
        from runner.harness import wait_for_log
        assert wait_for_log(llccpv, "Capture streaming started", timeout=5), \
            "llccpv never reached streaming"

        # Cycle: kill current feeder, spawn next, sleep until next cycle.
        for i in range(n_cycles):
            time.sleep(cycle_s - 0.6)
            feeder.kill()
            feeder.wait(timeout=2)
            if i < n_cycles - 1:
                time.sleep(0.3)  # brief gap — llccpv should survive it
                feeder = c.spawn_feeder(
                    format="yuyv", width=640, height=480, fps=30,
                    duration=cycle_s + 1.0, pattern="gradient",
                )

        rc = llccpv.wait(timeout=30)
        assert rc == 0, f"llccpv exit {rc}\n{llccpv.stderr_text()[-500:]}"
