# pmhwdec: hardware video decoding for PortMaster ports

Started 2026-10-30. Goal: one small library a port can use (directly, or through FFmpeg/mpv) to decode
H.264 / HEVC (and VP9 where the hardware has it) on the video engines of PortMaster handhelds, with a clean
software fallback. Grew out of the CedarX work for the Jellyfin MPV Shim port.

This file is the project's working log: what was tried, measured and decided, in order. Some device test rigs
it mentions (a ROCKNIX Cedrus test build, DeviceHub scripts) are not part of the published repository.

## Which chips (from PortMaster's own device data)
Source: `PortsMaster/PortMaster-GUI` `tools/test_suite/data/device_info.json` + `PortMaster/device_info.txt`
(fetched 2026-10-30).

| SoC | PortMaster devices |
|--|--|
| RK3326 / PX30 | RG351P/M, RG351V, RG351MP, R33S, R35S, R36S, XU10, ODROID Go Advance, ODROID Go Super, Gameforce Chi, RGB20S (+ clones) |
| RK3566 | RG353 M/V/P/VS/PS, RG503, RG ARC-D/S, RGB30, RGB10MAX3, RK2023, X55, GKD Bubble |
| RK3399 | RG552 |
| RK3588(S) | Gameforce Ace |
| RK3328 | only in device_info.txt's chip match (no named device in the test data) |
| H700 (Allwinner) | RG28XX, RG34XX(SP), RG35XX H/Plus/SP, RG40XX H/V, RG Cube XX |
| A133P (Allwinner) | TrimUI Smart Pro, TrimUI Brick |
| others, later | S922X (ODROID Go Ultra, RGB10MAX3 Pro: Amlogic, mainline meson-vdec), SD865 (RP5, RP Mini: Qualcomm venus, V4L2 stateful) |

