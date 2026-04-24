"""Minimal test runner for llccpv e2e tests.

Discovers functions named test_* in runner.tests.test_*, runs them
sequentially, prints pass/fail, exits nonzero on any failure.

Usage:
  python3 tests/e2e/runner/main.py              # quick-run (correctness + edge)
  python3 tests/e2e/runner/main.py --stress     # include stress / soak
  python3 tests/e2e/runner/main.py -k substring # run only matching tests
  python3 tests/e2e/runner/main.py --list       # list tests without running
"""

from __future__ import annotations

import argparse
import importlib
import inspect
import os
import pkgutil
import sys
import time
import traceback
from pathlib import Path

# Ensure `import runner` works whether we're run from repo root, tests/e2e,
# or anywhere else.
HERE = Path(__file__).resolve().parent
PKG_PARENT = HERE.parent           # tests/e2e/
if str(PKG_PARENT) not in sys.path:
    sys.path.insert(0, str(PKG_PARENT))


def discover(stress: bool):
    """Import runner.tests.* and yield (module_name, fn_name, callable)."""
    import runner.tests as tests_pkg
    modules = []
    for info in pkgutil.iter_modules(tests_pkg.__path__, prefix="runner.tests."):
        if info.name.endswith(".test_stress") and not stress:
            continue
        if info.name.endswith(".test_stress"):
            pass  # explicitly included
        modules.append(importlib.import_module(info.name))

    for mod in modules:
        for name, obj in inspect.getmembers(mod):
            if name.startswith("test_") and callable(obj):
                yield mod.__name__, name, obj


def run_human(tests, fail_fast):
    """Human-readable runner. Returns 0 on all-pass, 1 otherwise."""
    n_total = len(tests)
    n_pass = 0
    failures = []
    t0 = time.monotonic()
    for mod, name, fn in tests:
        print(f"-- {name} ({mod.split('.')[-1]})", flush=True)
        t = time.monotonic()
        try:
            fn()
        except Exception as e:
            elapsed = time.monotonic() - t
            print(f"   FAIL  ({elapsed:.2f}s)  {type(e).__name__}: {e}")
            traceback.print_exc()
            failures.append((name, e))
            if fail_fast:
                break
            continue
        elapsed = time.monotonic() - t
        print(f"   ok    ({elapsed:.2f}s)")
        n_pass += 1
    wall = time.monotonic() - t0
    print(f"\n{n_pass}/{n_total} passed in {wall:.1f}s")
    if failures:
        print(f"FAILED: {[f[0] for f in failures]}")
    return 0 if not failures else 1


def run_tap(tests, fail_fast):
    """TAP 13 runner. Meson test with protocol='tap' consumes this."""
    print(f"1..{len(tests)}", flush=True)
    failures = 0
    for i, (mod, name, fn) in enumerate(tests, 1):
        t = time.monotonic()
        try:
            fn()
            elapsed = time.monotonic() - t
            print(f"ok {i} - {name} # {elapsed:.2f}s", flush=True)
        except Exception as e:
            elapsed = time.monotonic() - t
            print(f"not ok {i} - {name} # {elapsed:.2f}s", flush=True)
            # TAP-safe diagnostic: every line prefixed with '#' so any
            # parser tolerates it. meson surfaces these as the test's
            # failure reason.
            print(f"# {type(e).__name__}: {e}")
            for tb_line in traceback.format_exc().rstrip().split("\n"):
                print(f"# {tb_line}")
            import sys; sys.stdout.flush()
            failures += 1
            if fail_fast:
                break
    return 0 if failures == 0 else 1


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--stress", action="store_true",
                    help="Include stress / soak tests (long-running).")
    ap.add_argument("-k", "--filter", default=None,
                    help="Only run tests whose name contains this substring.")
    ap.add_argument("--list", action="store_true",
                    help="List tests without running.")
    ap.add_argument("--fail-fast", action="store_true")
    ap.add_argument("--tap", action="store_true",
                    help="Emit TAP 13 output (for meson test protocol='tap').")
    args = ap.parse_args(argv)

    tests = list(discover(args.stress))
    if args.filter:
        tests = [t for t in tests if args.filter in t[1]]

    if args.list:
        for mod, name, _ in tests:
            print(f"{mod}::{name}")
        return 0

    if args.tap:
        return run_tap(tests, args.fail_fast)
    return run_human(tests, args.fail_fast)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
