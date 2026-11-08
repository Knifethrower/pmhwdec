/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * h264_pmhw / hevc_pmhw: hardware decoders through libpmhwdec (Allwinner video engine, Rockchip MPP, V4L2 stateless).
 * libpmhwdec.so is loaded at run time; without it, without hardware for the stream, or for streams the chip cannot
 * do (10-bit on 8-bit chips), init fails and the player falls back to the software decoder. Output is NV12.
 * A stream the container did not describe fully (mpv opens decoders without the profile) and that turns out too much
 * for the chip once its SPS arrives is handed to FFmpeg's own software decoder inside this one.
 * Drop into libavcodec/ with pmhwdec.h and register in allcodecs.c + Makefile (see README.md next to this file).
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "pmhwdec.h"

typedef struct PmhwApi {
    int (*query)(PmhwCodec, PmhwCaps *);
    PmhwDecoder *(*open)(PmhwCodec, int, int);
    int (*send)(PmhwDecoder *, const uint8_t *, int, int64_t);
    int (*get)(PmhwDecoder *, PmhwPicture *);
    void (*put)(PmhwDecoder *, PmhwPicture *);
    void (*flush)(PmhwDecoder *);
    void (*close)(PmhwDecoder *);
    const char *(*backend_name)(const PmhwDecoder *);
} PmhwApi;

typedef struct PmhwContext {
    void *lib;
    PmhwApi api;
    PmhwDecoder *dec;
    AVPacket *pkt;
    int have_pkt;       /* pkt holds an access unit the decoder asked to get again (PMHW_AGAIN) */
    int eos_sent;
    AVCodecContext *sw; /* software decoder after the hardware refused the stream mid-way */
} PmhwContext;

static av_cold int pmhw_ff_close(AVCodecContext *avctx)
{
    PmhwContext *s = avctx->priv_data;
    avcodec_free_context(&s->sw);
    if (s->dec)
        s->api.close(s->dec);
    s->dec = NULL;
    av_packet_free(&s->pkt);
    if (s->lib)
        dlclose(s->lib);
    s->lib = NULL;
    return 0;
}

