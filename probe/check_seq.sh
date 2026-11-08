#!/bin/bash
# WSL: h264_seq parse-only (qemu) vs FFmpeg trace_headers: slice header bit counts must match for every slice.
cd "$(dirname "$0")/clips"
for c in seq_p_cavlc seq_b_pyramid seq_720p_temporal; do
    echo "== $c"
    PARSE_ONLY=1 qemu-aarch64 ../h264_seq $c.h264 /dev/null > /tmp/pmhw_$c.parse 2>&1
    grep -E "^stream|^parsed|not handled|missing|warning" /tmp/pmhw_$c.parse
    grep '^SLICE' /tmp/pmhw_$c.parse | awk '{print $9}' > /tmp/pmhw_ours.txt
    # FFmpeg: the bit position after the last slice header syntax element = header size incl. NAL header
    ffmpeg -loglevel trace -i $c.h264 -c copy -bsf:v trace_headers -f null - 2>&1 | python3 -I ../ff_header_bits.py > /tmp/pmhw_ff.txt
    echo "slices: ours $(wc -l < /tmp/pmhw_ours.txt), ffmpeg $(wc -l < /tmp/pmhw_ff.txt), differing: $(paste /tmp/pmhw_ours.txt /tmp/pmhw_ff.txt | awk '$1 != $2' | wc -l)"
    grep '^SLICE' /tmp/pmhw_$c.parse | sed -n '1,8p'
done
