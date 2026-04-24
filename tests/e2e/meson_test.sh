#!/bin/bash
# Meson-invoked e2e test entry. TAP protocol.
# - If the llccpv-test loopback device is present, run the quick e2e suite
#   and emit per-test TAP so meson shows individual subtest status.
# - Otherwise, emit a single "ok # SKIP" entry so meson reports it as
#   skipped (non-fatal) with a pointer to tests/e2e/README.md.

cd "$(dirname "$0")/../.."
REPO=$(pwd)

if ! grep -l -E '^llccpv-test$' /sys/devices/virtual/video4linux/video*/name 2>/dev/null >/dev/null; then
    echo "1..1"
    echo "ok 1 # SKIP no v4l2loopback device 'llccpv-test'; see tests/e2e/README.md"
    exit 0
fi

exec python3 "$REPO/tests/e2e/runner/main.py" --tap
