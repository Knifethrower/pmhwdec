#!/bin/bash
# Builds out/pmhwdec-test-kit.zip: "PMHWDEC Test.sh" + pmhwdec-test/ (pmhw-test with libpmhwdec linked in, Rockchip
# MPP, the test clips + FFmpeg references, run.sh, licenses) for testers. Unzip into the ports folder, run it from
# the front end (or `sh pmhwdec-test/run.sh` over SSH), send back ports/pmhwdec-report.txt.
set -euo pipefail
ZIP=${KIT_ZIP:-}   # optional output path (default out/pmhwdec-test-kit.zip)
TEST_ARCH=${KIT_ARCH:-aarch64}   # armhf: the 32-bit pmhw-test (ARCH=armhf build.sh) with the 64-bit pmhw-cedard
P=$(cd "$(dirname "$0")/.." && pwd)
O=$P/out/aarch64
W=$(mktemp -d)
K=$W/pmhwdec-test
mkdir -p "$K/clips" "$K/licenses"
cp "$P/kit/PMHWDEC Test.sh" "$W/"
cp "$P/kit/pmhwdec-test/run.sh" "$P/out/$TEST_ARCH/pmhw-test" "$O/pmhw-cedard" "$K/"   # pmhw-cedard: the Allwinner decoder (GPL helper)
[ -f "$P/out/$TEST_ARCH/mpp/librockchip_mpp.so" ] && cp "$P/out/$TEST_ARCH/mpp/librockchip_mpp.so" "$K/"   # found next to pmhw-test
for f in seq_p_cavlc.h264 seq_b_pyramid.h264 seq_720p_temporal.h264 h264_slices.h264 bbb_h264.h264 \
         hevc_intra.hevc hevc_pb.hevc hevc_pb_wpp.hevc hevc_slices.hevc hevc_720p_10bit.hevc bbb8.hevc bbb10.hevc; do
    cp "$P/probe/clips/$f" "$K/clips/"
    [ -f "$P/probe/clips/${f%.*}.ref.crc" ] && cp "$P/probe/clips/${f%.*}.ref.crc" "$K/clips/"
done
cp "$P/LICENSE" "$K/licenses/LICENSE.pmhwdec.txt"
cp "$P/rkmpp/LICENSE.mpp.txt" "$K/licenses/LICENSE.mpp.txt" 2>/dev/null || true
echo "$(date +%Y-%m-%d) $(md5sum "$P/out/$TEST_ARCH/pmhw-test" | cut -c1-8)" > "$K/VERSION"
chmod 755 "$W" "$K" "$W/PMHWDEC Test.sh" "$K/run.sh" "$K/pmhw-test" "$K/pmhw-cedard"
ZIP=${ZIP:-$P/out/pmhwdec-test-kit.zip}
rm -f "$ZIP"
( cd "$W" && zip -qr -X "$ZIP" . )
rm -rf "$W"
ls -la "$ZIP"
unzip -l "$ZIP" | tail -1
