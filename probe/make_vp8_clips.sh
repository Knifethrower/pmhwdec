#!/bin/bash
# WSL: VP8 test clips (libvpx through FFmpeg, IVF) + FFmpeg framecrc references, in probe/clips. Each clip covers one
# part of the format: inter frames with golden / alt-ref updates, hidden alt-ref frames, token partitions, segments,
# the three simpler bitstream profiles, an odd picture size, and 720p / 1080p real pictures.
set -euo pipefail
cd "$(dirname "$0")/clips"
SRC=mkv/bbb_h264_51.mkv
TEST="-f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 90"
BBB="-ss 5 -t 10 -i $SRC -an -r 30"
ref() { ffmpeg -loglevel error -y -i "$1.ivf" -fps_mode passthrough -pix_fmt nv12 -f framecrc "$1.ref.crc"
        echo "$1: $(stat -c %s "$1.ivf") bytes, $(grep -vc '^#' "$1.ref.crc") pictures"; }
enc() { local name=$1; shift; [ -f "$name.ivf" ] || ffmpeg -loglevel error -y "$@" -f ivf "$name.ivf"; ref "$name"; }
VPX="-c:v libvpx -pix_fmt yuv420p"
enc vp8_inter      $TEST $VPX -deadline good -cpu-used 1 -auto-alt-ref 0 -g 30 -b:v 1M
enc vp8_altref     $TEST $VPX -deadline good -cpu-used 1 -auto-alt-ref 1 -lag-in-frames 16 -arnr-maxframes 7 -g 60 -b:v 800k
enc vp8_tokens     $TEST $VPX -deadline good -cpu-used 1 -slices 8 -auto-alt-ref 0 -b:v 2M
enc vp8_segments   $TEST $VPX -deadline realtime -cpu-used 5 -error-resilient 1 -b:v 700k -g 45
enc vp8_profile1   $TEST $VPX -profile:v 1 -deadline good -cpu-used 2 -auto-alt-ref 0 -b:v 1M
enc vp8_profile2   $TEST $VPX -profile:v 2 -deadline good -cpu-used 2 -auto-alt-ref 0 -b:v 1M
enc vp8_profile3   $TEST $VPX -profile:v 3 -deadline good -cpu-used 2 -auto-alt-ref 0 -b:v 1M
enc vp8_odd_size   -f lavfi -i testsrc2=size=318x202:rate=30 -frames:v 60 $VPX -deadline good -cpu-used 1 -auto-alt-ref 1 -lag-in-frames 10 -b:v 400k
enc vp8_720p       $BBB -vf scale=1280:720 $VPX -deadline good -cpu-used 2 -auto-alt-ref 1 -lag-in-frames 16 -b:v 3M
enc vp8_1080p      $BBB -vf scale=1920:1080 $VPX -deadline good -cpu-used 3 -auto-alt-ref 1 -lag-in-frames 16 -b:v 5600k
