#!/bin/bash
# WSL: software-decode benchmark clips (20 s, 30 fps, streaming-style bitrates) from the Big Buck Bunny source in
# probe/clips/mkv -> probe/clips/bench/*.mkv. Used by tools/sw_bench_dev.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC=$HERE/probe/clips/mkv/bbb_h264_51.mkv
OUT=$HERE/probe/clips/bench
mkdir -p "$OUT"
enc() {   # name scale codec-args
    local name=$1 scale=$2; shift 2
    [ -f "$OUT/$name.mkv" ] && return 0
    ffmpeg -hide_banner -loglevel error -y -ss 5 -t 20 -i "$SRC" -an -vf "scale=$scale:flags=lanczos" -r 30 "$@" "$OUT/$name.mkv"
    echo "$name: $(du -h "$OUT/$name.mkv" | cut -f1)"
}
X265="-c:v libx265 -preset fast -x265-params log-level=error"
enc h264_720p    1280:720  -c:v libx264 -preset medium -profile:v high -b:v 3M -maxrate 4.5M -bufsize 6M -pix_fmt yuv420p
enc h264_1080p   1920:1080 -c:v libx264 -preset medium -profile:v high -b:v 6M -maxrate 9M -bufsize 12M -pix_fmt yuv420p
enc hevc_720p    1280:720  $X265 -b:v 2M -pix_fmt yuv420p
enc hevc_1080p   1920:1080 $X265 -b:v 4M -pix_fmt yuv420p
enc hevc10_1080p 1920:1080 $X265 -b:v 4M -pix_fmt yuv420p10le -profile:v main10
