"""Per-frame pixel differences of two tight NV12 dumps (same size, display order): differing samples, max |diff|, PSNR.
usage: python pixdiff.py a.nv12 b.nv12 width height [frames]"""
import math
import sys

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
w, h = int(sys.argv[3]), int(sys.argv[4])
fs = w * h * 3 // 2
n = min(len(a), len(b)) // fs
if len(sys.argv) > 5:
    n = min(n, int(sys.argv[5]))
worst = None
for i in range(n):
    fa, fb = a[i * fs:(i + 1) * fs], b[i * fs:(i + 1) * fs]
    if fa == fb:
        continue
    diffs = [abs(x - y) for x, y in zip(fa, fb) if x != y]
    se = sum(d * d for d in diffs)
    psnr = 10 * math.log10(255 * 255 * fs / se)
    print(f"frame {i}: {len(diffs)} of {fs} samples differ, max {max(diffs)}, PSNR {psnr:.1f} dB")
    if worst is None or psnr < worst:
        worst = psnr
print(f"{n} frames compared; " + ("all identical" if worst is None else f"lowest PSNR {worst:.1f} dB"))
