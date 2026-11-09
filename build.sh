#!/bin/bash
# Builds libpmhwdec.so, libpmhwdec.a, pmhwdec-cedard and pmhwdec-test for aarch64 (WSL: clang + lld, bullseye sysroot,
# glibc 2.31).
#   bash build.sh            -> out/aarch64/
#   ARCH=x86_64 bash build.sh -> out/x86_64/ (host build: compiles and links; no backend finds hardware there)
#   ARCH=armhf bash build.sh  -> out/armhf/ (32-bit programs; buster armhf sysroot, glibc 2.28): libpmhwdec + pmhwdec-test
#     only. pmhwdec-cedard stays the aarch64 one: H700 / A133 handhelds run 64-bit kernels, and the 64-bit helper drives
#     the engine natively (cedar_ipc.h is the same on both). Ship out/aarch64/pmhwdec-cedard next to the armhf library.
# libpmhwdec (LGPL-2.1+) and pmhwdec-cedard (GPL-2.0+, the Cedrus-derived Allwinner decoder) are separate programs:
# the library never links cedar.c, it starts pmhwdec-cedard next to itself (src/cedar_client.c).
set -euo pipefail
LIBS="-lpthread -ldl"
HERE=$(cd "$(dirname "$0")" && pwd)
ARCH=${ARCH:-aarch64}
OUT=$HERE/out/$ARCH
mkdir -p "$OUT/obj" "$OUT/obj-cedard"
if [ "$ARCH" = aarch64 ]; then
    SYSROOT=${SYSROOT:-$HOME/chroot-bullseye}
    CC="clang --target=aarch64-linux-gnu --sysroot=$SYSROOT -mcpu=cortex-a53"
    LDFLAGS="-fuse-ld=lld"
    STRIP=llvm-strip
elif [ "$ARCH" = armhf ]; then
    SYSROOT=${SYSROOT:-$HOME/chroot-buster-armhf}
    CC="clang --target=arm-linux-gnueabihf --sysroot=$SYSROOT -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard"
    # the sysroot's libpthread.so / libdl.so are absolute symlinks out of it: link the shared ones by soname
    LDFLAGS="-fuse-ld=lld -L$SYSROOT/lib/arm-linux-gnueabihf"
    LIBS="-l:libpthread.so.0 -l:libdl.so.2"
    STRIP=llvm-strip
else
    CC="clang"
    LDFLAGS=""
    STRIP=strip
fi
CFLAGS="-O2 -fPIC -Wall -Wextra -Werror=implicit-function-declaration -I$HERE/src/uapi -I$HERE/include -I$HERE/src"
MPP_INC=${MPP_INC:-$HOME/pmhwdec/mpp/inc}         # Rockchip MPP headers (rkmpp/build_mpp.sh clones them)
SRCS="pmhwdec selftest stateless h264 hevc vp8 cedar_client rkmpp v4l2req v4l2m2m"
CEDARD_SRCS="cedard cedar stateless h264 hevc vp8"
rm -f "$OUT"/obj/*.o "$OUT"/obj-cedard/*.o
for s in $SRCS; do $CC $CFLAGS -I"$MPP_INC" -c "$HERE/src/$s.c" -o "$OUT/obj/$s.o"; done
[ "$ARCH" = armhf ] && CEDARD_SRCS=""
for s in $CEDARD_SRCS; do $CC $CFLAGS -c "$HERE/src/$s.c" -o "$OUT/obj-cedard/$s.o"; done
# public symbols only (pmhwdec_*)
$CC $LDFLAGS -shared -o "$OUT/libpmhwdec.so" -Wl,-soname,libpmhwdec.so.1 -Wl,--version-script="$HERE/src/exports.map" \
    "$OUT"/obj/*.o $LIBS
rm -f "$OUT/libpmhwdec.a" && ${AR:-llvm-ar} rcs "$OUT/libpmhwdec.a" "$OUT"/obj/*.o
[ -n "$CEDARD_SRCS" ] && $CC $LDFLAGS -o "$OUT/pmhwdec-cedard" "$OUT"/obj-cedard/*.o -lpthread
$CC $LDFLAGS -O2 -Wall -Wextra -I"$HERE/include" -o "$OUT/pmhwdec-test" "$HERE/tools/pmhwdec_test.c" "$OUT/libpmhwdec.a" $LIBS
$STRIP "$OUT/libpmhwdec.so" "$OUT/pmhwdec-test" "$OUT/pmhwdec-cedard" 2>/dev/null || true
ls -la "$OUT"/libpmhwdec.so "$OUT"/libpmhwdec.a "$OUT"/pmhwdec-test "$OUT"/pmhwdec-cedard 2>/dev/null || true
