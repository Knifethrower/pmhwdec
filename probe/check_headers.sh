#!/bin/bash
# WSL: our slice header bit count (qemu) next to FFmpeg's trace_headers bit positions, per test clip
cd "$(dirname "$0")/clips"
for c in main_cabac base_cavlc high_720p; do
    echo "== $c"
    qemu-aarch64 ../h264_frame $c.h264 /dev/null 2>&1 | grep '^stream'
    ffmpeg -loglevel trace -i $c.h264 -frames:v 1 -c copy -bsf:v trace_headers -f null - 2>&1 |
        grep -E "Slice Header|slice_qp_delta|slice_beta_offset|slice_alpha_c0|disable_deblocking_filter_idc" | head -5
done
