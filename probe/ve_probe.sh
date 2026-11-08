#!/bin/sh
# Device side: system facts + ve_probe. Read-only apart from the engine clock reference the probe takes
# and gives back. Usage: sh ve_probe.sh [path to ve_probe binary]   (default: next to this script)
BIN=${1:-$(dirname "$0")/ve_probe}
echo "== system"
uname -a
tr '\0' ' ' < /proc/device-tree/compatible; echo
grep -m1 -iE "hardware|model name" /proc/cpuinfo
echo "== nodes"
ls -l /dev/cedar_dev /dev/ion 2>&1
ls -l /dev/dma_heap 2>/dev/null
ls /sys/class/video4linux 2>/dev/null
echo "== device tree VE node"
for n in /proc/device-tree/*ve* /proc/device-tree/soc*/*ve* /proc/device-tree/*/video-codec*; do
    [ -e "$n/compatible" ] && echo "$n: $(tr '\0' ' ' < "$n/compatible")"
done 2>/dev/null
echo "== kernel log"
dmesg 2>/dev/null | grep -iE "cedar|[^a-z]ve[^a-z]|video.?engine" | tail -15
echo "== users of /dev/cedar_dev"
busy=0
for p in /proc/[0-9]*; do
    if ls -l "$p/fd" 2>/dev/null | grep -q cedar_dev; then
        echo "pid ${p#/proc/} $(cat "$p/comm")"; busy=1
    fi
done
if [ "$busy" = 1 ]; then echo "engine in use: not probing"; exit 2; fi
echo "== probe"
"$BIN"
