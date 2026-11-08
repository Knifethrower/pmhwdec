#!/bin/bash
# WSL: H.264 sequences with P/B frames (one slice per picture) + FFmpeg reference NV12 (display order), into clips/
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)/clips
mkdir -p "$D"; cd "$D"
enc() { ffmpeg -loglevel error -y -f lavfi -i "$1" -frames:v "$2" -c:v libx264 -pix_fmt yuv420p -x264-params "$3" "$4.h264"; }
# P only, CAVLC, POC type 2, 3 refs
enc "testsrc2=size=640x360:rate=30" 60 "slices=1:bframes=0:ref=3:cabac=0:keyint=30" seq_p_cavlc
# B pyramid + weighted P + multiple refs, CABAC, 8x8, two IDRs
enc "testsrc2=size=640x360:rate=30" 60 "slices=1:bframes=3:b-pyramid=normal:ref=4:weightp=2:keyint=30" seq_b_pyramid
# textured motion, 720p, temporal direct + explicit weighted B
enc "mandelbrot=size=1280x720:rate=30" 30 "slices=1:bframes=2:ref=3:direct=temporal:weightb=1:keyint=60" seq_720p_temporal
for c in seq_p_cavlc seq_b_pyramid seq_720p_temporal; do
    ffmpeg -loglevel error -y -i $c.h264 -pix_fmt nv12 -f rawvideo $c.ref.nv12
done
ls -la seq_*
