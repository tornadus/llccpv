"""Subprocess / PPM / assertion helpers for llccpv e2e tests.

Pure-stdlib. No pip deps. All tests use a context object produced by
`ctx()` to spawn the feeder and llccpv, capture logs, and clean up.
"""

from __future__ import annotations

import contextlib
import os
import shutil
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT  = Path(__file__).resolve().parents[3]
BUILD_DIR  = REPO_ROOT / "build"
LLCCPV_BIN = BUILD_DIR / "llccpv"
FEEDER_BIN = BUILD_DIR / "feeder"
DEFAULT_DEVICE = os.environ.get("LLCCPV_TEST_DEVICE", "/dev/video42")


# ---------- Process wrapper ----------

class Process:
    """subprocess.Popen wrapper that streams stderr into a captured list."""

    def __init__(self, cmd, label=""):
        self.cmd = cmd
        self.label = label or Path(cmd[0]).name
        self.proc = None
        self.stderr_lines = []
        self._drain_thread = None

    def start(self):
        self.proc = subprocess.Popen(
            self.cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._drain_thread = threading.Thread(target=self._drain, daemon=True)
        self._drain_thread.start()
        return self

    def _drain(self):
        assert self.proc and self.proc.stderr
        for line in self.proc.stderr:
            self.stderr_lines.append(line.rstrip())

    def wait(self, timeout=None):
        try:
            rc = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = self.proc.wait()
            raise TimeoutError(f"{self.label} exceeded timeout={timeout}")
        if self._drain_thread:
            self._drain_thread.join(timeout=1.0)
        return rc

    def terminate(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()

    def kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()

    def signal(self, sig):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(sig)

    @property
    def pid(self):
        return self.proc.pid if self.proc else None

    @property
    def returncode(self):
        return self.proc.returncode if self.proc else None

    @property
    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stderr_text(self):
        return "\n".join(self.stderr_lines)


# ---------- PPM loader ----------

@dataclass
class PPM:
    w: int
    h: int
    data: bytes

    def pixel(self, x, y):
        i = (y * self.w + x) * 3
        return (self.data[i], self.data[i + 1], self.data[i + 2])


def load_ppm(path):
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P6":
            raise ValueError(f"not P6: {magic!r}")
        line = f.readline().strip()
        while line.startswith(b"#"):
            line = f.readline().strip()
        w, h = map(int, line.split())
        maxval = int(f.readline().strip())
        if maxval != 255:
            raise ValueError(f"unexpected maxval {maxval}")
        data = f.read(w * h * 3)
        if len(data) != w * h * 3:
            raise ValueError(f"short read: {len(data)} bytes, expected {w*h*3}")
    return PPM(w=w, h=h, data=data)


# ---------- assertions ----------

def assert_pixel(ppm, x, y, expected, tol=4, label=""):
    actual = ppm.pixel(x, y)
    for i, (a, e) in enumerate(zip(actual, expected)):
        if abs(a - e) > tol:
            raise AssertionError(
                f"{label or 'pixel'} ({x},{y}) ch{i}: got {a} expected {e} "
                f"|d|={abs(a - e)} > tol={tol} (actual={actual})"
            )


def assert_region_mean(ppm, x0, y0, x1, y1, expected, tol=6, label=""):
    """Average RGB over [x0, x1) × [y0, y1) must match within tol."""
    r = g = b = 0
    count = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            pr, pg, pb = ppm.pixel(x, y)
            r += pr; g += pg; b += pb; count += 1
    mean = (r // count, g // count, b // count)
    for i, (a, e) in enumerate(zip(mean, expected)):
        if abs(a - e) > tol:
            raise AssertionError(
                f"{label or 'region'} [{x0},{y0}..{x1},{y1}) mean ch{i}: "
                f"got {a} expected {e} |d|={abs(a - e)} > tol={tol} (mean={mean})"
            )


# ---------- Test context ----------

@dataclass
class TestContext:
    device: str = DEFAULT_DEVICE
    tmpdir: Path = field(default_factory=lambda: Path(tempfile.mkdtemp(prefix="llccpv-e2e-")))
    processes: list = field(default_factory=list)

    def tmp(self, name):
        return str(self.tmpdir / name)

    def spawn_feeder(self, *, format, width, height,
                     fps=30.0, duration=3.0, pattern="gradient",
                     extra_args=None):
        cmd = [
            str(FEEDER_BIN),
            "--device", self.device,
            "--format", format,
            "--width", str(width), "--height", str(height),
            "--fps", str(fps),
            "--duration", str(duration),
            "--pattern", pattern,
        ]
        if extra_args:
            cmd.extend(extra_args)
        p = Process(cmd, label="feeder").start()
        self.processes.append(p)
        return p

    def spawn_llccpv(self, *, headless=True, exit_after_ms=2500,
                     frames=0, dump_frame=None,
                     range_mode="full", scale="nearest",
                     device=None, extra_args=None):
        cmd = [
            str(LLCCPV_BIN),
            "-d", device or self.device,
            "--no-audio",
            "--range", range_mode,
            "--scale", scale,
            "--exit-after", str(exit_after_ms),
        ]
        if headless:
            cmd.append("--headless")
        if frames > 0:
            cmd.extend(["--frames", str(frames)])
        if dump_frame:
            cmd.extend(["--dump-frame", dump_frame])
        if extra_args:
            cmd.extend(extra_args)
        p = Process(cmd, label="llccpv").start()
        self.processes.append(p)
        return p

    def cleanup(self):
        for p in self.processes:
            if p.alive:
                p.kill()
                with contextlib.suppress(Exception):
                    p.wait(timeout=1)
        shutil.rmtree(self.tmpdir, ignore_errors=True)


@contextlib.contextmanager
def ctx():
    c = TestContext()
    try:
        yield c
    finally:
        c.cleanup()


# ---------- small helpers ----------

def grep(lines, needle):
    return [l for l in lines if needle in l]


def wait_for_log(proc, needle, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if grep(proc.stderr_lines, needle):
            return True
        time.sleep(0.05)
    return False


def pid_rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except FileNotFoundError:
        return None
    return None


def pid_fd_count(pid):
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except FileNotFoundError:
        return None


def pid_cpu_pct(pid, sample_s=1.0):
    """Rough CPU% over sample_s seconds from /proc/PID/stat."""
    def read_cpu():
        try:
            with open(f"/proc/{pid}/stat") as f:
                fields = f.read().split()
            return int(fields[13]) + int(fields[14]), time.monotonic()
        except FileNotFoundError:
            return None

    s1 = read_cpu()
    if s1 is None: return None
    time.sleep(sample_s)
    s2 = read_cpu()
    if s2 is None: return None
    clk = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    return (s2[0] - s1[0]) / clk / (s2[1] - s1[1]) * 100.0
