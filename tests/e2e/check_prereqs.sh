#!/bin/bash
# Meson test that reports (via TAP) whether the e2e suite can run.
#
# Looks for the v4l2loopback device named 'llccpv-test'. Prints a SKIP
# directive when missing, so `meson test` shows a helpful note instead
# of failing.

echo "1..1"
if grep -l -E '^llccpv-test$' /sys/devices/virtual/video4linux/video*/name 2>/dev/null >/dev/null; then
    echo "ok 1 - llccpv-test loopback present; run tests/e2e/run.sh"
else
    echo "ok 1 # SKIP e2e disabled: no v4l2loopback device named 'llccpv-test'. See tests/e2e/README.md"
fi
