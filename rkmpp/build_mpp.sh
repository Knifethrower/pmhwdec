#!/bin/bash
# WSL: cross-builds Rockchip MPP (librockchip_mpp.so, Apache-2.0 / MIT) for aarch64 against the bullseye sysroot.
#   bash build_mpp.sh             -> ../out/aarch64/mpp/ (librockchip_mpp.so.1 + LICENSE.mpp.txt); headers in ~/pmhw/mpp/inc
#   ARCH=armhf bash build_mpp.sh  -> ../out/armhf/mpp/ (32-bit, buster armhf sysroot, glibc 2.28)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ARCH=${ARCH:-aarch64}
OUT=$HERE/../out/$ARCH/mpp
case "$ARCH" in
  aarch64) SYSROOT=${SYSROOT:-$HOME/chroot-bullseye}; TRIPLE=aarch64-linux-gnu; PROC=aarch64; CPU="-mcpu=cortex-a53"; LFIX= ;;
  armhf)   SYSROOT=${SYSROOT:-$HOME/chroot-buster-armhf}; TRIPLE=arm-linux-gnueabihf; PROC=arm
           CPU="-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard"
           # the buster sysroot's libpthread.so / libdl.so / libm.so / librt.so are absolute symlinks out of it
           LFIX=$HOME/pmhw/armhf-libfix; mkdir -p "$LFIX"
           for l in pthread.so.0 dl.so.2 m.so.6 rt.so.1; do ln -sf "$SYSROOT/lib/arm-linux-gnueabihf/lib$l" "$LFIX/lib${l%%.so*}.so"; done
           LFIX="-L$LFIX" ;;
  *) echo "ARCH must be aarch64 or armhf"; exit 1 ;;
esac
SRC=${SRC:-$HOME/pmhw/mpp}
REPO=https://github.com/rockchip-linux/mpp.git
TAG=${TAG:-1.1.0}               # 1.0.11 is broken upstream (av1 CMakeLists names a missing file)
command -v cmake >/dev/null || { echo "cmake missing (apt install cmake ninja-build)"; exit 1; }
if [ -d "$SRC/.git" ] && [ "$(git -C "$SRC" describe --tags 2>/dev/null)" != "$TAG" ]; then rm -rf "$SRC"; fi
if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$(dirname "$SRC")"
    git clone -q --depth 1 --branch "$TAG" "$REPO" "$SRC"
