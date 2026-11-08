// SPDX-License-Identifier: LGPL-2.1-or-later
/* Stateless decoding core: frame store, output (reorder) queue, NAL loop. Codec layers (h264.c, hevc.c) parse and
 * manage references; HwOps backends (cedar.c, later v4l2req) program the hardware for one slice at a time. */
#include <stdlib.h>
#include <string.h>
#include "internal.h"

int sl_free_slot(SL *s)
{
    for (int i = 0; i < s->nslots; i++) {
        Slot *t = &s->slot[i];
        if (!t->ref && !t->queued && !t->held && i != s->cur) return i;
    }
    return -1;
}

int sl_configure(SL *s, const Geom *g, int reorder)
{
    Geom want = *g;
    if (s->configured && !memcmp(&want, &s->geom, sizeof want)) { s->reorder = reorder; return PMHW_OK; }
    PmhwCaps caps;
    if (s->hw->query(s->codec, &caps) || want.bit_depth > caps.max_bit_depth || want.width > caps.max_width ||
        want.height > caps.max_height || !pmhw_selftest_depth_allowed(s->hw->name, s->codec, want.bit_depth)) {
        ERR("%s: %dx%d %d-bit is beyond this chip (%dx%d, %d-bit)", s->hw->name, want.width, want.height, want.bit_depth,
            caps.max_width, caps.max_height, caps.max_bit_depth);
        return PMHW_UNSUPPORTED;
    }
    for (int i = 0; i < s->nslots; i++)
        if (s->slot[i].queued || s->slot[i].held) { s->bump = 1; s->eos = 1; return PMHW_AGAIN; }   /* drain first */
    s->eos = 0;
    if (s->hw->configure(s->hwctx, &want)) { s->configured = 0; return PMHW_ERROR; }
    memset(s->slot, 0, sizeof s->slot);
    s->nslots = want.nslots;
    s->geom = want;
    s->configured = 1;
    s->reorder = reorder;
    s->cur = -1;
    LOG("%s: %dx%d (visible %dx%d at %d,%d), %d-bit, %d buffers, reorder %d", s->hw->name, want.width, want.height,
        want.vis_w, want.vis_h, want.vis_x, want.vis_y, want.bit_depth, want.nslots, reorder);
    return PMHW_OK;
}

void sl_queue(SL *s, int slot)
{
    s->slot[slot].queued = 1;
    s->slot[slot].epoch = s->epoch;
}

int sl_unescape(SL *s, const uint8_t *src, int n)
{
    if (n > s->rbsp_size) {
        free(s->rbsp); free(s->epb);
        s->rbsp_size = n + 4096;
        s->rbsp = malloc(s->rbsp_size);
        s->epb = malloc(sizeof(int) * (s->rbsp_size / 3 + 1));
        if (!s->rbsp || !s->epb) { s->rbsp_size = 0; return -1; }
    }
    int o = 0, zeros = 0;
    s->nepb = 0;
    for (int i = 0; i < n; i++) {
        if (zeros >= 2 && src[i] == 3) { zeros = 0; s->epb[s->nepb++] = i; continue; }
        zeros = src[i] == 0 ? zeros + 1 : 0;
        s->rbsp[o++] = src[i];
    }
    return o;
}

/* ---------------- Backend implementation ---------------- */
typedef struct { Backend be; const HwOps *ops; } StatelessBackend;

static void *sl_open(const HwOps *ops, PmhwCodec codec, int width, int height)
{
    PmhwCaps caps;
    if (ops->query(codec, &caps)) return NULL;
    if (width > caps.max_width || height > caps.max_height) { LOG("%s: %dx%d is too big", ops->name, width, height); return NULL; }
    SL *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->codec = codec;
    s->hw = ops;
    s->cur = -1;
    s->need_key = 1;
    s->hwctx = ops->open(codec);
    if (!s->hwctx) { free(s); return NULL; }
    s->codec_state = codec == PMHW_CODEC_H264 ? h264_new() : codec == PMHW_CODEC_HEVC ? hevc_new() :
                     codec == PMHW_CODEC_VP8 ? vp8_new() : NULL;
    if (!s->codec_state) { ops->close(s->hwctx); free(s); return NULL; }
    return s;
}

static void end_picture(SL *s)
{
    if (s->cur < 0) return;
    if (s->codec == PMHW_CODEC_H264) h264_end_picture(s); else hevc_end_picture(s);
}

