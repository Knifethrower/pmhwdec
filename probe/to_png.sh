#!/bin/bash
# WSL: hw NV12 (stride x coded height) -> PNG, cropped to the visible size. args: in.nv12 w h stride coded_h out.png
ffmpeg -loglevel error -y -f rawvideo -pix_fmt nv12 -s "$4x$5" -i "$1" -vf "crop=$2:$3:0:0" "$6"
