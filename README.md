# libpmhwdec

Hardware video decoding for PortMaster handhelds: one small library that decodes H.264 and HEVC
on the handheld's video engine and tells you plainly when it can't, so you fall back to software.
It picks the right backend at run time and checks it with a short self-test before trusting it.

| Backend | Where | Driver it talks to |
|---|---|---|
| `cedar` | Allwinner H616 / H618 / H700 / A133 on vendor kernels (Knulli, muOS) | the vendor `/dev/cedar_dev` + ION, through the `pmhw-cedard` helper (no Allwinner userspace code or blobs) |
| `rkmpp` | Rockchip on vendor (BSP) kernels (ArkOS / dArkOS, AmberELEC) | Rockchip MPP, a trimmed build shipped next to the library |
| `v4l2req` | mainline V4L2 stateless decoders (ROCKNIX: hantro, rkvdec, Cedrus) | `/dev/videoN` + media request API, frame- and slice-based |
| `v4l2m2m` | mainline V4L2 stateful decoders (e.g. Qualcomm on ROCKNIX) | `/dev/videoN` |

Output is NV12 in display order (10-bit HEVC as its top 8 bits), with a dma-buf fd for zero-copy where
the backend has one.

## Verified on

Bit-exact against FFmpeg's software decoder on test clips (Baseline/Main/High H.264 with B-frames,
weighted prediction and multiple slices; HEVC Main incl. WPP and multiple slices; Big Buck Bunny 1080p):

| Handheld | SoC, firmware | Backend | H.264 | HEVC | HEVC 10-bit |
|---|---|---|---|---|---|
| RG Cube XX | H700, Knulli | cedar | yes | yes | yes (8-bit output) |
| TrimUI Smart Pro | A133P, Knulli | cedar | yes | yes | no (the chip has none) |
| TrimUI Brick | A133P, muOS | cedar | yes | yes | no |
| R36S | RK3326, dArkOS | rkmpp | yes | yes | no |
| RG353V | RK3566, dArkOS | rkmpp | yes | yes | yes |
| RG552 | RK3399, AmberELEC | rkmpp | yes | yes | yes |
| RG351P | RK3326, ROCKNIX | v4l2req (hantro) | yes | no (no driver) | |
| RG34XX SP | H700, ROCKNIX with Cedrus test modules | v4l2req (Cedrus, slice-based) | yes | yes | yes |
| Retroid Pocket 5 | SM8250, ROCKNIX | v4l2m2m | yes (rare off-by-one samples from the firmware) | yes | no |

32-bit (armhf) programs use the armhf library; on Allwinner it starts the aarch64 `pmhw-cedard`, since
these handhelds run 64-bit kernels (checked on the RG Cube XX and, with armhf MPP, the R36S).

Chips without a test device are covered by the start-up self-test: a few tiny embedded clips are decoded
and compared against FFmpeg's output (8x8 block means) before a backend is used, at most 5 s, cached in
`/tmp` per boot. A backend that fails is not used for that codec; a failed 10-bit test caps it at 8 bit.

## API

`include/pmhwdec.h` (MIT):

```c
PmhwDecoder *d = pmhw_open(PMHW_CODEC_HEVC, width, height);   // NULL: no hardware, use software
while ((r = pmhw_send(d, data, size, pts)) == PMHW_AGAIN)      // Annex-B access units in
    drain();                                                   // all buffers out: hand some back first
if (r == PMHW_UNSUPPORTED) { /* close it, decode this stream in software */ }
drain();      // while (pmhw_get(d, &pic) == 1) { use pic (NV12); pmhw_put(d, &pic); }
pmhw_send(d, NULL, 0, 0); drain();                             // end of stream
```

`pmhw_query()` reports what a codec can do here without opening a decoder. `PMHWDEC_LOG=1` logs to stderr,
`PMHWDEC_BACKEND=<name>` forces a backend, `PMHWDEC_BACKEND=off` disables them all.

## Using it

- **FFmpeg / mpv**: `glue/ffmpeg/pmhwdec_ff.c` adds `h264_pmhw` / `hevc_pmhw` decoders that load the library
  at run time and fall back to FFmpeg's own decoders inside.
- **portmaster-unity**: its media runtime loads `libpmhwdec.so.1` (`UM_PMHW=<path>`) ahead of its software
  decoders, so Unity games' videos play on the hardware where there is one.
- **Directly**: link or `dlopen` `libpmhwdec.so.1`; ship `pmhw-cedard` (Allwinner) and `librockchip_mpp.so`
  (Rockchip) next to it.
- On ArkOS / dArkOS the Rockchip decoder nodes are root-only while ports run as `ark`: a launcher should
  `$ESUDO chgrp video` + `chmod 660` `/dev/vpu_service`, `/dev/hevc_service`, `/dev/rkvdec`, `/dev/mpp_service`.

## Building

Cross-compiled from Linux (WSL here) with clang + lld against a Debian sysroot:

```bash
bash build.sh                          # aarch64: out/aarch64/ (SYSROOT=bullseye arm64, glibc 2.31)
ARCH=armhf bash build.sh               # armhf:   out/armhf/   (SYSROOT=buster armhf, glibc 2.28)
bash rkmpp/build_mpp.sh                # Rockchip MPP 1.1.0, trimmed (~540 KB): out/<arch>/mpp/
ARCH=armhf bash rkmpp/build_mpp.sh
```

`build.sh` makes `libpmhwdec.so` (soname `libpmhwdec.so.1`), `libpmhwdec.a`, `pmhw-cedard` (aarch64 only)
and `pmhw-test`. `tools/make_kit.sh` packs a tester kit (`kit/`): a PortMaster "port" that runs the clips and
the self-test on a handheld and writes `ports/pmhwdec-report.txt`.

`probe/` holds the first experiments that drove the Allwinner engine from userspace; `DESIGN.md` is the
working log (chips, drivers, measurements, decisions).

## Licenses

- **libpmhwdec** (everything not listed below): LGPL-2.1-or-later (`LICENSE`). Closed programs may load it.
- **pmhw-cedard**, the Allwinner decoder, is a separate GPL-2.0-or-later program (`LICENSE.pmhw-cedard`):
  its register programming is ported from mainline Linux's Cedrus driver (Jens Kuske, Paul Kocialkowski /
  Bootlin). The library starts it as a helper process and talks to it over a socket (`src/cedar_ipc.h`);
  it never links it. `src/cedar.c`, `src/cedard.c`, `src/ve_regs.h`, `tools/gen_ve_regs.py` and the C
  files in `probe/` are GPL-2.0-or-later.
- **`include/pmhwdec.h`**: MIT.
- **Rockchip MPP** (built by `rkmpp/build_mpp.sh`): Apache-2.0 / MIT, its own license file.
- `src/uapi/linux/`: Linux UAPI headers (GPL-2.0 WITH Linux-syscall-note).