static int load_api(AVCodecContext *avctx, PmhwContext *s)
{
    s->lib = dlopen("libpmhwdec.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!s->lib)
        s->lib = dlopen("libpmhwdec.so", RTLD_NOW | RTLD_LOCAL);
    if (!s->lib) {
        av_log(avctx, AV_LOG_VERBOSE, "pmhw: %s\n", dlerror());
        return AVERROR(ENOSYS);
    }
#define SYM(f, n) if (!(*(void **)&s->api.f = dlsym(s->lib, n))) return AVERROR(ENOSYS);
    SYM(query, "pmhw_query") SYM(open, "pmhw_open") SYM(send, "pmhw_send") SYM(get, "pmhw_get")
    SYM(put, "pmhw_put") SYM(flush, "pmhw_flush") SYM(close, "pmhw_close") SYM(backend_name, "pmhw_backend_name")
#undef SYM
    return 0;
}

/* what the container says about the stream, against what the chip can do: refuse here so the player can still
 * pick the software decoder (later, mid-stream, it could not) */
static int stream_fits(AVCodecContext *avctx, const PmhwCaps *caps)
{
    int depth = avctx->bits_per_raw_sample > 0 ? avctx->bits_per_raw_sample : 8;
    if (avctx->codec_id == AV_CODEC_ID_HEVC && (avctx->profile == AV_PROFILE_HEVC_MAIN_10 || avctx->profile == AV_PROFILE_HEVC_REXT))
        depth = depth > 10 ? depth : 10;
    if (avctx->codec_id == AV_CODEC_ID_H264 && (avctx->profile == AV_PROFILE_H264_HIGH_10 ||
        avctx->profile == AV_PROFILE_H264_HIGH_10_INTRA || avctx->profile >= AV_PROFILE_H264_HIGH_422))
        return 0;                                     /* 10-bit / 4:2:2 / 4:4:4 H.264: no handheld decodes these */
    if (avctx->pix_fmt != AV_PIX_FMT_NONE && avctx->pix_fmt != AV_PIX_FMT_YUV420P && avctx->pix_fmt != AV_PIX_FMT_YUVJ420P &&
        avctx->pix_fmt != AV_PIX_FMT_YUV420P10 && avctx->pix_fmt != AV_PIX_FMT_NV12)
        return 0;
    if (depth > caps->max_bit_depth)
        return 0;
    if (avctx->width > caps->max_width || avctx->height > caps->max_height)
        return 0;
    return 1;
}

static av_cold int pmhw_ff_init(AVCodecContext *avctx)
{
    PmhwContext *s = avctx->priv_data;
    PmhwCodec codec = avctx->codec_id == AV_CODEC_ID_HEVC ? PMHW_CODEC_HEVC : PMHW_CODEC_H264;
    PmhwCaps caps;
    const char *env = getenv("PM_HWDEC");
    int ret;

    if (env && !strcmp(env, "0"))
        return AVERROR(ENOSYS);
    if ((ret = load_api(avctx, s)) < 0)
        return ret;
    if (s->api.query(codec, &caps)) {
        av_log(avctx, AV_LOG_VERBOSE, "pmhw: no hardware decoder for this codec\n");
        return AVERROR(ENOSYS);
    }
    if (!stream_fits(avctx, &caps)) {
        av_log(avctx, AV_LOG_INFO, "pmhw: %s can't decode this stream (profile %d, %dx%d, %d-bit max): software\n",
               caps.backend, avctx->profile, avctx->width, avctx->height, caps.max_bit_depth);
        return AVERROR(ENOSYS);
    }
    s->dec = s->api.open(codec, avctx->width, avctx->height);
    if (!s->dec)
        return AVERROR(ENOSYS);
    s->pkt = av_packet_alloc();
    if (!s->pkt)
        return AVERROR(ENOMEM);
    avctx->pix_fmt = AV_PIX_FMT_NV12;
    av_log(avctx, AV_LOG_INFO, "pmhw: hardware decoding (%s)\n", s->api.backend_name(s->dec));
    return 0;
}

static int output(AVCodecContext *avctx, AVFrame *frame, PmhwPicture *pic)
{
    PmhwContext *s = avctx->priv_data;
    int ret;

    if (avctx->width != pic->width || avctx->height != pic->height) {
        ret = ff_set_dimensions(avctx, pic->width, pic->height);
        if (ret < 0)
            goto out;
    }
    frame->width = pic->width;
    frame->height = pic->height;
    frame->format = AV_PIX_FMT_NV12;
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        goto out;
    av_image_copy_plane(frame->data[0], frame->linesize[0], pic->plane[0] + pic->top * pic->stride[0] + pic->left,
                        pic->stride[0], pic->width, pic->height);
    av_image_copy_plane(frame->data[1], frame->linesize[1], pic->plane[1] + pic->top / 2 * pic->stride[1] + (pic->left & ~1),
                        pic->stride[1], (pic->width + 1) & ~1, (pic->height + 1) / 2);
    frame->pts = pic->pts;
    frame->pkt_dts = AV_NOPTS_VALUE;
    if (pic->corrupt)
        frame->flags |= AV_FRAME_FLAG_CORRUPT;
out:
    s->api.put(s->dec, pic);
    return ret;
}

/* the hardware refused the stream: FFmpeg's software decoder takes over from the packet in s->pkt (Annex B, with the
 * parameter sets in band: the bitstream filter already ran, so no extradata) */
static int start_software(AVCodecContext *avctx, PmhwContext *s)
{
    const AVCodec *c = avcodec_find_decoder_by_name(avctx->codec_id == AV_CODEC_ID_HEVC ? "hevc" : "h264");
    int ret;
    if (!c || !(s->sw = avcodec_alloc_context3(c)))
        return AVERROR(ENOSYS);
    s->sw->width = avctx->width;
    s->sw->height = avctx->height;
    s->sw->thread_count = 0;                          /* auto: avctx's own count was cut to 1 (no thread caps here) */
    s->sw->flags = avctx->flags;
    s->sw->flags2 = avctx->flags2;
    s->sw->skip_loop_filter = avctx->skip_loop_filter;
    s->sw->skip_frame = avctx->skip_frame;
    s->sw->pkt_timebase = avctx->pkt_timebase;
    if ((ret = avcodec_open2(s->sw, c, NULL)) < 0)
        return ret;
    s->api.close(s->dec);
    s->dec = NULL;
    av_log(avctx, AV_LOG_INFO, "pmhw: the chip can't decode this stream: software decoding\n");
    return 0;
}

static int software_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    PmhwContext *s = avctx->priv_data;
    int ret;
    for (;;) {
        ret = avcodec_receive_frame(s->sw, frame);
        if (ret >= 0) {
            avctx->pix_fmt = frame->format;
            if (avctx->width != frame->width || avctx->height != frame->height)
                ff_set_dimensions(avctx, frame->width, frame->height);
            return 0;
        }
        if (ret != AVERROR(EAGAIN))
            return ret;
        if (!s->have_pkt) {
            ret = ff_decode_get_packet(avctx, s->pkt);
            if (ret == AVERROR_EOF) {
                if (!s->eos_sent)
                    avcodec_send_packet(s->sw, NULL);
                s->eos_sent = 1;
                continue;
            }
            if (ret < 0)
                return ret;                           /* EAGAIN: the player sends more */
            s->have_pkt = 1;
        }
        ret = avcodec_send_packet(s->sw, s->pkt);
        if (ret == AVERROR(EAGAIN))
            continue;                                 /* not taken: receive first, then the same packet again */
        av_packet_unref(s->pkt);
        s->have_pkt = 0;
        if (ret < 0 && ret != AVERROR_INVALIDDATA)
            return ret;
    }
}

