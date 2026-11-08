# Idea: AV1 decoding split between the CPU and the Mali GPU (non-reference frames)

Status: idea only, nothing built or measured (noted 2026-10-30). Every number below is an estimate.

## Why

The H700 and Rockchip video decoders have no AV1. dav1d on the CPU (4x Cortex-A53 about 1.5 GHz on the H700)
probably runs 1080p at about 20 fps (guess, not measured), short of 30. AV1 reaches the handheld unconverted only for
downloads, or when the server's conversion is off; streams are converted by the server by default (Jellyfin port).

## The idea

Each AV1 frame is predicted from earlier finished frames, including their loop filtering (deblocking, CDEF, loop
restoration). So moving the loop filters to the GPU normally means a round trip per frame: the filtered frame has to
come back to CPU memory before the next frame can be decoded. A full 1080p frame upload alone costs about 3-10 ms on
the RG Cube XX (texImage2D, 12-27 ms on the R36S), which eats the gain.

**Non-reference frames break that chain.** In typical libaom / SVT-AV1 encodes, up to about half the frames (the top
layer of the hierarchy) are never used as a reference. For those frames, nothing later reads the filtered output, so:

1. The CPU (dav1d) does entropy decoding, prediction and reconstruction as usual.
2. The CPU skips the loop filters on non-reference frames only.
3. The unfiltered frame plus the filter parameters (deblock levels, CDEF strengths and directions, LR units) are
   uploaded to the GPU once. It has to be uploaded for display anyway.
4. GLES compute or fragment shaders apply the filters there, and the result goes straight to the display.
   Nothing comes back to the CPU.

Reference frames keep the full CPU path, so prediction stays bit-exact. Only the displayed picture of non-reference
frames comes from the GPU filters. They should match dav1d bit for bit if the shaders are exact; that can be checked
against dav1d's output.

Film grain is a smaller version of the same thing: it is applied after decoding and nothing depends on it. mpv can
already draw it on the GPU through libplacebo (`vo=gpu-next` only; the port uses `vo=gpu`).

## Expected gain

Maybe about 20% (estimate). That depends on:
- the share of non-reference frames in real files (encoder and settings);
- the share of loop-filter time in dav1d on the A53 (CDEF + LR + deblock, guessed at 30-40% of frame time);
- what the Mali-G31 (2 cores, GLES 3.2) does with those shaders at 1080p alongside mpv's own rendering.

## What it would take

- dav1d hooks: a per-frame flag to skip the in-loop filters for non-reference frames, and a way to export the filter
  parameters per superblock. dav1d has `inloop_filters` flags, but those apply to every frame, references included.
- GLES shaders for AV1 deblocking, CDEF and loop restoration (Wiener + self-guided), 8-bit and 10-bit.
- mpv integration: a hardware-frame type carrying the unfiltered planes plus parameters, filtered in the renderer
  before scaling and color conversion. Or a standalone path outside mpv to measure first.
- Ordering: frames reach the display in output order. Reference frames come out of dav1d already filtered;
  non-reference frames carry the filter work with them.

## First steps to judge it (cheap)

1. Time dav1d on the RG Cube XX with a 1080p clip: full decode, no loop filters (`inloop_filters` off), and
   reference frames only (`decode_frame_type`). That gives the ceiling for this idea.
2. Count non-reference frames in a few real AV1 files (ffprobe / dav1d frame headers).
3. Write one filter (CDEF is the biggest) as a GLES shader and time it at 1080p on the Mali-G31.

If 1 shows less than about 15% headroom, drop it.

## Related ideas (same conversation)

- Download converted copies from the Jellyfin server at screen size. This fixes AV1 downloads outright and is the
  practical answer.
- Decode only reference frames with dav1d (`decode_frame_type`), at about 15 real fps.
- Convert downloads on the device with the chip's hardware H.264 encoder (MPP on Rockchip; no open encoder on Allwinner).
