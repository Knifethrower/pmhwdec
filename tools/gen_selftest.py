#!/usr/bin/env python3
"""Builds src/selftest_data.h: three tiny clips (H.264 High with B-frames, HEVC Main, HEVC Main 10; 160x96, 6
frames) split into access units, and FFmpeg's decode of each as 8x8 block means of Y, U and V per frame in display
order (10-bit: the top 8 bits, as libpmhwdec outputs them). The library decodes the clips at startup and refuses
a decoder whose output strays (see selftest.c).
Needs ffmpeg with libx264 + libx265 (WSL). usage: python3 tools/gen_selftest.py   (from the repo root)"""
import os
import subprocess
import sys
import tempfile
import zlib

W, H, N = 160, 96, 6
SRC = f"testsrc2=size={W}x{H}:rate=30"

CLIPS = [
    ("h264", "H264", 8, ["-c:v", "libx264", "-profile:v", "high", "-preset", "medium", "-pix_fmt", "yuv420p",
                         "-x264-params", "bframes=2:ref=3:b-pyramid=normal:weightp=2:keyint=60",
                         "-bsf:v", "filter_units=remove_types=6", "-f", "h264"]),
    ("hevc", "HEVC", 8, ["-c:v", "libx265", "-pix_fmt", "yuv420p",
                         "-x265-params", "bframes=2:ref=3:keyint=60:info=0:log-level=error", "-f", "hevc"]),
    ("hevc10", "HEVC", 10, ["-c:v", "libx265", "-pix_fmt", "yuv420p10le", "-profile:v", "main10",
                            "-x265-params", "bframes=2:ref=3:keyint=60:info=0:log-level=error", "-f", "hevc"]),
    ("vp8", "VP8", 8, ["-c:v", "libvpx", "-pix_fmt", "yuv420p", "-deadline", "good", "-cpu-used", "1",
                       "-auto-alt-ref", "0", "-g", "60", "-b:v", "300k", "-f", "ivf"]),
]


def ivf_frames(raw):
    """the frames of an IVF file, joined, and (start, end) of each"""
    pos = int.from_bytes(raw[6:8], "little")
    data, aus = bytearray(), []
    while pos + 12 <= len(raw):
        n = int.from_bytes(raw[pos:pos + 4], "little")
        aus.append((len(data), len(data) + n))
        data += raw[pos + 12:pos + 12 + n]
        pos += 12 + n
    return bytes(data), aus