static int pmhw_ff_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    PmhwContext *s = avctx->priv_data;
    PmhwPicture pic;
    int ret;

    if (s->sw)
        return software_receive_frame(avctx, frame);
    for (int spins = 0; spins < 10000; spins++) {
        if (s->api.get(s->dec, &pic) == 1)
            return output(avctx, frame, &pic);
        if (s->eos_sent)
            return AVERROR_EOF;
        if (!s->have_pkt) {
            ret = ff_decode_get_packet(avctx, s->pkt);
            if (ret == AVERROR_EOF) {
                s->api.send(s->dec, NULL, 0, 0);
                s->eos_sent = 1;
                continue;
            }
            if (ret < 0)
                return ret;                           /* EAGAIN: the player sends more */
            s->have_pkt = 1;
        }
        ret = s->api.send(s->dec, s->pkt->data, s->pkt->size, s->pkt->pts);
        if (ret == PMHW_AGAIN)
            continue;                                 /* get() frees a buffer, then the same packet again */
        if (ret == PMHW_UNSUPPORTED) {                /* e.g. 10-bit H.264: the SPS says so, the container did not */
            if ((ret = start_software(avctx, s)) < 0) {
                av_log(avctx, AV_LOG_ERROR, "pmhw: unsupported by this chip, no software decoder\n");
                return ret;
            }
            return software_receive_frame(avctx, frame);   /* s->pkt (have_pkt still set) goes to it first */
        }
        av_packet_unref(s->pkt);
        s->have_pkt = 0;
        if (ret == PMHW_ERROR)
            av_log(avctx, AV_LOG_WARNING, "pmhw: access unit dropped\n");
    }
    av_log(avctx, AV_LOG_ERROR, "pmhw: decoder makes no progress\n");
    return AVERROR_EXTERNAL;
}

static av_cold void pmhw_ff_flush(AVCodecContext *avctx)
{
    PmhwContext *s = avctx->priv_data;
    if (s->sw)
        avcodec_flush_buffers(s->sw);
    else
        s->api.flush(s->dec);
    av_packet_unref(s->pkt);
    s->have_pkt = 0;
    s->eos_sent = 0;
}

#define PMHW_DECODER(short_name, long_name, codec_id, bsf_name)                        \
const FFCodec ff_##short_name##_pmhw_decoder = {                                 \
    .p.name         = #short_name "_pmhw",                                       \
    CODEC_LONG_NAME(long_name " (pmhwdec hardware)"),                            \
    .p.type         = AVMEDIA_TYPE_VIDEO,                                        \
    .p.id           = codec_id,                                                  \
    .priv_data_size = sizeof(PmhwContext),                                       \
    .init           = pmhw_ff_init,                                              \
    FF_CODEC_RECEIVE_FRAME_CB(pmhw_ff_receive_frame),                            \
    .flush          = pmhw_ff_flush,                                             \
    .close          = pmhw_ff_close,                                             \
    .bsfs           = bsf_name,                                                  \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP | \
                      FF_CODEC_CAP_SETS_FRAME_PROPS,                             \
    .p.pix_fmts     = (const enum AVPixelFormat[]){ AV_PIX_FMT_NV12, AV_PIX_FMT_NONE }, \
    .p.wrapper_name = "pmhw",                                                    \
};

PMHW_DECODER(h264, "H.264", AV_CODEC_ID_H264, "h264_mp4toannexb")
PMHW_DECODER(hevc, "HEVC", AV_CODEC_ID_HEVC, "hevc_mp4toannexb")
