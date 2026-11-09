#!/bin/bash
# WSL: hevc_seq parse-only (qemu) vs FFmpeg trace_headers: slice header sizes must match for every slice.
cd "$(dirname "$0")/clips"
for c in hevc_intra hevc_pb hevc_pb_wpp hevc_720p_10bit bbb8 bbb10; do
    echo "== $c"
    PARSE_ONLY=1 qemu-aarch64 ../hevc_seq $c.hevc /dev/null > /tmp/pmhwdec_$c.parse 2>&1
    grep -E "^stream|^parsed|not handled|missing|bad|without|RASL" /tmp/pmhwdec_$c.parse | sort | uniq -c | head -5
    grep '^SLICE' /tmp/pmhwdec_$c.parse | awk '{print $9}' > /tmp/pmhwdec_ours.txt
    ffmpeg -loglevel trace -i $c.hevc -c copy -bsf:v trace_headers -f null - 2>&1 | python3 -I ../ff_header_bits.py > /tmp/pmhwdec_ff.txt
    echo "slices: ours $(wc -l < /tmp/pmhwdec_ours.txt), ffmpeg $(wc -l < /tmp/pmhwdec_ff.txt), differing: $(paste /tmp/pmhwdec_ours.txt /tmp/pmhwdec_ff.txt | awk '$1 != $2' | wc -l)"
    paste /tmp/pmhwdec_ours.txt /tmp/pmhwdec_ff.txt | awk '$1 != $2 {print "  first diff at slice " NR ": ours " $1 " ffmpeg " $2; exit}'
    grep '^SLICE' /tmp/pmhwdec_$c.parse | sed -n '1,6p'
done
