#!/bin/bash
# WSL: cedar.c compiled against the reference header (mainline cedrus_regs.h, given as $1) and against our
# src/ve_regs.h must give identical code and data: then ve_regs.h carries exactly the same hardware facts.
# usage: bash tools/check_ve_regs.sh [reference cedrus_regs.h]   (default: downloads mainline's into a temp dir)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
if [ -n "${1:-}" ]; then REF=$(readlink -f "$1"); else
    REF=$T/cedrus_regs.h
    curl -sSfL https://raw.githubusercontent.com/torvalds/linux/master/drivers/staging/media/sunxi/cedrus/cedrus_regs.h -o "$REF"
    sed -i '1a #define BIT(n) (1u << (n))\n#define GENMASK(h, l) (((~0UL) << (l)) \& (~0UL >> (63 - (h))))' "$REF"
fi
SYSROOT=${SYSROOT:-$HOME/chroot-bullseye}
CC="clang --target=aarch64-linux-gnu --sysroot=$SYSROOT -mcpu=cortex-a53 -O2 -fPIC -I$HERE/src/uapi -I$HERE/include -I$HERE/src"
sed "s|#include \"ve_regs.h\"|#include \"$REF\"|" "$HERE/src/cedar.c" > "$T/cedar_ref.c"
cp "$HERE/src/cedar.c" "$T/cedar_new.c"
grep -q "#include \"$REF\"" "$T/cedar_ref.c"
$CC -c "$T/cedar_ref.c" -o "$T/ref.o"
$CC -c "$T/cedar_new.c" -o "$T/new.o"
for s in .text .rodata .data; do
    llvm-objcopy -O binary --only-section=$s "$T/ref.o" "$T/ref$s" 2>/dev/null || true
    llvm-objcopy -O binary --only-section=$s "$T/new.o" "$T/new$s" 2>/dev/null || true
    if cmp -s "$T/ref$s" "$T/new$s"; then echo "$s identical ($(stat -c %s "$T/new$s") bytes)"; else echo "$s DIFFERS"; exit 1; fi
done
rm -rf "$T"
