#!/bin/bash
# WSL: static aarch64 ve_probe (runs on Knulli, muOS and dArkOS alike). Output next to this script.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SYSROOT=${SYSROOT:-$HOME/chroot-bullseye}
clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld -static -O2 -Wall -Wextra \
    -o "$HERE/ve_probe" "$HERE/ve_probe.c"
clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld -static -O2 -Wall -Wextra \
    -o "$HERE/h264_frame" "$HERE/h264_frame.c"
clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld -static -O2 -Wall -Wextra \
    -o "$HERE/h264_seq" "$HERE/h264_seq.c"
clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld -static -O2 -Wall -Wextra \
    -o "$HERE/hevc_seq" "$HERE/hevc_seq.c"
llvm-strip "$HERE/ve_probe" "$HERE/h264_frame" "$HERE/h264_seq" "$HERE/hevc_seq" 2>/dev/null || true
file "$HERE/ve_probe" "$HERE/h264_frame" "$HERE/h264_seq" "$HERE/hevc_seq"
