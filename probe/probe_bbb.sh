#!/bin/bash
# WSL: stream facts of the BBB 1080p clips from the Jellyfin cedar work
for f in ~/jms/bbb.h264 ~/jms/bbb8.hevc ~/jms/bbb10.hevc; do
    echo "== $f $(stat -c %s $f) bytes"
    ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,width,height,pix_fmt,has_b_frames,r_frame_rate -count_frames -show_entries stream=nb_read_frames -of compact=p=0 "$f"
    ffmpeg -loglevel trace -i "$f" -frames:v 4 -c copy -bsf:v trace_headers -f null - 2>&1 |
        grep -E "first_slice_segment_in_pic_flag|first_mb_in_slice|tiles_enabled_flag|entropy_coding_sync_enabled_flag|scaling_list_enabled_flag|long_term_ref_pics_present_flag|num_entry_point_offsets|weighted_pred_flag|nal_unit_type " |
        sed 's/^\[trace_headers @ [^]]*\] *//' | awk '{print $2, $NF}' | sort | uniq -c | head -14
done
