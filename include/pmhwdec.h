/* SPDX-License-Identifier: MIT */
/* The public API header is MIT so any player, FFmpeg build or port can include it; the library is LGPL-2.1-or-later
 * (the Allwinner decoder runs in a separate GPL helper program, pmhw-cedard). */
/* pmhwdec: hardware video decoding for PortMaster handhelds.
 *
 * Access units in (Annex-B byte stream, one picture per call), pictures out in display order. Backends are picked
 * at run time: Allwinner video engine on vendor kernels (blob-free, "cedar"), later Rockchip MPP ("rkmpp") and
 * mainline V4L2 stateless decoders ("v4l2req"). No vendor headers are needed to use this API.
 *
 * Typical loop:
 *     PmhwDecoder *d = pmhw_open(PMHW_CODEC_HEVC, w, h);   // NULL: no hardware, use software
 *     for each access unit:
 *         while ((r = pmhw_send(d, data, size, pts)) == PMHW_AGAIN)   // all buffers out: return some first
 *             drain();
 *         if (r == PMHW_UNSUPPORTED) -> close it, decode this stream in software
 *         drain();
 *     pmhw_send(d, NULL, 0, 0);                              // end of stream: release the reorder queue
 *     drain();
 *   drain(): while (pmhw_get(d, &pic) == 1) { use pic; pmhw_put(d, &pic); }
 * Seeking: pmhw_flush(d), then send from a key frame (pictures before the first key frame are dropped).
 */
#ifndef PMHWDEC_H
#define PMHWDEC_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PMHWDEC_VERSION 1

typedef enum { PMHW_CODEC_H264 = 0, PMHW_CODEC_HEVC = 1, PMHW_CODEC_VP9 = 2, PMHW_CODEC_COUNT } PmhwCodec;

/* what the best backend on this device can do for one codec */
typedef struct PmhwCaps {
    int max_width, max_height;
    int max_bit_depth;           /* 8, or 10 when 10-bit streams decode (output is still 8 bit, see PmhwPicture) */
    const char *backend;         /* "cedar", "rkmpp", "v4l2req" */
} PmhwCaps;

/* return values */
enum {
    PMHW_OK = 0,
    PMHW_AGAIN = 1,              /* pmhw_send: no free picture buffer: get/put pictures, then send the same data again */
    PMHW_ERROR = -1,             /* this access unit was dropped; the decoder goes on with the next one */
    PMHW_UNSUPPORTED = -2,       /* this stream can't be decoded here (e.g. 10-bit on an 8-bit chip, interlaced,
                                  * scaling lists): switch to software decoding for it */
};

typedef enum { PMHW_FMT_NV12 = 1 } PmhwFormat;

typedef struct PmhwPicture {
    int width, height;           /* visible size */
    int left, top;               /* visible area's offset inside the planes */
    PmhwFormat format;           /* NV12, 8 bit (10-bit streams: the top 8 bits of each sample) */
    const uint8_t *plane[2];     /* Y, interleaved UV; CPU-readable (may be uncached: copy whole rows) */
    int stride[2];
    int64_t pts;                 /* the pts given to pmhw_send for this picture's access unit */
    int corrupt;                 /* decoded with errors or missing references */
    /* zero-copy: dmabuf holding the whole picture, or -1. Valid until pmhw_put(); do not close it. */
    int dmabuf_fd;
    uint32_t drm_fourcc;         /* DRM_FORMAT_NV12 */
    uint64_t drm_modifier;       /* 0 = linear */
    uint32_t offset[2];          /* plane offsets inside dmabuf_fd */
    void *priv;
} PmhwPicture;

typedef struct PmhwDecoder PmhwDecoder;

/* 0 and caps filled = usable; -1 = no hardware for this codec here (use software).
 * Environment: PMHWDEC_BACKEND=<name> forces one, PMHWDEC_BACKEND=off disables all, PMHWDEC_LOG=1 logs to stderr. */
int pmhw_query(PmhwCodec codec, PmhwCaps *caps);
/* width/height from the container (0 if unknown): only used to refuse too big streams early. NULL = no hardware. */
PmhwDecoder *pmhw_open(PmhwCodec codec, int width, int height);
/* one access unit (Annex-B, start codes included, parameter sets may come along); NULL/0 = end of stream */
int pmhw_send(PmhwDecoder *dec, const uint8_t *data, int size, int64_t pts);
/* 1 = picture returned (hand it back with pmhw_put), 0 = none ready */
int pmhw_get(PmhwDecoder *dec, PmhwPicture *pic);
void pmhw_put(PmhwDecoder *dec, PmhwPicture *pic);
/* drop everything queued (seek); pictures the caller still holds stay valid until pmhw_put */
void pmhw_flush(PmhwDecoder *dec);
void pmhw_close(PmhwDecoder *dec);
const char *pmhw_backend_name(const PmhwDecoder *dec);

#ifdef __cplusplus
}
#endif
#endif
