#!/bin/sh
# Device side: h264_seq on every seq_* clip in the folder; <clip>.hw.nv12 + <clip>.log next to them.
D=${1:-$(dirname "$0")}
for p in /proc/[0-9]*; do
    if ls -l "$p/fd" 2>/dev/null | grep -q cedar_dev; then echo "engine in use by $(cat "$p/comm"): not testing"; exit 2; fi
done
free -m | sed -n 2p
for c in seq_p_cavlc seq_b_pyramid seq_720p_temporal; do
    echo "== $c"
    "$D/h264_seq" "$D/$c.h264" "$D/$c.hw.nv12" > "$D/$c.log" 2>&1
    grep -E "^stream|^RESULT|ERROR|not handled|missing|warning|engine:" "$D/$c.log"
done
