#!/bin/sh
# libpmhwdec test kit: what this device's video decoder does with libpmhwdec, as one report to paste back.
# Runs the startup self-test, then decodes the test clips through the library and compares every frame with
# FFmpeg's decode. Changes nothing on the device (the self-test result goes to /tmp, removed at the end).
# usage: sh run.sh [report file]   (the launcher next to this folder calls it)
K=$(cd "$(dirname "$0")" && pwd)
R=${1:-$K/../pmhwdec-report.txt}
if [ "$(id -u)" != 0 ] && sudo -n true 2>/dev/null; then exec sudo -n sh "$0" "$R"; fi
C=/tmp/pmhwdec-selftest.$(id -u)
PAUSE=/var/run/battery-saver/pmhwdec-kit.pause    # Knulli: no idle suspend while it runs
if [ -d /etc/idlewatcher ]; then mkdir -p /var/run/battery-saver && touch "$PAUSE"; fi
trap 'rm -f "$PAUSE" "$C"' EXIT INT TERM
rm -f "$C"
exec > "$R" 2>&1

line() { echo "---------------- $*"; }
first() { [ -r "$1" ] && head -c "${2:-300}" "$1" | tr '\0' ' '; echo; }

line "libpmhwdec test kit $(cat "$K/VERSION" 2>/dev/null), $(date)"
echo "model:      $(first /proc/device-tree/model)"
echo "compatible: $(first /proc/device-tree/compatible)"
echo "kernel:     $(uname -a)"
echo "user:       $(id)"
for f in /etc/os-release /usr/share/plymouth/themes/text.plymouth /etc/hostname; do
    [ -r "$f" ] && { echo "$f:"; head -6 "$f" | sed 's/^/    /'; }
done
grep -m1 -iE "^(Hardware|model name)" /proc/cpuinfo
echo "memory: $(awk '/MemTotal|MemAvailable/{printf "%s %s MB  ", $1, int($2/1024)}' /proc/meminfo)"

line "decoder device files"
ls -l /dev/mpp_service /dev/rkvdec /dev/hevc_service /dev/vpu_service /dev/vpu-service /dev/cedar_dev /dev/ion \
      /dev/dma_heap/* /dev/media* /dev/video* 2>/dev/null
for v in /sys/class/video4linux/*; do [ -r "$v/name" ] && echo "$(basename "$v"): $(cat "$v/name")"; done

line "capabilities + self-test"
PMHWDEC_LOG=1 PMHWDEC_SELFTEST=1 "$K/pmhwdec-test" -q
echo "self-test results: $(tr '\n' ';' < "$C" 2>/dev/null)"

line "test clips (every frame compared with FFmpeg's decode)"
for f in seq_p_cavlc.h264 seq_b_pyramid.h264 seq_720p_temporal.h264 h264_slices.h264 bbb_h264.h264 \
         hevc_intra.hevc hevc_pb.hevc hevc_pb_wpp.hevc hevc_slices.hevc hevc_720p_10bit.hevc bbb8.hevc bbb10.hevc; do
    c=${f%.*}
    [ -f "$K/clips/$f" ] || continue
    PMHWDEC_LOG=1 timeout 120 "$K/pmhwdec-test" "$K/clips/$f" > /tmp/pmhwkit.log 2> /tmp/pmhwkit.err
    rc=$?
    res=$(grep '^RESULT' /tmp/pmhwkit.log | sed 's/ send_errors.*per frame:/,/')
    [ -n "$res" ] || res="RESULT none (exit code $rc)"
    if [ -f "$K/clips/$c.ref.crc" ]; then
        exact=$(awk '
            FNR == NR { if ($0 !~ /^#/ && NF) { n = split($0, a, ","); x = a[n]; gsub(/[ \r]/, "", x); ref[r++] = tolower(x) }; next }
            /^FRAME/ { for (i = 1; i <= NF; i++) if ($i ~ /^adler=/) ours[o++] = "0x" tolower(substr($i, 7)) }
            END {
                m = r < o ? r : o; bad = 0
                for (i = 0; i < m; i++) if (ref[i] != ours[i]) bad++
                printf "%d of %d frames bit-exact (%d decoded, %d in the reference)", m - bad, m, o, r
            }' "$K/clips/$c.ref.crc" /tmp/pmhwkit.log)
    else
        exact="10-bit: no reference here (the self-test checks it)"
    fi
    printf "%-20s %s | %s\n" "$c" "$res" "$exact"
    grep -m3 -vE "^pmhwdec: (using|cedar: engine|cedar: [0-9]|cedar: picture buffers|self-test .*: OK)" /tmp/pmhwkit.err | sed 's/^/    /'
done
rm -f /tmp/pmhwkit.log /tmp/pmhwkit.err

line "kernel messages about the decoder (last 15)"
dmesg 2>/dev/null | grep -iE "vpu|vdec|mpp|cedar|hantro|rkvdec|venus|iris|v4l2|oops|paging request" | tail -15
line "end"
