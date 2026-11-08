#!/bin/bash
# WSL: test clips (first frame IDR, one slice) + FFmpeg reference NV12 of frame 0, into probe/clips/
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)/clips
mkdir -p "$D"; cd "$D"
src="testsrc2=size=640x360:rate=30"
ffmpeg -loglevel error -y -f lavfi -i "$src" -frames:v 1 -c:v libx264 -profile:v main -x264-params slices=1 -pix_fmt yuv420p main_cabac.h264
ffmpeg -loglevel error -y -f lavfi -i "$src" -frames:v 1 -c:v libx264 -profile:v baseline -x264-params slices=1 -pix_fmt yuv420p base_cavlc.h264
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 1 -c:v libx264 -profile:v high -x264-params slices=1 -pix_fmt yuv420p high_720p.h264
for c in main_cabac base_cavlc high_720p; do
    ffmpeg -loglevel error -y -i $c.h264 -frames:v 1 -pix_fmt nv12 -f rawvideo $c.ref.nv12
done
ls -la
