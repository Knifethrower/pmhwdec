#!/bin/sh
# Device side: HEVC clips + BBB H.264 1080p through the engine; logs + a few frames next to the clips.
# Refuses while anything else has /dev/cedar_dev open. Usage: sh run_hevc.sh <dir>
D=${1:-$(dirname "$0")}
for p in /proc/[0-9]*; do
    if ls -l "$p/fd" 2>/dev/null | grep -q cedar_dev; then echo "engine in use by $(cat "$p/comm"): not testing"; exit 2; fi
done
free -m | sed -n 2p
for c in hevc_intra hevc_pb hevc_pb_wpp hevc_720p_10bit bbb8 bbb10; do
    echo "== $c"
    DUMP_FRAMES=3 "$D/hevc_seq" "$D/$c.hevc" "$D/$c.hw.nv12" > "$D/$c.log" 2>&1
    grep -E "^stream|^RESULT|not handled|missing|bad |without|engine:" "$D/$c.log" | head -8
done
echo "== bbb_h264"
DUMP_FRAMES=3 "$D/h264_seq" "$D/bbb_h264.h264" "$D/bbb_h264.hw.nv12" > "$D/bbb_h264.log" 2>&1
grep -E "^stream|^RESULT|not handled|missing|engine:" "$D/bbb_h264.log" | head -8