def run(args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def nals(data):
    """(start offset including the start code, payload offset) of each NAL in an Annex-B stream"""
    out, i = [], 0
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        s = j - 1 if j > 0 and data[j - 1] == 0 else j
        out.append((s, j + 3))
        i = j + 3
    return out


def access_units(data, codec):
    """splits at the first slice of each picture; parameter sets / SEI before it go with it"""
    ns = nals(data)
    aus, start, have_vcl = [], 0, False
    for k, (s, p) in enumerate(ns):
        if codec == "H264":
            t = data[p] & 0x1F
            vcl = 1 <= t <= 5
            first = vcl and (data[p + 1] & 0x80) != 0          # first_mb_in_slice == 0
        else:
            t = (data[p] >> 1) & 0x3F
            vcl = t <= 31
            first = vcl and (data[p + 2] & 0x80) != 0          # first_slice_segment_in_pic_flag
        if not vcl and have_vcl:                               # non-VCL after a picture starts the next AU
            aus.append((start, s))
            start, have_vcl = s, False
        elif first and have_vcl:
            aus.append((start, s))
            start = s
        if vcl:
            have_vcl = True
    aus.append((start, len(data)))
    return aus


def block_means(frame8):
    """frame8: NV12-ordered 8-bit planes as (Y bytes, U bytes, V bytes) -> means of 8x8 blocks, rounded"""
    y, u, v = frame8
    out = bytearray()
    for plane, pw, ph in ((y, W, H), (u, W // 2, H // 2), (v, W // 2, H // 2)):
        for by in range(0, ph, 8):
            for bx in range(0, pw, 8):
                s = n = 0
                for yy in range(by, min(by + 8, ph)):
                    row = plane[yy * pw + bx: yy * pw + min(bx + 8, pw)]
                    s += sum(row)
                    n += len(row)
                out.append((s + n // 2) // n)
    return bytes(out)


def decode(path, depth):
    """FFmpeg's frames in display order as 8-bit (Y, U, V); 10-bit keeps the top 8 bits (truncated)"""
    fmt = "yuv420p" if depth == 8 else "yuv420p10le"
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", fmt, "-"],
                         check=True, stdout=subprocess.PIPE).stdout
    bps = 1 if depth == 8 else 2
    ys, cs = W * H * bps, (W // 2) * (H // 2) * bps
    fsize = ys + 2 * cs
    assert len(raw) == fsize * N, (len(raw), fsize * N)
    frames = []
    for f in range(N):
        b = raw[f * fsize:(f + 1) * fsize]
        planes = [b[:ys], b[ys:ys + cs], b[ys + cs:]]
        if depth == 10:
            planes = [bytes((p[i] | p[i + 1] << 8) >> 2 for i in range(0, len(p), 2)) for p in planes]
        frames.append(planes)
    return frames


def nv12_adler(frame8):
    """adler32 of the visible NV12 picture (Y rows, then interleaved UV rows), as pmhwdec-test prints it"""
    y, u, v = frame8
    uv = bytes(x for pair in zip(u, v) for x in pair)
    return zlib.adler32(uv, zlib.adler32(y, 1)) & 0xFFFFFFFF


def carray(name, data):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 20):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 20]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = [
        "/* SPDX-License-Identifier: LGPL-2.1-or-later */",
        "/* Generated by tools/gen_selftest.py: do not edit. Self-test clips (libx264 / libx265 encodes of FFmpeg's",
        f" * testsrc2 pattern, {W}x{H}, {N} frames) and FFmpeg's decode of them as 8x8 block means (Y, U, V). */",
        "#include <stdint.h>",
        f"#define ST_W {W}",
        f"#define ST_H {H}",
        f"#define ST_FRAMES {N}",
        f"#define ST_MEANS {(W // 8) * (H // 8) + 2 * (W // 16) * (H // 16)}",
    ]
    table = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, codec, depth, enc in CLIPS:
            path = os.path.join(tmp, name + ".bin")
            run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", SRC, "-frames:v", str(N)] + enc + [path])
            data = open(path, "rb").read()
            if codec == "VP8":
                data, aus = ivf_frames(data)
            else:
                aus = access_units(data, codec)
            frames = decode(path, depth)
            means = b"".join(block_means(f) for f in frames)
            adlers = [nv12_adler(f) for f in frames]
            out.append(carray(f"st_{name}_data", data))
            out.append(f"static const uint32_t st_{name}_au[{len(aus) + 1}] = {{ "
                       + ", ".join(str(s) for s, _ in aus) + f", {len(data)} }};")
            out.append(carray(f"st_{name}_means", means))
            out.append(f"static const uint32_t st_{name}_adler[{N}] = {{ "
                       + ", ".join(f"0x{a:08x}" for a in adlers) + " };")
            table.append(f"    {{ \"{name}\", PMHW_CODEC_{codec}, {depth}, st_{name}_data, st_{name}_au, "
                         f"{len(aus)}, st_{name}_means, st_{name}_adler }},")
            print(f"{name}: {len(data)} bytes, {len(aus)} access units", file=sys.stderr)
    out.append("static const struct StClip { const char *name; int codec, depth; const uint8_t *data; "
               "const uint32_t *au; int naus; const uint8_t *means; const uint32_t *adler; } st_clips[] = {")
    out += table
    out.append("};")
    open(os.path.join(root, "src", "selftest_data.h"), "w", newline="\n").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