fi
git -C "$SRC" log -1 --format='mpp %H %cd' > "$HERE/MPP_VERSION.txt"
python3 -I "$HERE/patch_soc_name.py" "$SRC"     # RG552-style device trees: see the script
echo "patched: osal/mpp_soc.c read_soc_name() keeps plain rockchip,rkNNNN compatibles (pmhwdec rkmpp/patch_soc_name.py)" >> "$HERE/MPP_VERSION.txt"
python3 -I "$HERE/patch_hw_list.py" "$SRC"      # MPP_HW_LIST: only the decoder IPs below
echo "patched: mpp/soc.cmake takes MPP_HW_LIST (pmhwdec rkmpp/patch_hw_list.py)" >> "$HERE/MPP_VERSION.txt"
python3 -I "$HERE/patch_no_enc.py" "$SRC"       # no encoder framework: see the script
echo "patched: encoder framework out of the CMake lists, mpp/codec/pmhw_noenc.c stubs (pmhwdec rkmpp/patch_no_enc.py)" >> "$HERE/MPP_VERSION.txt"
python3 -I "$HERE/patch_no_dbg_log.py" "$SRC"   # mpp_dbg() traces compile out unless MPP_DBG_LOG=1
echo "patched: osal/inc/mpp_debug.h takes PMHW_NO_DBG_LOG (pmhwdec rkmpp/patch_no_dbg_log.py)" >> "$HERE/MPP_VERSION.txt"
cat > "$SRC/../$ARCH.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $PROC)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET $TRIPLE)
set(CMAKE_CXX_COMPILER_TARGET $TRIPLE)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld $LFIX")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld $LFIX -Wl,--no-as-needed -lm")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
B=$SRC/../mpp-build; [ "$ARCH" = aarch64 ] || B=$SRC/../mpp-build-$ARCH
rm -rf "$B" && mkdir -p "$B"
# only what libpmhwdec drives (mpp/codecs.cmake options): H.264, HEVC and VP8 decode; no other decoder, no encoder
CODECS="-DENABLE_H264D=ON -DENABLE_H265D=ON -DENABLE_VP8D=ON"
for c in AVSD AVS2D H263D MPEG2D MPEG4D VP9D JPEGD AV1D H264E H265E JPEGE VP8E; do CODECS="$CODECS -DENABLE_$c=OFF"; done
# decoder IPs (mpp/soc.cmake MPP_HW_TYPES) = which HAL files are built; each HAL registers only on the SoCs in its
# .soc_type list. PortMaster's Rockchip chips (DESIGN.md) use:
#   VDPU2   hal_h264d_vdpu2   RK3326/PX30 H.264 (vpu_service); also listed for RK3399/3328/356x/3588 as 2nd choice
#   VDPU341 hal_h264d_rkv_reg RK3399/RK3328 H.264 (rkvdec); hal_h265d_rkv: RK3326/PX30 HEVC (hevc_service) and
#                             RK3399/RK3328 HEVC (rkvdec)
#   VDPU34X hal_h26[45]d_vdpu34x  RK3566/7/8 + RK3588 H.264/HEVC (the RK3588 VDPU381 runs this HAL)
# left out: VDPU1 (RK3036/3066/3188/3288/312x/3368), VDPU382 (RK3528/3562), VDPU383 (RK3576), VDPU384A (RV1126B),
# VDPU384B (RK3572/3538/3539), VDPU381/720/730/AV1D_VPU (AVS2/JPEG/AV1 only) and every encoder.
CODECS="$CODECS -DMPP_HW_LIST=VDPU2,VDPU341,VDPU34X"
# export only what src/rkmpp.c dlsym()s (its SYM(field, "name") list), so --gc-sections drops the rest of the public
# API as well; every stock MPP has these, so a system librockchip_mpp swapped in still works.
MAP=$SRC/../mpp_api.map
API=$(grep -oE 'SYM\([a-z_0-9]+, *"[a-z_0-9]+"\)' "$HERE/../src/rkmpp.c" | grep -oE '"[a-z_0-9]+"' | tr -d '"' | sort -u)
[ "$(echo "$API" | wc -w)" -ge 20 ] || { echo "symbol list not found in src/rkmpp.c"; exit 1; }
{ echo "{ global:"; echo "$API" | sed 's/$/;/'; echo "local: *; };"; } > "$MAP"
# -Os (Release defaults to -O3): MPP's CPU side is header parsing + register setup, the decode is the hardware's.
# No unwind tables: MPP is plain C (no C++, no pthread_cancel / backtrace), .eh_frame was 87 KB.
# Measured 2026-11-07 (stripped / gzip -9): full 2.87 MB; other codecs off 1.35 MB; public-API exports + gc + -Os 1,089,984
# (464 KB); rkmpp.c-only exports 996,200 (429 KB); HW list 902,816 (394 KB); no unwind tables 816,256 (370 KB);
# no encoder framework 593,968 (279 KB); no mpp_dbg traces 539,088 (255 KB). Details: DESIGN.md.
# The linker flags replace the toolchain file's _INIT ones, so lld / libm are repeated here. -z defs: a symbol a
# patch removed but something still names fails the link, not the dlopen().
SECT="-ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables"
DBG=-DPMHW_NO_DBG_LOG; [ "${MPP_DBG_LOG:-0}" = 1 ] && DBG= && echo "MPP_DBG_LOG=1: mpp_dbg() traces kept"
cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$SRC/../$ARCH.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TEST=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON $CODECS \
    -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld $LFIX -Wl,--no-as-needed -lm -Wl,-z,defs -Wl,--gc-sections -Wl,--version-script=$MAP" \
    -DCMAKE_C_FLAGS="$CPU -fPIC -w $SECT $DBG" -DCMAKE_CXX_FLAGS="$CPU -fPIC -w $SECT $DBG" >/dev/null
ninja -C "$B" rockchip_mpp 2>&1 | grep -E "error|FAILED" | head -20 || true
mkdir -p "$OUT"
SO=$(find "$B" -name 'librockchip_mpp.so*' -type f | head -1)
[ -n "$SO" ] || { echo "build failed"; exit 1; }
cp -L "$SO" "$OUT/librockchip_mpp.so"
llvm-strip "$OUT/librockchip_mpp.so"
llvm-readelf -d "$OUT/librockchip_mpp.so" | grep -E "SONAME|NEEDED"
{ echo "librockchip_mpp.so.1: Rockchip MPP, $(cat "$HERE/MPP_VERSION.txt"), $REPO (tag $TAG)."; echo;
  cat "$SRC/LICENSES/Apache-2.0" 2>/dev/null || cat "$SRC/LICENSE.md" 2>/dev/null || ls "$SRC"; } > "$OUT/LICENSE.mpp.txt"
ls -la "$OUT"
