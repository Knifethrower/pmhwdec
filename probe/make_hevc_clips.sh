#!/bin/bash
# WSL: HEVC test clips (x265, one slice per picture, closed GOP) + the BBB 1080p clips; FFmpeg framecrc references.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)/clips
mkdir -p "$D"; cd "$D"
enc() { ffmpeg -loglevel error -y -f lavfi -i "$1" -frames:v "$2" -c:v libx265 -pix_fmt "$3" -x265-params "log-level=error:$4" "$5.hevc"; }
# intra only, no WPP
enc "testsrc2=size=640x360:rate=30" 10 yuv420p "keyint=1:no-wpp=1:open-gop=0" hevc_intra
# P/B, weighted P, no WPP
enc "testsrc2=size=640x360:rate=30" 60 yuv420p "keyint=30:no-wpp=1:open-gop=0:bframes=3:ref=3:weightp=1" hevc_pb
# same with WPP (entry points)
enc "testsrc2=size=640x360:rate=30" 60 yuv420p "keyint=30:wpp=1:open-gop=0:bframes=3:ref=3" hevc_pb_wpp
# 720p textured motion, B-pyramid, 10-bit
enc "mandelbrot=size=1280x720:rate=30" 30 yuv420p10le "keyint=60:open-gop=0:bframes=4:b-pyramid=1:ref=4" hevc_720p_10bit
# real footage: first 300 frames of the BBB clips from the Jellyfin cedar work, and BBB H.264 1080p
for c in bbb8 bbb10; do
    ffmpeg -loglevel error -y -i ~/jms/$c.hevc -frames:v 300 -c copy -bsf:v hevc_mp4toannexb $c.hevc 2>/dev/null ||
        ffmpeg -loglevel error -y -i ~/jms/$c.hevc -frames:v 300 -c copy $c.hevc
done
ffmpeg -loglevel error -y -i ~/jms/bbb.h264 -frames:v 300 -c copy bbb_h264.h264
# references: per-frame adler32 of the decoded NV12 (8-bit; 10-bit streams: see compare_crc.py)
for c in hevc_intra hevc_pb hevc_pb_wpp bbb8 bbb_h264; do
    f=$c.hevc; [ -f $f ] || f=$c.h264
    ffmpeg -loglevel error -y -i $f -pix_fmt nv12 -f framecrc $c.ref.crc
done
for c in hevc_720p_10bit bbb10; do
    ffmpeg -loglevel error -y -i $c.hevc -frames:v 30 -pix_fmt p010le -f rawvideo $c.ref.p010
done
ls -la hevc_* bbb*
