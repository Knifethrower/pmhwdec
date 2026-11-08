"""Compares the engine's per-frame adler32 (FRAME lines of h264_seq / hevc_seq) with FFmpeg's, in display order.
usage: python compare_crc.py hw.log ref.crc
       python compare_crc.py hw.log ref.p010 width height    (10-bit: references made by truncating and by rounding)"""
import sys
import zlib


def ours(log_path):
    frames = []
    for line in open(log_path):
        if line.startswith("FRAME "):
            f = line.split()
            ad = [x for x in f if x.startswith("adler=")][0][6:]
            frames.append((int(f[2]), int(f[3]), int(f[1]), f[4], int(ad, 16), "ENGINE" in line))
    return sorted(frames)


def ref_crc(path):
    out = []
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        out.append(int(line.split(",")[-1].strip(), 16))
    return out


def ref_p010(path, w, h, n):
    data = open(path, "rb").read()
    fsize = w * h * 3 // 2 * 2
    trunc, rnd = [], []
    for i in range(min(n, len(data) // fsize)):
        fr = memoryview(data)[i * fsize:(i + 1) * fsize]
        hi = bytes(fr[1::2])                                        # p010: value << 6, high byte = top 8 bits
        lo = bytes(fr[0::2])
        r = bytes(min(255, b + (l >> 7)) for b, l in zip(hi, lo))  # round at bit 1 of the 10-bit value
        trunc.append(zlib.adler32(hi, 0))
        rnd.append(zlib.adler32(r, 0))
    return trunc, rnd


def main():
    hw = ours(sys.argv[1])
    if sys.argv[2].endswith(".p010"):
        w, h = int(sys.argv[3]), int(sys.argv[4])
        trunc, rnd = ref_p010(sys.argv[2], w, h, len(hw))
        n = min(len(trunc), len(hw))
        mt = sum(hw[i][4] == trunc[i] for i in range(n))
        mr = sum(hw[i][4] == rnd[i] for i in range(n))
        print(f"{len(hw)} decoded; first {n} in display order: {mt} match truncated 10->8, {mr} match rounded")
        return
    ref = ref_crc(sys.argv[2])
    n = min(len(ref), len(hw))
    bad = [i for i in range(n) if hw[i][4] != ref[i]]
    errs = sum(1 for f in hw if f[5])
    print(f"{len(hw)} decoded, {len(ref)} reference frames, engine errors {errs}: bit-exact {n - len(bad)} of {n}")
    for i in bad[:8]:
        e, poc, dec, typ, ad, err = hw[i]
        print(f"  display {i} (decode {dec}, {typ}, epoch {e}, poc {poc}) ours {ad:08x} ref {ref[i]:08x}")


if __name__ == "__main__":
    main()
