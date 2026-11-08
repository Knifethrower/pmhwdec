#!/bin/bash
# WSL: extra clips for the library test (multi-slice) + framecrc references for every 8-bit clip used by run_lib.sh
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)/clips
cd "$D"
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 60 -c:v libx264 -pix_fmt yuv420p \
    -x264-params "slices=4:bframes=3:ref=3:keyint=30" h264_slices.h264
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 60 -c:v libx265 -pix_fmt yuv420p \
    -x265-params "log-level=error:slices=4:keyint=30:open-gop=0:bframes=3" hevc_slices.hevc
for f in seq_p_cavlc.h264 seq_b_pyramid.h264 seq_720p_temporal.h264 h264_slices.h264 hevc_slices.hevc; do
    ffmpeg -loglevel error -y -i $f -pix_fmt nv12 -f framecrc ${f%.*}.ref.crc
done
ls -la *.ref.crc
