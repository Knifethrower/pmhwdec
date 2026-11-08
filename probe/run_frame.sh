#!/bin/sh
# Device side: run h264_frame on every clip in this folder, outputs <clip>.hw.nv12 next to them.
# Refuses while anything else has /dev/cedar_dev open. Usage: sh run_frame.sh <dir with h264_frame + clips>
D=${1:-$(dirname "$0")}
for p in /proc/[0-9]*; do
    if ls -l "$p/fd" 2>/dev/null | grep -q cedar_dev; then echo "engine in use by $(cat "$p/comm"): not testing"; exit 2; fi
done
for c in main_cabac base_cavlc high_720p; do
    echo "== $c"
    "$D/h264_frame" "$D/$c.h264" "$D/$c.hw.nv12"
done
dmesg 2>/dev/null | grep -iE "cedar|ve:|iommu" | tail -8
