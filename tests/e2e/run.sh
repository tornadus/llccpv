#!/bin/bash
# Entry point for the llccpv e2e test suite.
#
# Verifies prereqs, builds targets, runs the Python harness.
# Accepts all arguments of tests/e2e/runner/main.py (e.g. --stress, -k, --list).

set -e

cd "$(dirname "$0")/../.."
REPO=$(pwd)

# Prereq: the loopback device must exist.
if ! grep -l -E '^llccpv-test$' /sys/devices/virtual/video4linux/video*/name 2>/dev/null >/dev/null; then
    echo "e2e: no v4l2loopback device named 'llccpv-test'."
    echo "Run: tests/e2e/setup_device.sh  (one-time, needs root)"
    exit 2
fi

# Build llccpv (and the e2e-only 'feeder' target).
meson compile -C build llccpv feeder >/dev/null

# Run tests. All args pass through.
python3 "$REPO/tests/e2e/runner/main.py" "$@"
