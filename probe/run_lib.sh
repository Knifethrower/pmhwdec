#!/bin/sh
# Device side: pmhwdec-test (public API only) on every clip in the folder; <clip>.lib.log next to them.
# Refuses while anything else has /dev/cedar_dev open. Usage: sh run_lib.sh <dir>
D=${1:-$(dirname "$0")}
# dArkOS logs in as ark; the decoder nodes may be root-only: rerun as root when sudo needs no password
if [ "$(id -u)" != 0 ] && [ -z "$PMHWDEC_NO_SUDO" ] && sudo -n true 2>/dev/null; then exec sudo -n sh "$0" "$@"; fi
echo "running as $(id -un); nodes: $(ls -l /dev/mpp_service /dev/rkvdec /dev/hevc_service /dev/vpu_service /dev/cedar_dev 2>/dev/null | awk '{print $1, $3, $4, $NF}' | tr '\n' ';')"
for p in /proc/[0-9]*; do
    if ls -l "$p/fd" 2>/dev/null | grep -q cedar_dev; then echo "engine in use by $(cat "$p/comm"): not testing"; exit 2; fi
done
# keep the device awake: Knulli's idlewatcher hooks skip dim/suspend while a *.pause file exists here
PAUSE=/var/run/battery-saver/pmhwdec.pause
if [ -d /etc/idlewatcher ]; then mkdir -p /var/run/battery-saver && touch "$PAUSE"; trap 'rm -f "$PAUSE"' EXIT INT TERM; fi
"$D/pmhwdec-test" -q
for f in seq_p_cavlc.h264 seq_b_pyramid.h264 seq_720p_temporal.h264 h264_slices.h264 bbb_h264.h264 \
         hevc_intra.hevc hevc_pb.hevc hevc_pb_wpp.hevc hevc_slices.hevc hevc_720p_10bit.hevc bbb8.hevc bbb10.hevc; do
    [ -f "$D/$f" ] || continue
    c=${f%.*}
    PMHWDEC_LOG=1 "$D/pmhwdec-test" "$D/$f" > "$D/$c.lib.log" 2> "$D/$c.lib.err"
    printf "%-20s %s\n" "$c" "$(grep '^RESULT' "$D/$c.lib.log")"
    grep -m2 -vE "^pmhwdec: (using|cedar: engine|cedar: [0-9])" "$D/$c.lib.err" | sed 's/^/    /'
done
