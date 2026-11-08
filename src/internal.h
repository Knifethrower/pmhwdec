/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* pmhwdec internals: backend interface, the stateless decoder core, shared helpers. */
#ifndef PMHW_INTERNAL_H
#define PMHW_INTERNAL_H
#include <stdint.h>
#include <stdio.h>
#include "pmhwdec.h"

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define DIV_ROUND_UP(x, d) (((x) + (d) - 1) / (d))
#ifndef BIT
#define BIT(n) (1u << (n))
#endif

extern int pmhw_log_on;
#define LOG(...) do { if (pmhw_log_on) { fprintf(stderr, "pmhwdec: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define ERR(...) do { fprintf(stderr, "pmhwdec: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---- a backend, as the public API sees it ---- */
typedef struct Backend {
    const char *name;
    int (*query)(PmhwCodec codec, PmhwCaps *caps);
    void *(*open)(PmhwCodec codec, int width, int height);
    int (*send)(void *ctx, const uint8_t *data, int size, int64_t pts);
    int (*get)(void *ctx, PmhwPicture *pic);
    void (*put)(void *ctx, PmhwPicture *pic);
    void (*flush)(void *ctx);
    void (*close)(void *ctx);
} Backend;

/* ---- stateless decoding: shared parser/DPB core driving hardware ops (cedar now, v4l2req later) ---- */
#define MAX_SLOTS 18             /* cedrus H.264 frame list size; HEVC uses at most 16 */
#define SL_EXTRA_SLOTS 3         /* pictures the caller may hold at once without stalling the decoder */

typedef struct Slot {
    int ref;                     /* short-term reference */
    int poc, frame_num, pic_num;
    int epoch;                   /* bumps at every IDR / IRAP that starts a coded video sequence */
    int queued, held, corrupt, used;
    int64_t pts;
} Slot;

typedef struct Geom {
    PmhwCodec codec;
    int width, height;           /* coded size in luma samples (H.264: MB aligned) */
    int stride, coded_h;         /* NV12 layout: ALIGN16 */
    int vis_x, vis_y, vis_w, vis_h;
    int bit_depth;
    int h264_direct_8x8, h264_frame_mbs_only;
    int hevc_ctb_log2;
    int nslots;
} Geom;

struct H264Job;
struct HevcJob;

typedef struct HwOps {
    const char *name;
    int (*query)(PmhwCodec codec, PmhwCaps *caps);
    void *(*open)(PmhwCodec codec);
    int (*configure)(void *hw, const Geom *g);        /* (re)allocates g->nslots picture buffers */
    int (*h264_slice)(void *hw, const struct H264Job *j);   /* 0 ok, 1 decoded with errors, -1 failed */
    int (*hevc_slice)(void *hw, const struct HevcJob *j);
    void (*export_pic)(void *hw, int slot, PmhwPicture *pic);
    void (*close)(void *hw);
    /* optional: frame-based hardware decodes here, after all slices of the picture went through *_slice();
     * called before reference marking. 0 ok, 1 decoded with errors, -1 failed */
    int (*end_picture)(void *hw, int slot);
} HwOps;

typedef struct SL {
    PmhwCodec codec;
    const HwOps *hw;
    void *hwctx;
    Slot slot[MAX_SLOTS];
    int nslots, configured;
    Geom geom;
    int reorder;                 /* pictures the output queue holds back */
    int epoch, eos, bump, need_key;
    int cur;                     /* slot of the picture being decoded, -1 = none */
    int cur_corrupt;
    void *codec_state;
    uint8_t *rbsp;
    int rbsp_size;
    int *epb, nepb;              /* raw positions of removed emulation prevention bytes in the last unescape */
} SL;

int sl_free_slot(SL *s);
/* PMHW_AGAIN while old pictures are still queued/held; then reallocates */
int sl_configure(SL *s, const Geom *g, int reorder);
void sl_queue(SL *s, int slot);
int sl_unescape(SL *s, const uint8_t *src, int n);

/* codec layers (h264.c, hevc.c) */
void *h264_new(void);
int h264_nal(SL *s, const uint8_t *nal, int len, int64_t pts);
void h264_end_picture(SL *s);
void h264_reset(SL *s);
void h264_free(void *st);
void *hevc_new(void);
int hevc_nal(SL *s, const uint8_t *nal, int len, int64_t pts);
void hevc_end_picture(SL *s);
void hevc_reset(SL *s);
void hevc_free(void *st);

/* backends */
extern const HwOps pmhw_cedar_ops;               /* pmhw-cedard only (GPL): the library uses the client */
extern const Backend pmhw_cedar_client_backend;
extern const HwOps pmhw_v4l2req_ops;
extern const Backend pmhw_rkmpp_backend;
extern const Backend pmhw_v4l2m2m_backend;
const Backend *pmhw_stateless_backend(const HwOps *ops);

/* self-test (selftest.c): the bit depth a backend passed for a codec (0 = not usable), capped at max_depth */
int pmhw_selftest(const Backend *be, PmhwCodec codec, int max_depth);
/* for backends that see a stream's depth only in its headers: 0 = its 10-bit test failed, refuse the stream */
int pmhw_selftest_depth_allowed(const char *backend, PmhwCodec codec, int depth);

#endif
