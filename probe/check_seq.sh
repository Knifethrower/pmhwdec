#!/bin/bash
# WSL: h264_seq parse-only (qemu) vs FFmpeg trace_headers: slice header bit counts must match for every slice.
cd "$(dirname "$0")/clips"
for c in seq_p_cavlc seq_b_pyramid seq_720p_temporal; do
    echo "== $c"
    PARSE_ONLY=1 qemu-aarch64 ../h264_seq $c.h264 /dev/null > /tmp/pmhwdec_$c.parse 2>&1
    grep -E "^stream|^parsed|not handled|missing|warning" /tmp/pmhwdec_$c.parse
    grep '^SLICE' /tmp/pmhwdec_$c.parse | awk '{print $9}' > /tmp/pmhwdec_ours.txt
    # FFmpeg: the bit position after the last slice header syntax element = header size incl. NAL header
    ffmpeg -loglevel trace -i $c.h264 -c copy -bsf:v trace_headers -f null - 2>&1 | python3 -I ../ff_header_bits.py > /tmp/pmhwdec_ff.txt
    echo "slices: ours $(wc -l < /tmp/pmhwdec_ours.txt), ffmpeg $(wc -l < /tmp/pmhwdec_ff.txt), differing: $(paste /tmp/pmhwdec_ours.txt /tmp/pmhwdec_ff.txt | awk '$1 != $2' | wc -l)"
    grep '^SLICE' /tmp/pmhwdec_$c.parse | sed -n '1,8p'
done