static int sl_send(void *ctx, const uint8_t *data, int size, int64_t pts)
{
    SL *s = ctx;
    if (!data || size <= 0) { end_picture(s); s->eos = 1; return PMHW_OK; }
    if (s->eos) s->eos = 0;
    if (s->codec == PMHW_CODEC_VP8) return vp8_frame(s, data, size, pts);   /* no NAL units: one frame per call */
    int rc = PMHW_OK;
    for (int i = 0; i + 3 <= size;) {
        if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) { i++; continue; }
        int st = i + 3, e = st;
        while (e + 3 <= size && !(data[e] == 0 && data[e + 1] == 0 && (data[e + 2] == 1 || (e + 3 < size && data[e + 2] == 0 && data[e + 3] == 1)))) e++;
        if (e + 3 > size) e = size;
        i = e;
        int r = s->codec == PMHW_CODEC_H264 ? h264_nal(s, data + st, e - st, pts) : hevc_nal(s, data + st, e - st, pts);
        if (r == PMHW_AGAIN) return PMHW_AGAIN;    /* nothing of this picture was decoded yet: caller resends */
        if (r == PMHW_UNSUPPORTED) return r;
        if (r == PMHW_ERROR) rc = PMHW_ERROR;
    }
    end_picture(s);
    return rc;
}

static int sl_get(void *ctx, PmhwPicture *pic)
{
    SL *s = ctx;
    int best = -1, n = 0, min_epoch = 1 << 30;
    for (int i = 0; i < s->nslots; i++) {
        Slot *t = &s->slot[i];
        if (!t->queued) continue;
        n++;
        if (t->epoch < min_epoch) min_epoch = t->epoch;
        if (best < 0 || t->epoch < s->slot[best].epoch || (t->epoch == s->slot[best].epoch && t->poc < s->slot[best].poc)) best = i;
    }
    if (best < 0) { s->bump = 0; return 0; }
    /* a picture leaves the queue when the queue is full, a new sequence has begun, at end of stream, or to free a buffer */
    if (!(n > s->reorder || min_epoch < s->epoch || s->eos || s->bump)) return 0;
    s->bump = 0;
    Slot *t = &s->slot[best];
    t->queued = 0;
    t->held = 1;
    memset(pic, 0, sizeof *pic);
    s->hw->export_pic(s->hwctx, best, pic);
    pic->width = s->geom.vis_w; pic->height = s->geom.vis_h;
    pic->left = s->geom.vis_x; pic->top = s->geom.vis_y;
    pic->pts = t->pts;
    pic->corrupt = t->corrupt;
    pic->priv = (void *)(intptr_t)(best + 1);
    return 1;
}

static void sl_put(void *ctx, PmhwPicture *pic)
{
    SL *s = ctx;
    int i = (int)(intptr_t)pic->priv - 1;
    if (i >= 0 && i < s->nslots) s->slot[i].held = 0;
    pic->priv = NULL;
}

static void sl_flush(void *ctx)
{
    SL *s = ctx;
    for (int i = 0; i < s->nslots; i++) { s->slot[i].queued = 0; s->slot[i].ref = 0; }
    s->cur = -1;
    s->eos = s->bump = 0;
    s->need_key = 1;
    if (s->codec == PMHW_CODEC_H264) h264_reset(s); else if (s->codec == PMHW_CODEC_VP8) vp8_reset(s); else hevc_reset(s);
}

static void sl_close(void *ctx)
{
    SL *s = ctx;
    if (!s) return;
    if (s->codec == PMHW_CODEC_H264) h264_free(s->codec_state); else if (s->codec == PMHW_CODEC_VP8) vp8_free(s->codec_state); else hevc_free(s->codec_state);
    s->hw->close(s->hwctx);
    free(s->rbsp); free(s->epb);
    free(s);
}

/* one Backend per HwOps; the ops pointer travels through a small trampoline table */
#define MAX_STATELESS 4
static StatelessBackend tab[MAX_STATELESS];
static int ntab;

#define TRAMP(n) \
    static int q##n(PmhwCodec c, PmhwCaps *caps) { return tab[n].ops->query(c, caps); } \
    static void *o##n(PmhwCodec c, int w, int h) { return sl_open(tab[n].ops, c, w, h); }
TRAMP(0) TRAMP(1) TRAMP(2) TRAMP(3)
static int (*const qs[MAX_STATELESS])(PmhwCodec, PmhwCaps *) = { q0, q1, q2, q3 };
static void *(*const os[MAX_STATELESS])(PmhwCodec, int, int) = { o0, o1, o2, o3 };

const Backend *pmhw_stateless_backend(const HwOps *ops)
{
    for (int i = 0; i < ntab; i++) if (tab[i].ops == ops) return &tab[i].be;
    if (ntab == MAX_STATELESS) return NULL;
    StatelessBackend *b = &tab[ntab];
    b->ops = ops;
    b->be = (Backend){ ops->name, qs[ntab], os[ntab], sl_send, sl_get, sl_put, sl_flush, sl_close };
    ntab++;
    return &b->be;
}
