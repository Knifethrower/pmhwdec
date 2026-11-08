"""PSNR of the engine's NV12 (stride x coded height) against FFmpeg's cropped NV12 reference.
usage: python3 compare.py hw.nv12 ref.nv12 width height stride coded_height"""
import math
import sys


def psnr(a, b):
    se = sum((x - y) * (x - y) for x, y in zip(a, b))
    return float("inf") if se == 0 else 10 * math.log10(255 * 255 * len(a) / se)


def main():
    hw_path, ref_path = sys.argv[1], sys.argv[2]
    w, h, stride, coded_h = map(int, sys.argv[3:7])
    hw = open(hw_path, "rb").read()
    ref = open(ref_path, "rb").read()
    y_hw = b"".join(hw[r * stride:r * stride + w] for r in range(h))
    uv0 = stride * coded_h
    uv_hw = b"".join(hw[uv0 + r * stride:uv0 + r * stride + w] for r in range(h // 2))
    y_ref, uv_ref = ref[:w * h], ref[w * h:w * h * 3 // 2]
    grey = sum(1 for v in y_hw if v == 0x80) / len(y_hw)
    print(f"PSNR Y {psnr(y_hw, y_ref):.2f} dB, UV {psnr(uv_hw, uv_ref):.2f} dB, untouched-grey luma {grey:.1%}")


if __name__ == "__main__":
    main()
