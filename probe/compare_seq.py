"""Per-frame PSNR of h264_seq output (decode order, log has FRAME lines) against FFmpeg's display-order NV12.
usage: python compare_seq.py hw.nv12 hw.log ref.nv12 width height"""
import math
import sys


def psnr(a, b):
    se = sum((x - y) * (x - y) for x, y in zip(a, b))
    return float("inf") if se == 0 else 10 * math.log10(255 * 255 * len(a) / se)


def main():
    hw_path, log_path, ref_path = sys.argv[1:4]
    w, h = int(sys.argv[4]), int(sys.argv[5])
    stride, coded_h = (w + 15) // 16 * 16, (h + 15) // 16 * 16
    hw_frame, ref_frame = stride * coded_h * 3 // 2, w * h * 3 // 2
    frames = []
    for line in open(log_path):
        if line.startswith("FRAME "):
            f = line.split()
            frames.append((int(f[2]), int(f[3]), int(f[1]), f[4], "ENGINE" in line))
    hw = open(hw_path, "rb").read()
    ref = open(ref_path, "rb").read()
    n_ref = len(ref) // ref_frame
    print(f"{len(frames)} decoded, {n_ref} reference frames")
    exact, worst = 0, None
    for disp, (epoch, poc, dec, typ, err) in enumerate(sorted(frames)):
        if disp >= n_ref:
            break
        base = dec * hw_frame
        y = b"".join(hw[base + r * stride:base + r * stride + w] for r in range(h))
        uv0 = base + stride * coded_h
        uv = b"".join(hw[uv0 + r * stride:uv0 + r * stride + w] for r in range(h // 2))
        r0 = disp * ref_frame
        ry, ruv = ref[r0:r0 + w * h], ref[r0 + w * h:r0 + ref_frame]
        py = float("inf") if y == ry else psnr(y, ry)
        puv = float("inf") if uv == ruv else psnr(uv, ruv)
        same = py == float("inf") and puv == float("inf")
        exact += same
        if not same and (worst is None or py < worst[1]):
            worst = (disp, py, puv, typ)
        if not same or disp < 3:
            print(f"display {disp:3d} (decode {dec:3d}, {typ}, epoch {epoch}, poc {poc:4d}): "
                  f"Y {py:.2f} UV {puv:.2f}{' ENGINE ERROR' if err else ''}")
    print(f"bit-exact {exact} of {min(len(frames), n_ref)}" + (f"; worst display {worst[0]} ({worst[3]}) Y {worst[1]:.2f} dB" if worst else ""))


if __name__ == "__main__":
    main()