## Decoder hardware and drivers per chip
"Vendor" = Rockchip BSP kernel (ArkOS/dArkOS, AmberELEC, older JELOS); "mainline" = ROCKNIX and other
mainline-based builds (Knulli's RK builds are Batocera = mainline: to check).

| SoC | Decoder blocks | Vendor kernel | Mainline |
|--|--|--|--|
| RK3326 | VDPU2 (H.264 1080p, VP8, MPEG-2) + RK HEVC 1080p | 4.4: `/dev/vpu_service` + `/dev/hevc_service` (vpu_combo) -> MPP | hantro (H.264, VP8, MPEG-2), **no HEVC driver** |
| RK3566 | RKVDEC2 VDPU346 (H.264/HEVC 10-bit/VP9, 4K) + VDPU121 hantro G1 (H.264 1080p) | 4.19: `/dev/mpp_service` -> MPP | hantro G1 H.264 only; VDPU346 rkvdec patches (C. Hewitt, v3 Jan 2026) **not seen merged** |
| RK3399 | rkvdec (H.264/HEVC 10-bit/VP9 4K) + VDPU2 | 4.4: `/dev/rkvdec` + `/dev/vpu_service` -> MPP (seen on RG552 AmberELEC 2026-10-30, nodes root-only 0600) | rkvdec H.264 + VP9; HEVC = LibreELEC out-of-tree patches |
| RK3328 | as RK3399 | MPP | as RK3399 |
| RK3588 | VDPU381 (H.264/HEVC/VP9 8K), AV1 decoder, hantro G1 | 5.10/6.1: MPP | VDPU381 H.264/HEVC merged in Linux 7.0; AV1 since 6.5 |
| H700 | Allwinner VE | Knulli/muOS 4.9: `/dev/cedar_dev` -> libcedarc (proven: H.264 87, HEVC10 108 fps) | ROCKNIX: cedrus (H.264, HEVC 8-bit) |
| A133P | Allwinner VE (H.264/HEVC 4K30, VP9 720p30) | Knulli/muOS on TrimUI's 4.9 kernel: `/dev/cedar_dev` expected -> libcedarc, **but no proven Linux libVE** (see below) | **no Cedrus support** (no A100/A133 compatible in mainline cedrus.c) |

What ROCKNIX actually ships (it carries out-of-tree media patches) has to be read off the devices.

## A133P (TrimUI Smart Pro, TrimUI Brick): researched 2026-10-30, no device run yet
- Chip: sun50iw10 (A133 / A133 Plus / R818 / A100 family), 4x A53, PowerVR GE8300. Datasheet/product brief:
  H.265 and H.264 decode up to 4K30, VP9 up to 720p30 only. The "P" does not change the video engine as far as
  any source says.
- Mainline: cedrus has no A100/A133 entry (compatible list in drivers/staging/media/sunxi/cedrus/cedrus.c,
  checked 2026-10-30: a10 ... h6, h616). So vendor CedarX is the only route; also for a future mainline CFW.
- The catch is `libVE.so`: the closed library that identifies the engine (`VeGetChipId`, reads the VE version
  registers; unknown chips abort with "You should know ic version!"). CalvinXu17/libcedarc README: Allwinner
  published an A133-capable libVE only for **Android**; the glibc toolchain libVE "does not support the A133 VE".
  Calvin converted the Android libVE to glibc (bionic -> glibc, .rela.android relocations decoded, liblog wrapper)
  and uses the glibc toolchain set for everything else. His tests: JPEG/H.264 **encode** and MJPEG decode OK;
  **H.264/HEVC decode not tested**.
- His A133 build flags (from cedarc_config.go a100 Q): `CONF_KERNEL_VERSION_4_9 CONF_IMG_GPU_USE_COMMON_STRUCT
  CONF_USE_IOMMU CONF_KERN_BITWIDE=64 CONFIG_VE_IPC_ENABLE GPU_ALIGN_STRIDE=32 CONF_VE_FREQ_ENABLE_SETUP
  CONF_CERES_VE_FREQ_ENABLE_SETUP CONF_PIE_AND_NEWER`. The H700 bridge uses only the first and CONF_USE_IOMMU.
- The V853 set that works on the H700 (2022, newer than Calvin's toolchain set) is not proven either way for the
  A133: libVE picks the chip at run time, so only a device run tells.
- Earlier H700 note: the old Calvin libvideoengine passes HEVC to hardware only for decIpVersion 0x31010/0x30111.
  If one of those is the A133's engine, the Calvin set may do HEVC there (it did not on the H700, 0x33010).
- Display: a muOS forum thread reports mpv's gpu output broken on the Brick (PowerVR), so the copy path to the
  port's own renderer matters more here than zero-copy.
- Licensing is worse than on the H700: Calvin's libVE is a modified Android Allwinner binary, no licence. Same
  rule as the H700: never ship it, fetch on device (or personal builds only).

### Blob-free engine ID probe (probe/ve_probe.c, 2026-10-30)
No Allwinner userspace: GPL vendor driver interface only (ENGINE_REQ + ENABLE_VE, mmap of 0x01C0E000, read
0x00/0xe0/0xe4/0xf0, release). `probe/build.sh` (WSL, static aarch64), `probe/ve_probe.sh` (device: facts + probe,
refuses while anything has /dev/cedar_dev open).

| Device | Kernel | 0xf0 (Cedrus VE_VERSION) | 0xe0 | 0xe4 | VE_MODE |
|--|--|--|--|--|--|
| TrimUI Smart Pro (A133P, Knulli) | 4.9.191, DT `ve@01c0e000` `allwinner,sunxi-cedar-ve` | 0x00000000 | 0x00030210 | 0x00012010 | 0xc0000007 |
| RG Cube XX (H700, Knulli) | 4.9.170, DT `ve@01c0e000` `allwinner,sunxi-cedar-ve` | 0x00000000 | 0x00033010 | 0x00012011 | 0xc0000007 |

- The blobs' `nIcVeVersion` is (0xe0 << 32) | 0xe4: H700 0x3301000012011 = 0x33010 / 0x12011. decIpVersion = 0xe0.
- A133P decIpVersion **0x30210**; H616/H700 **0x33010**: different IP revision of the same register layout family
  (0xf0 empty, IDs at 0xe0/0xe4 on both). Not the old blob's HEVC values (0x31010/0x30111) either.
- Cedrus never reads the version (only an H6/H616 compatible + capability flags), so whether the A133P's 0x30210
  engine takes Cedrus' H6 register programming can only be found by decoding a frame (next probe: H.264 one
  I-frame with Cedrus' register sequence, compare against FFmpeg).

### Blob-free H.264 frame test (probe/h264_frame.c, 2026-10-30): **works on the A133P, bit-exact**
Cedrus' H.264 register sequence (cedrus_h264.c set_params/setup, cedrus_hw.c untiled NV12 output) from userspace
over /dev/cedar_dev; ION system heap buffers mapped for the VE with IOCTL_GET_IOMMU_ADDR; completion through the
vendor driver's IRQ (IOCTL_WAIT_VE_DE, H.264 status 0x228). Own minimal SPS/PPS/slice header parser (header bit
counts checked against FFmpeg trace_headers). Clips from `probe/make_clips.sh` (testsrc2, x264, one IDR frame).

| TrimUI Smart Pro (A133P, Knulli 4.9.191) | Result | VE time | vs FFmpeg |
|--|--|--|--|
| 640x360 baseline CAVLC | slice done, 920/920 MBs | 0.79 ms | **identical** (Y and UV) |
| 640x360 main CABAC | slice done, 920/920 MBs | 0.82 ms | **identical** |
| 1280x720 high CABAC, 8x8 transform | slice done, 3600/3600 MBs | 3.14 ms | **identical** |

- So the A133P's 0x30210 engine takes the H6/H616 Cedrus programming unchanged, at least for H.264 intra.
- TrimUI's ION has no ION_IOC_HEAP_QUERY (EINVAL): the test falls back to trying heap ids; id 0 (system) works
  and maps for the VE. VE addresses come out around 0xfb5xxxxx-0xfb7xxxxx (IOMMU space).
- ERROR_CASE reads 0x40 after good decodes: not an error indicator by itself.
- P/B frames: see the sequence test below. Not covered yet: multiple slices, real camera content, HEVC, long runs.

### Blob-free H.264 sequences (probe/h264_seq.c, 2026-10-30): **bit-exact on the H700**
Adds references to h264_frame: POC types 0/2, sliding window + MMCO 1, list init + short-term modification, explicit
weighted prediction tables, implicit/temporal/spatial direct (VE does those from the frame list + mv_col buffers),
pool of max_num_ref_frames + 2 buffers (NV12 + two mv_col fields each). Output in decode order + FRAME log;
`compare_seq.py` puts it in display order. `PARSE_ONLY=1` runs the parser under qemu; `check_seq.sh` compares every
slice header size with FFmpeg trace_headers (all 150 match; FFmpeg's trace includes cabac_alignment_one_bits, the
VE count does not). Clips: `make_seq_clips.sh`.

| Clip | RG Cube XX (H700) | engine avg / max |
|--|--|--|
| seq_p_cavlc: 640x360, 60 frames, P only, CAVLC, 3 refs, weightp list duplicates, POC type 2 | **60/60 identical** | 0.90 / 1.17 ms |
| seq_b_pyramid: 640x360, 60 frames, B pyramid, 4 refs, weightp, CABAC, 8x8, MMCO 1, 2 IDRs | **60/60 identical** | 1.26 / 1.54 ms |
| seq_720p_temporal: 1280x720 mandelbrot, 30 frames, temporal direct, explicit weighted B | **30/30 identical** | 4.95 / 6.29 ms |

TrimUI Smart Pro (A133P): **150/150 identical** too (60, 60, 30), engine avg/max 0.88 / 1.21, 1.21 / 1.78,
4.66 / 6.83 ms. (Hub SSH dropped once between run and pull; frames pulled after it came back.)
- RG Cube XX (H700, Knulli 4.9.170), same binary and clips, 2026-10-30: all three **identical** to FFmpeg, all MBs,
  0.76 / 0.81 / 3.04 ms. VE addresses ~0xff5xxxxx-0xff7xxxxx. So one userspace code path covers both chips.

Device probe plan (Smart Pro on Knulli, Brick on muOS; both offline 2026-10-30), with pctest/cedar/cedar_probe.c:
1. look: `/dev/cedar_dev`, `/dev/ion` or dma_heap, `iommu`, dmesg cedar lines, VE version from the probe log.
2. V853 blobs + open parts as on the H700 (known good build).
3. Calvin's converted libVE + V853 for the rest; then Calvin's full set; each with the a100 flags above.
4. per set: H.264 / HEVC 8-bit / HEVC 10-bit 1080p clips, fps + PSNR vs FFmpeg, as in FINDINGS.md.

### Blob-free HEVC + H.264 1080p (probe/hevc_seq.c, h264_seq.c, 2026-10-30): **bit-exact on the H700**
hevc_seq: cedrus_h265.c register sequence (regs copied into probe/cedrus_regs_h265.h, GPL-2.0+), own parser: VPS/SPS
(profile_tier_level, short-term RPS incl. inter-RPS prediction) / PPS / slice segment header, POC (prevTid0Pic),
RPS marking, list construction + modification, weighted prediction tables (derived chroma offsets), WPP entry points
(offsets converted to unescaped bytes: the VE strips emulation prevention bytes), RASL skipped only after a CRA that
starts the stream. Both decoders now print FFmpeg framecrc's adler32 per frame (no 900 MB dumps on a 1 GB device),
DUMP_FRAMES / MAX_FRAMES caps, wall time per frame. Checks: `check_hevc.sh` (header bits vs trace_headers, all match
except the last slice of the cut BBB files, where FFmpeg's trace ends at EOF), `compare_crc.py`.
Clips: `make_hevc_clips.sh` (x265 testsrc2/mandelbrot + first 300 frames of the BBB clips from the Jellyfin work).

RG Cube XX (H700), engine time per frame (CPU sleeps in the IRQ wait meanwhile):

| Clip | Frames | vs FFmpeg | engine avg / max | incl. parse + copy |
|--|--|--|--|--|
| HEVC 640x360 intra, no WPP | 10 | **10/10 identical** | 0.44 / 0.48 ms | 0.48 ms |
| HEVC 640x360 P/B, weighted P, 3 refs | 60 | **60/60** | 0.55 / 0.67 ms | 0.56 ms |
| same with WPP (entry points) | 60 | **60/60** | 0.55 / 0.65 ms | 0.57 ms |
| HEVC 720p Main 10, B-pyramid, 4 refs | 30 | **30/30 = FFmpeg's 10-bit decode truncated to 8 bit** | 3.58 / 4.43 ms | 3.62 ms |
| **BBB HEVC 1080p Main (WPP, CRA + RASL)** | 300 | **300/300** | **4.56** / 11.51 ms | 4.59 ms |
| **BBB HEVC 1080p Main 10** | 300 | first 30 checked: 30/30 truncated match | **6.25** / 10.83 ms | 6.34 ms |
| **BBB H.264 1080p High (B-frames, weighted P)** | 300 | **300/300** | **10.18** / 12.33 ms | 10.23 ms |

- vs software on the same device (Construct Native session): OpenH264 1080p High decode ~89 ms/frame -> engine
  10.2 ms (**~8.7x**, and the CPU is free meanwhile). VP9 1080p libvpx 9.7-10.2 ms with 2 threads busy.
- vs the Allwinner blobs (Jellyfin FINDINGS, same clips): H.264 87.5 fps (11.4 ms), HEVC8 135 fps (7.4 ms), HEVC10
  108 fps (9.3 ms) -> ours 10.2 / 4.6 / 6.3 ms. Same or faster, without any Allwinner code.
- 10-bit: the primary output is the top 8 bits (truncation, no rounding/dither), as Cedrus documents; the 2-bit plane
  is written next to it (not used yet). A 10-bit output path (P010 for HDR-capable consumers) is possible later.
- TrimUI Smart Pro (A133P), same binaries, 2026-10-30:

  | Clip | Result vs FFmpeg | engine avg / max |
  |--|--|--|
  | HEVC 640x360 intra / P/B / P/B + WPP | **10/10, 60/60, 60/60 identical** | 0.71 / 0.75 / 0.74 ms |
  | BBB HEVC 1080p Main, 300 frames | **300/300 identical** | **6.25** / 11.60 ms |
  | BBB H.264 1080p High, 300 frames | **300/300 identical** | **9.63** / 12.96 ms |
  | HEVC 720p **Main 10** | engine ERROR on 30/30 frames (status success + error bits, stops after 60-100 of 240 CTBs) | |
  | BBB HEVC 1080p Main 10 | engine ERROR on 105/300; the other frames finish but are wrong too (0 of the first 30 match) | |

  Main 10 failing fits the A133's spec (H.265 decode listed without 10-bit) and the old blobs' HEVC plugin refusing
  10-bit on older chips: the A133P engine most likely has no 10-bit HEVC. pmhwdec_query must report max_bit_depth 8
  there (decide by the 0xe0 IP version: 0x30210 = 8-bit only, 0x33010 = 10-bit), and players should ask for a
  transcode or use software for Main 10.

## libpmhwdec v0 (2026-10-30): the library
Layout: `include/pmhwdec.h` (public API), `src/pmhwdec.c` (backend pick), `src/stateless.c` (frame store, output
reorder queue, AGAIN protocol, configure/caps checks), `src/h264.c` + `src/hevc.c` (codec layers: parsing, POC,
reference marking, lists; jobs shaped like the V4L2 stateless controls so a v4l2req backend can reuse them),
`src/cedar.c` (Allwinner backend), `src/cedrus_regs.h` (mainline, verbatim), `tools/pmhwdec_test.c` (public API only),
`build.sh` (aarch64 .so/.a + pmhwdec-test; only the 8 pmhwdec_* symbols exported).
- Two backend kinds: stream backends (rkmpp: data in, pictures out) and the stateless core driving HwOps (cedar now,
  v4l2req later for ROCKNIX: cedrus/hantro/rkvdec).
- New vs the probes: several SPS/PPS ids, H.264 VUI max_num_reorder_frames, POC type 1, MMCO 5, multiple slices per
  picture (H.264 + independent HEVC slices), start at the first key frame after open/flush, missing references
  substituted + picture marked corrupt, PMHWDEC_UNSUPPORTED for streams beyond the chip (Main 10 on A133P, interlaced,
  scaling lists, tiles), engine reset after a timeout, one engine per process with a mutex.
- **Cached picture buffers**: uncached ION made CPU copy-out cost 32 ms per 1080p frame (the decode is 5-11 ms).
  Now ION_FLAG_CACHED + sunxi's flush (ioctl 5 on the ION fd, clean+invalidate of a user VA range; ION_IOC_CUSTOM
  form as fallback, uncached if neither) after clearing at allocation and before handing a picture out:
  copy-out 2.7 ms per 1080p frame, flush 0.2-0.3 ms in pmhwdec_get. PMHWDEC_CACHED=0 = old behavior.
- **Kernel oops with dmabuf import (fixed 2026-10-30 21:45)**: 4.9 ION zaps the user mapping of a cached buffer
  whenever a device maps its dmabuf (ion_map_dma_buf → sync_for_device). After the Construct Native runtime
  imported pictures into EGL (Mali), the next export's sunxi flush walked the now unmapped user VAs. The result was an
  oops in `__dma_flush_range` (from ion_ioctl), with the process stuck in do_exit holding mmap_sem; the RG Cube XX
  wedged three times (load 51, SSH dead until a reboot). Found by that session with a dmesg log kept on the SD card.
  Fix: cedar_export reads one byte per page first (ION's fault handler maps them back), then flushes. All 12 clips
  have the same checksums as before, with no speed change, on the RG Cube XX.
  Still open: Mali (kbase) can map an import per GPU job. If a caller puts a picture back while a draw using it is
  still in flight, a map can land between the touch and the flush. Callers should wait for the GPU (fence/glFinish)
  and drop the EGL image before pmhwdec_put. That is a rule for the API docs, since the library cannot see it.
- **Zero-copy per GPU (from the Construct Native runtime):**
  - **R36S 2GB (Mali):** the single-NV12-image import works, 1080p H.264 35.9 → 15.0 ms/frame. Per-plane R8 is
    refused (0x3009).
  - **TrimUI Smart Pro (PowerVR GE8300, 2026-10-31):** no YUV import at all. eglQueryDmaBufFormatsEXT lists only
    AB30 AR24 AB24 XR24 XB24 RG16, and both R8 and NV12 are refused (0x3009). Frames are copied: 1080p ~26 ms/frame,
    720p ~11. The picture is right, and 5 runs gave no oops with the page-touch fix.
    **Zero-copy works with RGBA-packed planes:** each NV12 plane is imported as AB24 at width/4 (Y 480x1080, UV
    480x540 for 1080p; same fd, offset and pitch) and a GLSL ES 3.00 shader unpacks the bytes. 1080p H.264 26.3 →
    19.3 ms/frame, 720p 11.4 → 7.5 (upload + convert 13.9 → 4.9 and 7.4 → 3.0 ms). Frame 100 differs from the
    copied one by 0.63/255 on average; no oops. Runtime import order: R8/GR88 planes → AB24-packed → one NV12
    external image (RMMV Native rmmv-native/src/yuvgl.rs, uncommitted 2026-10-31).
- **Tiled vs linear copy-out (tools/bench_copyout.c, 2026-11-07):** the Allwinner media-codec route (32x32-tiled
  output, untiled on the CPU, plus one more copy, as GreenOvercast does it) against our linear copy-out. RG Cube XX
  (H700, schedutil): 1080p 14.45 vs 2.56 ms, 720p 6.41 vs 1.12 ms. R36S 2GB (A35, 1008 MHz): 1080p ~12.7 vs 1.85,
  720p 5.8 vs 1.1. The cache flush and the GPU upload both routes pay when copying are not included.

RG Cube XX (H700), pmhwdec-test through the public API, cached buffers (results/cubexx/lib):

| Clip | vs FFmpeg (output order) | send (decode) per frame | copy-out (1080p uncached was 32 ms) |
|--|--|--|--|
| seq_p_cavlc / seq_b_pyramid / seq_720p_temporal | 60/60, 60/60, 30/30 identical | 0.9 / 1.3 / 4.9 ms | |
| h264_slices (720p, 4 slices, B) | **60/60** | 5.0 ms | 1.2 ms |
| BBB H.264 1080p | **300/300** | 10.7 ms | 2.8 ms |
| hevc_intra / pb / pb_wpp | 10/10, 60/60, 60/60 | 0.7-0.9 ms | |
| hevc_slices (720p, 4 slices) | **60/60** | 2.5 ms | 1.2 ms |
| HEVC 720p Main 10 / BBB 1080p Main 10 | 30/30 truncated match (first 30) | 4.4 / 6.7 ms | 1.2 / 2.7 ms |
| BBB HEVC 1080p | **300/300** | 4.8 ms | 2.6 ms |

TrimUI Smart Pro (A133P), rerun with keep-awake (results/smartpro/lib): **all 10 8-bit clips bit-exact** (incl. both
4-slice clips and BBB 1080p H.264 300/300 at 10.3 ms, BBB HEVC 300/300 at 6.3 ms; copy-out 2.9-3.2 ms at 1080p);
both Main 10 clips refused with PMHWDEC_UNSUPPORTED before any decode; caps say HEVC 8-bit.
Keep-awake: Knulli's idlewatcher hooks (/etc/idlewatcher/{idle,extended}.d) skip dim + suspend while any
/var/run/battery-saver/*.pause exists; probe/run_lib.sh creates pmhwdec.pause and removes it on exit (300 s idle =
dim + optional powersave governor, 900 s = suspend on this unit).
Earlier: the device dropped off the network during bbb_h264 (1080p) and stayed offline (> 2 min):
**Cause (user, 2026-10-30): the Smart Pro goes to sleep** (Knulli idle suspend; the test runs send no input; dmesg
already showed "[cedar] standby suspend/resume"). Not an engine freeze. Device test scripts need a keep-awake for the
run, then the remaining Smart Pro clips can be rerun.

## rkmpp backend (2026-10-30): **bit-exact on the RG552 (RK3399, AmberELEC 4.4)**
| Clip (RG552, pmhwdec-test through the public API) | vs FFmpeg |
|--|--|
| seq_p_cavlc, seq_b_pyramid, seq_720p_temporal, h264_slices | 60/60, 60/60, 30/30, 60/60 identical |
| BBB H.264 1080p | **300/300** |
| hevc_intra, hevc_pb, hevc_pb_wpp, hevc_slices | 10/10, 60/60, 60/60, 60/60 |
| BBB HEVC 1080p | **300/300** |
| HEVC 720p Main 10, BBB Main 10 | PMHWDEC_UNSUPPORTED (MPP output format 0x1 = NV15, stride 1600 / 2816 bytes) |

Throughput (`pmhwdec-test -s`: send/get only, no checksum/copy; MPP decodes in its own thread): **BBB H.264 1080p 236 fps,
BBB HEVC 1080p 482 fps**; CPU ~94% idle in the snapshot after. Copy-out from the cached DRM buffers 1.2-1.3 ms/1080p.
Two fixes on the way:
- MPP's link misses libm (`undefined symbol: cos` on AmberELEC, glibc 2.38): build_mpp.sh links -lm.
- **MPP misdetects the RG552 as an RK3326**: its device tree compatible is "rockchip,rk3326-rg552-linux
  rockchip,rk3399"; MPP joins the strings and strstr()s its chip table from the end, where "rk3326" comes first ->
  H.264 worked (shared VPU node) but HEVC opened the RK3326's missing /dev/hevc_service ("open vcodec_service (null)
  failed"). `rkmpp/patch_soc_name.py` (applied by build_mpp.sh) keeps only plain "rockchip,rkNNNN" compatibles.

**RG353V (RK3566, dArkOS, kernel 5.10, /dev/mpp_service root:video 0660)**, 2026-10-30: all 10 8-bit clips
bit-exact; BBB 1080p H.264 **241 fps**, HEVC **463 fps** (`-s`); Main 10 -> UNSUPPORTED (NV15, stride 2496 at 1080p).
Ports run as root there (`sudo -n` works for ark); run_lib.sh re-execs with sudo -n when that needs no password.
**R36S 2GB (RK3326, dArkOS, vendor 4.4.189, /dev/vpu_service + /dev/hevc_service, compatible
"rockchip,rk3326-odroidgo3-linux rockchip,rk3326")**, 2026-10-30: all 10 8-bit clips bit-exact incl. HEVC (the RK3326
HEVC block, which mainline has no driver for); BBB 1080p H.264 **69 fps**, HEVC **317 fps** (`-s`); Main 10 -> NV15 ->
UNSUPPORTED for now. (The original R36S is out of service: bad USB-C OTG port.)

**RG351P (RK3326, ROCKNIX, kernel 7.1.2)**: no MPP nodes; hantro `rockchip,px30-vpu-dec` = /dev/video1 (+ media0),
encoder /dev/video0. ROCKNIX's own mpv 0.41 / libavcodec 61 already has the v4l2request hwaccels (h264, hevc, mpeg2,
vp8, vp9) + v4l2m2m. So ROCKNIX's player decodes in hardware already; ports bundle their own libs and need our v4l2req
backend on the same kernel interface (RK3326 mainline: H.264 / VP8 / MPEG-2 only, no HEVC driver).

## v4l2req backend (2026-10-30): **bit-exact on the RG351P (RK3326 hantro, ROCKNIX 7.1.2)**
`src/v4l2req.c` (HwOps for the stateless core): finds a /dev/video* M2M-mplane device taking V4L2_PIX_FMT_H264_SLICE
with FRAME_BASED decode mode + ANNEX_B start codes, and its /dev/media* (sysfs `device/media*`, else driver name).
Per picture: all slices with start codes in one OUTPUT buffer; SPS / PPS / flat scaling matrix / decode params (DPB
from the shared codec layer, references named by the capture buffer's timestamp in ns) as controls of one media
request; capture buffer index = picture slot (one queued per decode, synchronous); poll the request, DQBUF both,
REINIT. New HwOps hook `end_picture` (frame-based decode after the last slice, before reference marking). Parser now
keeps the V4L2-only fields (SPS id / constraint flags / level / gaps / qpprime, PPS id / init_qs, idr_pic_id,
pic_order_cnt_bit_size, dec_ref_pic_marking_bit_size). Mainline uapi headers vendored in src/uapi/linux
(videodev2.h, v4l2-controls.h, v4l2-common.h, media.h; dual GPL-2.0+ syscall-note / BSD-3) + a compiler.h stub
(__user etc.), first on the include path: the bullseye sysroot's 5.10 headers predate the stable stateless API.

| RG351P (hantro px30-vpu-dec, /dev/video1 + /dev/media0) | vs FFmpeg |
|--|--|
| seq_p_cavlc, seq_b_pyramid, seq_720p_temporal, h264_slices | 60/60, 60/60, 30/30, 60/60 identical |
| BBB H.264 1080p | **300/300**; decode 17.7 ms/frame = **56 fps** (`-s`) |

**Copy-out fixed (2026-10-30)**: hantro's MMAP capture buffers are coherent = uncached (no
V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS): 34 ms per 1080p CPU copy. Now V4L2_MEMORY_DMABUF capture buffers from
/dev/dma_heap/system (cached, non-contiguous; used only when the video device has an iommu_group in sysfs) +
DMA_BUF_IOCTL_SYNC START/END READ in pmhwdec_get: **copy-out 4.2 ms per 1080p frame** (A35), sync 0.7 ms, decode
unchanged at 17.7 ms, all five clips still bit-exact (results/rg351p/lib-heap). Fallbacks: MMAP with the
non-coherent hint, then plain MMAP; PMHWDEC_V4L2_DMABUF=0 forces MMAP. uapi dma-heap.h / dma-buf.h vendored too.
Not done yet: HEVC over V4L2, VP9.
- **Allwinner VP9: parked (user 2026-11-07).** H700 and A133P have a separate block, vp9@01c00000
  (`allwinner,sunxi-google-vp9`, the Google/Hantro G2 design), not part of Cedar. Knulli binds no driver to it and
  gives it no device node, so it would need a kernel module (mainline hantro has `sun50i-h6-vpu-g2`) plus our own
  VP9 parser and probability adaptation. Software VP9 on the RG Cube XX is already about 10 ms per 1080p frame on
  two threads, so the only gain would be battery and CPU.

**What ROCKNIX actually enables** (release 20261001 device list + `projects/ROCKNIX/devices/*/linux/linux.aarch64.conf`
on the `next` branch, checked 2026-10-30, user's point):
- H700 (RG35XX family, RG40XX, RG Cube XX, RG28XX, RG34XX): **no Cedrus** in the kernel config (and no hantro):
  no hardware decoding on ROCKNIX H700 at all -> the slice-based (Cedrus) V4L2 path serves no device: dropped.
  **Update 2026-11-01:**
  - **Mainline:** got "staging: media: sunxi: cedrus: add H616 variant" on 2026-10-04 (in master after v7.3, so
    kernel 7.4). sun50i-h616.dtsi now has the video-codec@1c0e000 node, with IOMMU and SRAM, enabled.
  - **ROCKNIX:** `next` is on 7.2.0 with `# CONFIG_VIDEO_SUNXI is not set` (checked today). Once it moves to 7.4+,
    turning on CONFIG_VIDEO_SUNXI + CONFIG_VIDEO_SUNXI_CEDRUS is all ROCKNIX H700 needs.
  - **GreenOvercast** (Producdevity, an MPL cloud-gaming client) works around this today with two out-of-tree
    modules: Cedrus patched with the H616 match + SRAM syscon bit, and a DT-overlay module that adds the VE node
    (no IOMMU; the Cedrus driver uses CMA). Its launcher loads them with $ESUDO and unloads them on exit.
  - **Their modules only fit one build:** ROCKNIX 20260801 official, kernel 7.0.11, exact BUILD_ID and
    Module.symvers. Current ROCKNIX is 20261101, so they already fall back to software there.
  - **Not worth copying:** every ROCKNIX release would need a rebuild.
  - **Our side either way:** Cedrus H.264 is slice-based only (DECODE_MODE_SLICE_BASED, START_CODE_NONE), plus
    SLICE_PARAMS/PRED_WEIGHTS per slice and V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF for multi-slice pictures.
    Cedrus HEVC needs HEVC over V4L2, which is still open.
  - **Slice-based H.264 in src/v4l2req.c (written 2026-11-01; bit-exact on Cedrus 2026-11-07, below):**
    - **Mode choice:** find_device takes frame-based + Annex-B first (hantro/rkvdec, unchanged), else slice-based
      with START_CODE_NONE (else Annex-B).
    - **Requests:** one per slice, synchronous. Each carries SPS/PPS/scaling/decode params + SLICE_PARAMS, plus
      PRED_WEIGHTS when weighted prediction applies. Ref lists map slot -> dp.dpb[] index, and header_bit_size =
      our header_bits (from the NAL header byte, as Cedrus skips it).
    - **Capture buffer:** each slice waits until the next arrives, so all but the last go with
      V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF. Without that capability only single-slice pictures work.
    - **Failures:** a failed picture with the capture buffer still queued is recovered by capture
      STREAMOFF/STREAMON (now in frame mode too, where a failed request used to leave the buffer queued).
    - **Frame-based retest (done 2026-11-07, after the single-planar and HEVC changes):** RG351P (hantro) all
      6 H.264 clips bit-exact again, self-test OK, HEVC clips correctly NOHW. 1080p `-s` now 50.7 fps (was 56 on
      2026-10-30), but the pre-refactor build gives the same 50.2-50.8 in the same session: device conditions, not code.
  - **Test rig (2026-11-07): Cedrus as temporary modules on the RG34XX SP (ROCKNIX 20261101, kernel 7.2.0),
    rocknix/:**
    - **What it is:** a dev target for the slice-based path (and later HEVC over V4L2), not something to ship.
      The modules only load on the build they were made for, like GreenOvercast's.
    - **build_cedrus.sh (WSL):** linux-7.2 + ROCKNIX's patches at the device's BUILD_ID, the device's own
      /proc/config.gz (config-20261101-h700) plus VIDEO_SUNXI=y / VIDEO_SUNXI_CEDRUS=m. Two options name files
      on ROCKNIX's CI and are cleared (INITRAMFS_SOURCE, EXTRA_FIRMWARE). Then mainline 1f24d4307515 (the H616
      match), built as sunxi-cedrus.ko, plus pmhw_h616_ve.ko (h616_ve.dts, applied with of_overlay_fdt_apply).
      Cedrus in 7.2 needs no v4l2-h264 module.
    - **Symbol versions:** the rebuilt tree's modversions CRCs all differ from the shipped kernel's (_printk
      too: ROCKNIX builds with GCC 16.2, WSL has 13.3), and insmod refuses them. kcrc_dump.py, run on the device,
      reads the real ones out of /flash/KERNEL (uncompressed Image) at the /proc/kallsyms addresses. That file
      is Module.symvers-20261101-h700, and the modules link against it. Checked: kfree matches ROCKNIX's own
      hci_uart.ko.
    - **No IOMMU in the overlay:** mainline's node has `iommus = <&iommu 3>, <&iommu 2>`. On 7.2 that clears
      the IOMMU's bypass register as soon as the VE attaches. The display engine (master 0) has no domain
      there, so it faulted on its framebuffer non-stop (1,300 faults in 20 s, black screen) until a reboot.
      Without it Cedrus uses CMA (32 MB pool), as in GreenOvercast's overlay.
    - **Load:** `insmod pmhw_h616_ve.ko; insmod sunxi-cedrus.ko` gives /dev/video0 "cedrus" + /dev/media0.
      Unload in reverse.
  - **Results on Cedrus (RG34XX SP, 2026-11-07), after two fixes:**
    - **Fix 1, single-planar API:** Cedrus exposes V4L2_CAP_VIDEO_M2M (plain OUTPUT/CAPTURE), not the
      multi-planar API hantro has. find_device skipped it, so "no hardware". v4l2req now takes both (Dev.mplane,
      OUT_T/CAP_T, buf_init/buf_set).
    - **Then:** self-test OK, and every H.264 clip bit-exact against FFmpeg: seq_p_cavlc 60/60,
      seq_b_pyramid 60/60, seq_720p_temporal 30/30, h264_slices 60/60 (4 slices: HOLD_CAPTURE_BUF works) and
      bbb_h264 1080p 300/300. header_bit_size from the NAL header byte was right as written.
    - **Fix 2, cached capture buffers without an IOMMU:** uncached MMAP copy-out was 29.7 ms per 1080p frame
      and 13 ms at 720p. With no IOMMU, heap_buffers now takes a CMA dma-heap (default_cma_region, the pool's
      kernel name is "reserved"). It fits once slice mode uses 2 input buffers of w*h*3/4 (at least 1 MB)
      instead of 4 x 3 MB, which had fragmented the pool.
      - 720p: copy-out 13.4 -> 1.4 ms, total 30.8 -> 18.8 ms per frame.
      - 360p: 3.15 -> 0.3 ms.
      - 1080p: ten 3.1 MB capture buffers can't fit 32 MB, so it falls back to uncached MMAP (still
        exact; zero-copy consumers don't copy anyway).
    - **Once ROCKNIX ships Cedrus itself (7.4+ with the mainline node and the IOMMU wired up for the display
      too):** the system-heap path applies, as on hantro.
  - **HEVC over V4L2, slice-based (2026-11-07, same rig): bit-exact on Cedrus.**
    - **v_hevc_slice:** one request per slice segment, like H.264. Controls: SPS, PPS, DECODE_PARAMS (DPB =
      reference slots with their capture timestamps, poc_st_curr_before/after from the slice's RPS, IRAP/IDR
      flags), SLICE_PARAMS (ref_idx_l0/l1 = DPB indices, pred weight table with delta_chroma_log2_weight_denom
      = chroma - luma denom), and ENTRY_POINT_OFFSETS when the slice has entry points (up to 256).
    - **data_byte_offset = our data_byte:** the RBSP byte where slice data starts, counted from the NAL header.
      Cedrus skips (offset - 1) * 8 bits with the engine's emulation-prevention-aware reader, then reads the
      byte_alignment() byte. **Entry points = ep_unescaped** (unescaped offsets - 1), which Cedrus copies into the
      engine unchanged, as cedar.c does.
    - **8-bit results:** hevc_intra 10/10, hevc_pb 60/60, hevc_pb_wpp 60/60 (WPP), hevc_slices 60/60
      (multi-slice) and bbb8 1080p 300/300, all against FFmpeg. Self-test OK. H.264 rerun unchanged.
    - **10-bit:** Cedrus has H265_10_DEC on H6/H616 and outputs the top 8 bits as NV12, with the low 2 bits in an
      extra area after it, the same as cedar.c. The bit depth sizes the capture buffers, so v_configure sets an
      SPS (CUR_VAL) between S_FMT output and G_FMT capture. v_query offers 10 for HEVC and the self-test decides
      ("hevc10: OK").
      - Compared with the cedar backend on the RG Cube XX (same engine, verified against FFmpeg in October):
        bbb10 300/300 and hevc_720p_10bit 30/30 identical frame CRCs.
      - The kit has no reference CRCs for those two clips; tools/dev_clip.sh prints 0/0 for them.
    - **Frame-based HEVC (rkvdec, hantro G2: all of a picture's slice params as one array control):** not
      written. find_device skips HEVC devices without slice-based mode.
  - **Debug loop:** tools/dev_clip.sh `<hub id> [clip ...]` pushes the kit to a device you hold, decodes with
    logging on and counts frames matching each clip's FFmpeg CRCs.
- RK3326: hantro (H.264 / VP8 / MPEG-2): done above.
- RK3399, RK3566, RK3576: CONFIG_VIDEO_ROCKCHIP_VDEC (rkvdec) + hantro: the only places where HEVC over V4L2 could
  matter; which formats each ROCKNIX kernel's rkvdec exposes needs a device on ROCKNIX (none on the hub now).
- RK3588: the config has Rockchip BSP-only options (ISPP, HDMI-RX): vendor kernel -> rkmpp, not V4L2.
- Qualcomm (SM8250 RP5 etc.): venus = V4L2 stateful (v4l2m2m), a different backend if ever wanted.

### Written before the first run

- `rkmpp/build_mpp.sh`: clones rockchip-linux/mpp at tag **1.1.0** (1.0.11 is broken upstream: its av1 CMakeLists
  names a missing file) into WSL ~/pmhwdec/mpp and cross-builds `out/aarch64/mpp/librockchip_mpp.so` (needs only
  libc/libm/pthread, SONAME librockchip_mpp.so.1) + LICENSE.mpp.txt. Needs -fPIC on all objects. User approved the clone.
  **Cut to the bone 2026-11-07: 539,088 bytes stripped, 254,930 gzip -9** (stock full build 2.87 MB). Steps, each
  measured stripped / gzip -9: other codecs + encoders off (mpp/codecs.cmake), -Os, function/data sections, lld
  --gc-sections, exports = all 301 inc/*.h functions: 1,089,984 / 463,710 -> exports = only the 28 functions
  src/rkmpp.c dlsym()s (read from its SYM() list at build time; any stock MPP has them): 996,200 / 429,241 ->
  decoder IP list (`rkmpp/patch_hw_list.py`, MPP_HW_LIST=VDPU2,VDPU341,VDPU34X): 902,816 / 393,537 -> no unwind tables
  (MPP is plain C, no C++ / pthread_cancel / backtrace): 816,256 / 370,389 -> encoder framework out
  (`rkmpp/patch_no_enc.py`: mpp_enc_cfg / mpp_enc_impl / rate control / vepu HAL commons / kmpp venc leave the CMake
  lists, mpp/codec/pmhwdec_noenc.c stubs the 12 symbols the decoder side still names, mpp_init(MPP_CTX_ENC) -> MPP_NOK):
  593,968 / 279,487 -> env-gated mpp_dbg() traces compiled out (`rkmpp/patch_no_dbg_log.py`, -DPMHWDEC_NO_DBG_LOG;
  errors/warnings stay; `MPP_DBG_LOG=1 bash rkmpp/build_mpp.sh` keeps them for chasing a device problem):
  539,088 / 254,930. Linked with -z defs, so a patch that misses a symbol fails the build, not the dlopen().
  HALs kept (each registers only on the SoCs in its .soc_type): h264d_vdpu2 (RK3326/PX30 H.264; 2nd choice on
  RK3399/3328/356x/3588), h264d_rkv_reg = VDPU341 (RK3399/RK3328 H.264), h265d_rkv (RK3326/PX30 hevc_service and
  RK3399/RK3328 rkvdec HEVC), h264d/h265d_vdpu34x (RK3566/7/8 and RK3588, whose VDPU381 core runs this HAL). Cut:
  vdpu1 (RK3036/3066/3188/3288/312x/3368), vdpu382 (RK3528/3562), vdpu383 (RK3576), vdpu384a (RV1126B), vdpu384b
  (RK3572/3538/3539). Measured and not taken: ENABLE_VPROC=OFF (-19 KB) drops IEP deinterlacing of interlaced
  streams on RK3399/RK3566 (MPP's default); -Oz + thin LTO + ICF was smaller stripped but gzipped bigger.
  R36S 2GB (RK3326, dArkOS) kit run with this build: self-test OK, all 10 8-bit clips bit-exact, per-frame send/get
  times equal to the 1,089,984 build run right after (e.g. BBB H.264 send 4.48 vs 4.49 ms, get 0.705 vs 0.712 ms).
- `src/rkmpp.c`: stream backend. dlopen()s librockchip_mpp (next to libpmhwdec / pmhwdec-test first, then the system),
  only the needed symbols (`*_with_caller` / `*_f` forms of the buffer macros). Offered when a Rockchip decoder node
  exists (/dev/mpp_service, rkvdec, hevc_service, vpu_service, vpu-service) and mpp_check_support_format() agrees.
  Whole access units (split mode 0), non-blocking get, MPP_ERR_BUFFER_FULL -> PMHWDEC_AGAIN, EOS waits up to 2 s for
  MPP's EOS frame. Output from a cached DRM buffer group (plain DRM, then ION as fallbacks), mpp_buffer_sync_begin
  (read-only) before the CPU gets a frame. 10-bit (NV15) and FBC formats -> PMHWDEC_UNSUPPORTED for now.
- Built against MPP headers (Apache-2.0) at compile time; the .so is shipped next to libpmhwdec by a port.
- Test plan: RG552 (AmberELEC 4.4, RK3399: rkvdec + vpu_service, root-only nodes), then RG353V / R36S (dArkOS).

## FFmpeg / mpv glue (2026-10-30): **plays in mpv on the RG Cube XX**
`glue/ffmpeg/pmhwdec_ff.c`: FFmpeg decoders `h264_pmhwdec` / `hevc_pmhwdec` (same pattern as the Jellyfin port's
h264_cedar): dlopen libpmhwdec.so(.1), pmhwdec_query + a check of the container's profile / bit depth / size against the
caps in init (refuse = the player falls back to software; mid-stream it could not), mp4toannexb bsfs, NV12 output
copied from the (cached) planes, PMHWDEC_AGAIN handled by keeping the packet, PMHWDEC_UNSUPPORTED mid-stream =
AVERROR_PATCHWELCOME, corrupt pictures flagged. Env PM_HWDEC=0 disables.
Jellyfin port wiring: tools-src/pmhwdec/ (copies of the glue + pmhwdec.h), build_libmpv.sh `ffmpeg_add_pmhwdec` (FFmpeg
7.1.2), release/jellyfinmpvshim/pmhwdec_setup.sh (pmhwdec-test -q -> PM_HW_VD / PM_HW_HEVC10, turns Transcode HEVC off once),
launcher sources it and only falls back to cedar_fetch.sh (Allwinner blobs) when it found nothing, run.py uses
PM_HW_VD and keeps the "10-bit only for H.264" profile tweak only where HEVC 10-bit decodes, stage_release.sh stages
libs/pmhwdec (libpmhwdec.so, pmhwdec-test, librockchip_mpp.so) + LICENSE.pmhwdec / LICENSE.mpp + src/pmhwdec.
Test (Jellyfin pctest/device_pmhwdec.sh: new libmpv + libpmhwdec from /tmp, installed port untouched), RG Cube XX:
pmhwdec_setup -> `hevc_pmhwdec,h264_pmhwdec,` + HEVC10; BBB HEVC 10-bit 1080p and H.264 1080p MKVs (5.1 AC-3): mpv picks the
pmhwdec decoders, **0 decoder drops, 30 fps, backward seek OK**; frame grabs clean (results/cubexx/mpv).
**Open before a release: the license of libpmhwdec** (see below), then a zip, then the other devices.

### License: library **LGPL-2.1-or-later**, Allwinner decoder a separate **GPL** program (user decision 2026-11-07)
- **Why:** a GPL library can't be loaded into a closed program (portmaster-unity's Unity players). Only src/cedar.c
  derives from Cedrus, so the split was made there.
- **What changed:** everything else (core, parsers, self-test, rkmpp, v4l2req, v4l2m2m, tools) is our own code and
  went to LGPL-2.1-or-later. cedar.c + ve_regs.h + cedard.c build `pmhwdec-cedard` (GPL-2.0-or-later,
  LICENSE.pmhwdec-cedard), a helper program the library starts next to itself.
- **Protocol (src/cedar_client.c <-> src/cedard.c, src/cedar_ipc.h):**
  - One helper per decoder, over a socketpair on fd 3 (posix_spawn).
  - Access units go over as bytes. Pictures come back as ION dma-buf fds (SCM_RIGHTS), mapped once per buffer
    (cached by inode, dropped when the helper reallocates). The helper does the cache flush before each picture.
  - OPEN carries the client's self-test depth.
  - 3 s reply timeout: a helper that dies or hangs ends the stream with PMHWDEC_UNSUPPORTED (software).
  - The helper exits on socket EOF.
- **Verified 2026-11-07** through the tester kit:
  - RG Cube XX (Knulli, H700): all 12 clips, 8-bit bit-exact, 10-bit self-test OK, kernel log clean.
  - Cost of the process boundary: about 1 ms per 1080p frame (H.264 send 10.2 -> 11.4 ms, get 0.6 ms).
- **Before (2026-10-30, GPL-2.0-or-later):** src/cedrus_regs.h was mainline's header verbatim (GPL-2.0-only), so
  the library was GPL-2.0-only, which does not combine with the Jellyfin port's LGPLv3 FFmpeg. The fix then:
- `src/ve_regs.h`: the 182 register offsets / bit fields / values cedar.c uses, as plain numbers, generated by
  `tools/gen_ve_regs.py`; `tools/check_ve_regs.sh` compiles cedar.c against a fresh download of mainline's header and
  against ours: **.text (11580 bytes), .rodata and .data identical**. The verbatim copy is gone from src/ (and the repo).
- Ported logic comes only from cedrus_h264.c / cedrus_h265.c (GPL-2.0-or-later); from cedrus_hw.c (GPL-2.0-only)
  only register values (facts, now in ve_regs.h).
- `LICENSE` (now LGPL-2.1 text + a per-part list), SPDX tags on every source. `include/pmhwdec.h` is MIT (any program
  can include it); the FFmpeg glue is LGPL-2.1-or-later like FFmpeg; uapi headers keep theirs; MPP is separate (Apache/MIT).
- probe/ (research programs, not part of the library) still has the verbatim GPL-2.0 HEVC register header.

## rkmpp 10-bit HEVC (2026-10-30)
MPP outputs 10-bit as NV15 (4 samples in 5 bytes); rk_get converts to NV12 keeping the top 8 bits (truncation, as on
Allwinner) into a pool of 8 reusable buffers (no dmabuf for these). Results vs FFmpeg's truncated 10-bit decode:
RG353V (RK3566) and RG552 (RK3399) **30/30 + 30/30 match**, 1080p Main 10 192 / 176 fps incl. conversion (~4.5 ms per
frame in pmhwdec_get); 8-bit regression 300/300 on all three. **RK3326 (R36S 2GB): 0 matches** (its HEVC block is Main
only; MPP accepts the stream anyway) -> soc_is_rk3326() (plain "rockchip,rk3326"/"px30" compatible) caps HEVC at 8
bit and refuses NV15 there with PMHWDEC_UNSUPPORTED (checked on the R36S 2GB).

## v4l2m2m backend (2026-10-30): stateful V4L2, **RP5 (SM8250, ROCKNIX 7.2, qcom-iris)**
`src/v4l2m2m.c`: stream backend per dev-decoder.rst (source-change init, MIN_BUFFERS_FOR_CAPTURE + extra, COMPOSE crop,
DEC_CMD_STOP drain, OUTPUT restart on seek, pts in buffer timestamps; Venus/Iris luma padded to 32 lines; cached
dma_heap capture when the device has an iommu_group, else MMAP). Guards: no capture setup within 3 s of being fed ->
PMHWDEC_UNSUPPORTED (Iris refuses 10-bit silently: dmesg "unsupported content, bit depth: 20002"); no progress 5 s ->
PMHWDEC_ERROR. pmhwdec-test got a stuck guard. Order in pmhwdec.c: cedar, rkmpp, v4l2req, v4l2m2m.
| RP5 | result |
|--|--|
| HEVC intra / P/B / WPP / 4 slices | **all bit-exact** (10, 60, 60, 60) |
| BBB HEVC / H.264 1080p | **552 / 572 fps** (`-s`) |
| HEVC Main 10 | UNSUPPORTED after ~3 s (driver refuses 10-bit) |
| H.264 | mostly exact, but **nondeterministic ±1 deviations**: run 1 seq_p_cavlc 48/60, seq_720p_temporal 29/30, BBB 24/300 exact; run 2 seq_p_cavlc 60/60 identical; BBB frames 6-7: 1-5 samples of 3.1 M off by 1 (PSNR 106-113 dB). Same bytes are exact on the other 5 devices: Iris firmware/hardware, invisible. |

Jellyfin zip re-staged with this build (libpmhwdec incl. v4l2m2m). qemu smoke test fails since today with the OLD
libmpv too ("Suspected software renderer" under Xvfb): a WSL Mesa/Xvfb environment change, not this work; to fix later.

## Startup self-test + tester kit (2026-10-31): support for chips without a test device

For chips no hub device has (RK3588, RK3328, RK3576, H616/H618, Rockchip HEVC on ROCKNIX ...), the library checks
itself instead of being trusted blind:
- **Known-answer test** (src/selftest.c, data from tools/gen_selftest.py → src/selftest_data.h, ~10 KB of clips):
  before a backend is used for a codec, pmhwdec_query / pmhwdec_open decode a built-in 160x96, 6-frame clip with it (H.264
  High with B-frames and weighted prediction, HEVC Main, HEVC Main 10; frame order I B B P B P). Every frame's 8x8
  block means (Y, U, V) must be within 2 of FFmpeg's decode. Block means rather than checksums: the RP5's rare ±1
  sample deviations don't trip it, while wrong formats, colors, references or order move means by tens.
- Failing the 8-bit test means "no hardware" for that codec, so the caller decodes in software. Failing the 10-bit
  test caps max_bit_depth at 8, and backends that only see the depth in the stream's headers refuse such streams
  (sl_configure; rkmpp at the NV15 info change).
- Each clip runs in its own thread with a 5 s limit. A decoder that hangs disables that backend for every codec.
- Results are cached in /tmp/pmhwdec-selftest.<uid>, keyed by boot_id and library build. PMHWDEC_SELFTEST=0 skips
  it, =1 ignores the cache. PMHWDEC_SELFTEST_INJECT=<n> adds n to every measured mean (proves failures are caught).
- Measured 2026-10-31: RG Cube XX 23 ms fresh / 8 ms cached (H.264, HEVC, HEVC10 OK). TrimUI Smart Pro 31 / 13 ms
  (H.264, HEVC OK; 10-bit not tried, caps say 8). INJECT=10 on both: every codec "no hardware" with the reason
  logged. The clip suite after it has the same checksums as before on both.
- **Tester kit** (tools/make_kit.sh → out/pmhwdec-test-kit.zip, 14 MB): "PMHWDEC Test.sh" + pmhwdec-test/
  (pmhwdec-test with the library linked in, MPP, 12 clips + FFmpeg framecrc references, run.sh). It writes
  ports/pmhwdec-report.txt with model, compatible, kernel, firmware, decoder device files, V4L2 device names, the
  self-test, per-clip speed and frame-by-frame exactness (10-bit: self-test only), and decoder kernel messages.
  Root via sudo -n where needed (dArkOS), Knulli keep-awake, nothing left behind. Dry run over SSH on the RG Cube
  XX 2026-10-31: all 8-bit clips exact. The front-end launcher itself has not been run from a menu yet.

## What the firmwares already ship (checked read-only 2026-10-30)
| Device / CFW | Players | Hardware decode available to them |
|--|--|--|
| RG Cube XX, Knulli (H700, 4.9) | ffmpeg, mpv, gst, vlc | none: libavcodec has only the generic `*_v4l2m2m` (stateful) wrappers, and the kernel has no V4L2 decoder device (only v4l-subdevs + /dev/media0). No libcedarc/libVE anywhere. |
| TrimUI Smart Pro, Knulli (A133P, 4.9) | same | none, same as the Cube |
| RG552, AmberELEC (RK3399, 4.4) | mpv, vlc (no ffmpeg binary) | none: libavcodec has no rkmpp, no librockchip_mpp on the system |
| ROCKNIX (all, mainline) | not checked (devices offline) | **likely yes in its own players**: LibreELEC lineage = FFmpeg v4l2-request + cedrus/hantro/rkvdec. Verify. |
| dArkOS / ArkOS, muOS | not checked | unknown; muOS forum: mpv's GPU output broken on the Brick |

- So on vendor-kernel firmwares nothing is duplicated: there is no hardware path at all. On ROCKNIX the kernel drivers
  exist and the v4l2req backend uses them (no driver work), only the userspace side is ours.
- Even where a CFW has it, ports can't rely on it: PortMaster ports bundle their own libraries and run on every CFW.
- Prior art: libvdpau-sunxi (userspace VE over /dev/cedar_dev, pre-H6 chips, abandoned ~2016); FFmpeg's upstream
  `*_rkmpp` decoders and nyanmisaka/ffmpeg-rockchip (MPP, vendor kernels); LibreELEC's v4l2-request FFmpeg patches.
  The rkmpp and v4l2req backends should reuse these instead of rewriting them. Nothing found that does Cedrus-style
  userspace decoding for H6/H616/A133 on vendor kernels.

## Software baselines to beat (RG Cube XX, H700; from the Construct Native session, 2026-10-30)
Source: the Construct Native port's notes ("Video codecs on the RG Cube XX", "Install-time VP9 conversion + GPU
frame conversion"). 12 s of a 1080p H.264 High clip with B-frames, 30 fps, ms per frame, inside the RMMV Native runtime.

| Codec / size | Decode | Full path (decode + convert + upload) |
|--|--|--|
| H.264 1080p High, OpenH264 single thread (threaded decoder crashed) | ~89 | 104 (CPU convert), 97 (GPU convert) |
| H.264 720p Baseline | 13.0 | 25.4 CPU / 17.9 GPU |
| H.264 540p Main | | 33 |
| H.264 944x754 | | 35 |
| VP9 1080p, 2 tile columns, libvpx 2 threads (+row-mt) | 9.7-10.2 | 34.6 CPU / 19.4 GPU (21 with row-mt) |
| VP9 720p | 4.1 | 16.5 CPU / 8.8 GPU |

- Our engine: H.264 720p inter frames 4.7-5 ms, 1080p key frames not measured yet: roughly 15-20x faster than
  OpenH264 at 1080p, and H.264 1080p is the case where software fails (~10 fps).
- Frame hand-off cost on Mali-G31: tight I420 planes -> RGBA texture ~3 ms (720p) / ~5 ms (1080p) with their GPU
  shader (src/yuvgl.rs, R8 planes -> FBO); plane copy 1.7-3.8 ms more, which a dmabuf -> EGLImage path would save.
  So the API should hand out NV12 planes (small shader change for them) and dmabuf fds where EGL can import them.
- Their on-device transcode (OpenH264 -> VP9 720p) runs 6-7.5 fps on 1080p sources, paced by the H.264 decode: a
  user of this library too.
- Consumer waiting: Construct Native's src/video.rs Codec enum can take a C API returning planes or dmabufs (that
  line of work is paused by the user).

## Backends
One API, backends picked at run time by probing device nodes; each backend is its own .so, dlopen()ed, so a
missing or refused backend never breaks the port.

1. **rkmpp** (vendor Rockchip kernels): Rockchip MPP (`rockchip-linux/mpp`, Apache-2.0/MIT: we can build and ship
   it). MPP itself handles the 4.4 legacy nodes (vpu_service/rkvdec/hevc_service) and 4.19+ mpp_service. Covers
   most RK devices people own (ArkOS/dArkOS on R36S & clones, RG353, AmberELEC). Output NV12 (10-bit: NV15
   packed, needs unpacking or RGA), DRM buffers with dmabuf fds.
2. **cedar** (Allwinner vendor kernels): the existing Jellyfin bridge, moved here. Blobs fetched on device
   (licence decision of 2026-10-14 stays).
3. **v4l2req** (mainline: cedrus, hantro, rkvdec/rkvdec2): V4L2 stateless needs a bitstream parser to fill the
   slice/SPS/PPS controls. Plan: reuse FFmpeg's parsers through the out-of-tree v4l2-request hwaccel
   (Kwiboo/LibreELEC patches) inside a static minimal libavcodec in this backend, so callers still just send
   access units.
4. **v4l2m2m** (stateful: SD865 venus, Amlogic meson-vdec): later, lowest priority.
5. **sw**: none here: the caller keeps its own software decoder; open() just fails.

## API (generalizing CedarBridge, see pmhwdec.h)
Access units in (Annex-B), pictures out. Pictures carry CPU-readable planes (copy path, always) and, when the
backend has them, a dmabuf fd + DRM fourcc/modifier for zero-copy EGL import. Query call to ask which codecs /
max sizes / bit depths the current device can do before opening, so a player can tell its server to transcode.

## FFmpeg / mpv glue
`<codec>_pmhwdec` lavc decoders (same pattern as h264_cedar/hevc_cedar in cedardec.c): dlopen libpmhwdec, fall back
on init failure. The Jellyfin port switches from the cedar-only decoders to these.

## Phases
1. API header + rkmpp backend + `pmhwdec-test` CLI (fps, frame dump, PSNR vs FFmpeg); cross-built in WSL.
2. Device runs: RG552 (AmberELEC 4.4 rk3399), R36S (dArkOS 4.4 rk3326), RG353V (dArkOS rk3566).
3. Move cedar backend in; check A133P (Smart Pro, Brick).
4. v4l2req backend on ROCKNIX (RG351P rk3326, RGB30 rk3566, RP5 later).
5. FFmpeg glue, Jellyfin port switch, dmabuf zero-copy path.

## Open questions
- dArkOS on RK3566: which kernel / node (mpp_service expected). Ports there run as root? (RG552 nodes are 0600 root.)
- RK3326 HEVC on mainline: nothing upstream; ROCKNIX patches?
- 10-bit output on RK: NV15 -> 8-bit conversion cost on A35/A55 (RGA can do it on vendor kernels).

Sources: [CNX: RK3588/RK3576 decoders in Linux 7.0](https://www.cnx-software.com/2026/02/27/rockchip-rk3588-rk3576-h-264-and-h-265-video-decoders-mainline-linux/),
[LKML: VDPU346 v3](https://lkml.org/lkml/2026/1/18/415), [Pine64 mainline decoding table](https://pine64.org/documentation/General/Mainline_Hardware_Decoding/).

## VP8 (2026-11-08)
One parser (`src/vp8.c`) feeds cedar and v4l2req; MPP parses VP8 itself (`-DENABLE_VP8D=ON`, needs the VDPU2 HAL already in the build).
- The Cedrus table at 0x1008 / 0x1010 must hold the fixed key-frame mode probabilities on key frames (145,156,163,128 / 142,114,183), not the inter defaults. `parse_header` writes them into `Vp8Hdr.e`; hantro ignores them on key frames, so both paths share it. With the inter defaults the engine loses sync at the first 4x4 block with a non-DC neighbor.
- The A133P does have VP8: all clips bit-exact on the TrimUI Smart Pro, although its product brief does not list the codec.
- Bit-exact 2026-11-08 (10 clips, profiles 0-3, alt-ref, token partitions, segments, odd size, 720p, 1080p): RG Cube XX, TrimUI Smart Pro, RG351P (hantro), RG552 (MPP); R36S 2GB (MPP) up to 720p. 1080p decode without copy-out: H700 113 fps, A133P 106, RK3326 hantro 71, RK3399 85; libvpx NEON on the H700 is about 32 fps.
