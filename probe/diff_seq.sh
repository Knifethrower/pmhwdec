#!/bin/bash
# WSL: first slices where our header size differs from FFmpeg's, with FFmpeg's trace of that slice
cd "$(dirname "$0")/clips"
c=${1:-seq_b_pyramid}
PARSE_ONLY=1 qemu-aarch64 ../h264_seq $c.h264 /dev/null | grep '^SLICE' > /tmp/pmhwdec_o.txt
ffmpeg -loglevel trace -i $c.h264 -c copy -bsf:v trace_headers -f null - 2>&1 > /dev/null | cat > /tmp/pmhwdec_trace.txt
python3 -I ../ff_header_bits.py < /tmp/pmhwdec_trace.txt > /tmp/pmhwdec_f.txt
paste -d'|' /tmp/pmhwdec_o.txt /tmp/pmhwdec_f.txt | awk -F'|' '{split($1,a," "); if (a[9] != $2) print NR": ours "a[9]" ffmpeg "$2"  "$1}' | head -4
n=$(paste -d'|' /tmp/pmhwdec_o.txt /tmp/pmhwdec_f.txt | awk -F'|' '{split($1,a," "); if (a[9] != $2) {print NR; exit}}')
echo "--- FFmpeg trace of slice $n"
awk -v want="$n" '/\] Slice Header$/{k++} k==want' /tmp/pmhwdec_trace.txt | sed -n '1,45p' | sed 's/^\[trace_headers @ [^]]*\] //'
