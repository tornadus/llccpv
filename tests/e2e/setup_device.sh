#!/bin/bash
# One-time setup for the llccpv e2e test device. Requires root (uses kdesu
# per the project's convention).

set -e

DEV=/dev/video42
NAME=llccpv-test

if [ -e "$DEV" ]; then
    cur=$(cat "/sys/devices/virtual/video4linux/$(basename $DEV)/name" 2>/dev/null || echo "")
    if [ "$cur" = "$NAME" ]; then
        echo "e2e loopback already present: $DEV ($NAME)"
        exit 0
    fi
    echo "ERROR: $DEV exists but is named '$cur' (expected '$NAME')."
    echo "Remove it first: kdesu v4l2loopback-ctl delete $DEV"
    exit 1
fi

echo "Creating $DEV as v4l2loopback device '$NAME' (exclusive caps)..."
kdesu -c "v4l2loopback-ctl add -n '$NAME' -x 1 '$DEV'"

if [ ! -e "$DEV" ]; then
    echo "ERROR: $DEV not created."
    exit 2
fi

echo "Done. Device ready: $DEV"
ls -l "$DEV"
