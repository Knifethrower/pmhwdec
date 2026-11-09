// SPDX-License-Identifier: LGPL-2.1-or-later
/* VP8 for the stateless backends: parses the frame header (RFC 6386, sections 9 and 19.2), keeps the state that
 * lasts from frame to frame (probabilities, segment and loop filter settings, the three reference frames) and hands
 * one job per frame to the hardware. VP8 has no reordering: pictures come out in decoding order, hidden (alt-ref)
 * frames are decoded and not shown. Input: one VP8 frame per pmhwdec_send(), as stored in WebM or IVF. */
#include <stdlib.h>
#include <string.h>
#include "codec.h"
#include "vp8_tables.h"

/* ---------------- boolean decoder ---------------- */
/* The value being compared sits at bit 16 of code; bits counts up to 0, when two more bytes are needed. Written so
 * that the state at the end of the header is what the V4L2 drivers ask for (coder_state, first_part_header_bits). */
typedef struct Bool { const uint8_t *p, *end; unsigned code; int bits, high; } Bool;

static unsigned next_byte(Bool *b, int at) { return b->p + at < b->end ? b->p[at] : 0; }

static void bool_init(Bool *b, const uint8_t *p, int n)
{
    b->p = p;
    b->end = p + n;
    b->code = next_byte(b, 0) << 16 | next_byte(b, 1) << 8 | next_byte(b, 2);
    b->p += 3;
    b->bits = -16;
    b->high = 255;
}

static unsigned renorm(Bool *b)
{
    int shift = __builtin_clz((unsigned)b->high) - 24;
    unsigned code = b->code << shift;
    b->high <<= shift;
    b->bits += shift;
    if (b->bits >= 0 && b->p < b->end) {
        code |= (next_byte(b, 0) << 8 | next_byte(b, 1)) << b->bits;
        b->p += 2;
        b->bits -= 16;
    }
    return code;
}

static int rb(Bool *b, int prob)
{
    unsigned code = renorm(b), low = 1 + (((b->high - 1) * prob) >> 8), low16 = low << 16;
    int bit = code >= low16;
    b->high = bit ? b->high - (int)low : (int)low;
    b->code = bit ? code - low16 : code;
    return bit;
}

static int lit(Bool *b, int n) { int v = 0; while (n--) v = v << 1 | rb(b, 128); return v; }
static int flag(Bool *b) { return rb(b, 128); }
static int signed_lit(Bool *b, int n) { int v = lit(b, n); return flag(b) ? -v : v; }
static int opt_signed(Bool *b, int n) { return flag(b) ? signed_lit(b, n) : 0; }

/* ---------------- state between frames ---------------- */
typedef struct Vp8State {
    Vp8Entropy e, saved;
    int seg_abs;
    int8_t seg_q[4], seg_lf[4], ref_delta[4], mode_delta[4];
    uint8_t seg_probs[3];
    int last, golden, alt;      /* slots, -1 = none */
    int count;                  /* display order of the frames shown */
} Vp8State;

void vp8_reset(SL *s)
{
    Vp8State *v = s->codec_state;
    v->last = v->golden = v->alt = -1;
}

void *vp8_new(void)
{
    Vp8State *v = calloc(1, sizeof *v);
    if (v) v->last = v->golden = v->alt = -1;
    return v;
}

void vp8_free(void *st) { free(st); }

static void key_frame_defaults(Vp8State *v)
{
    static const uint8_t ymode[4] = { 112, 86, 140, 37 }, uvmode[3] = { 162, 101, 204 };
    memcpy(v->e.coeff, vp8_default_coef_probs, sizeof v->e.coeff);
    memcpy(v->e.mv, vp8_default_mv_context, sizeof v->e.mv);
    memcpy(v->e.y_mode, ymode, sizeof ymode);
    memcpy(v->e.uv_mode, uvmode, sizeof uvmode);
    v->seg_abs = 0;
    memset(v->seg_q, 0, sizeof v->seg_q);
    memset(v->seg_lf, 0, sizeof v->seg_lf);
    memset(v->ref_delta, 0, sizeof v->ref_delta);
    memset(v->mode_delta, 0, sizeof v->mode_delta);
}

/* the compressed header, from the start of the first partition */
static void parse_header(Vp8State *v, Vp8Hdr *h, const uint8_t *part, int n)
{
    Bool b;
    bool_init(&b, part, n);
    if (h->key) {
        key_frame_defaults(v);
        flag(&b);                               /* color_space */
        flag(&b);                               /* clamping_type: the hardware always clamps, same result */
    }
    h->seg_enabled = flag(&b);
    if (h->seg_enabled) {
        h->seg_update_map = flag(&b);
        h->seg_update_data = flag(&b);
        if (h->seg_update_data) {
            v->seg_abs = flag(&b);
            for (int i = 0; i < 4; i++) v->seg_q[i] = opt_signed(&b, 7);
            for (int i = 0; i < 4; i++) v->seg_lf[i] = opt_signed(&b, 6);
        }
        if (h->seg_update_map)
            for (int i = 0; i < 3; i++) v->seg_probs[i] = flag(&b) ? lit(&b, 8) : 255;
    }
    h->filter_simple = flag(&b);
    h->lf_level = lit(&b, 6);
    h->sharpness = lit(&b, 3);
    h->lf_delta_enabled = flag(&b);
    if (h->lf_delta_enabled) {
        h->lf_delta_update = flag(&b);
        if (h->lf_delta_update) {
            for (int i = 0; i < 4; i++) if (flag(&b)) v->ref_delta[i] = signed_lit(&b, 6);
            for (int i = 0; i < 4; i++) if (flag(&b)) v->mode_delta[i] = signed_lit(&b, 6);
        }
    }
    h->log2_parts = lit(&b, 2);
    h->y_ac_qi = lit(&b, 7);
    h->y_dc = opt_signed(&b, 4);
    h->y2_dc = opt_signed(&b, 4);
    h->y2_ac = opt_signed(&b, 4);
    h->uv_dc = opt_signed(&b, 4);
    h->uv_ac = opt_signed(&b, 4);
    int refresh_entropy;
    if (h->key) {
        h->refresh_golden = h->refresh_alt = h->refresh_last = 1;
        refresh_entropy = flag(&b);
    } else {
        h->refresh_golden = flag(&b);
        h->refresh_alt = flag(&b);
        if (!h->refresh_golden) h->copy_to_golden = lit(&b, 2);
        if (!h->refresh_alt) h->copy_to_alt = lit(&b, 2);
        h->sign_golden = flag(&b);
        h->sign_alt = flag(&b);
        refresh_entropy = flag(&b);
        h->refresh_last = flag(&b);
    }
    if (!refresh_entropy) v->saved = v->e;      /* this frame's updates are for this frame only */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                for (int l = 0; l < 11; l++)
                    if (rb(&b, vp8_coef_update_probs[i][j][k][l])) v->e.coeff[i][j][k][l] = lit(&b, 8);
    h->no_coeff_skip = flag(&b);
    h->prob_skip = h->no_coeff_skip ? lit(&b, 8) : 0;
    if (!h->key) {
        h->prob_intra = lit(&b, 8);
        h->prob_last = lit(&b, 8);
        h->prob_gf = lit(&b, 8);
        if (flag(&b)) for (int i = 0; i < 4; i++) v->e.y_mode[i] = lit(&b, 8);
        if (flag(&b)) for (int i = 0; i < 3; i++) v->e.uv_mode[i] = lit(&b, 8);
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 19; j++)
                if (rb(&b, vp8_mv_update_probs[i][j])) { int x = lit(&b, 7); v->e.mv[i][j] = x ? x << 1 : 1; }
    }
    h->e = v->e;
    if (h->key) {                               /* a key frame's modes use fixed probabilities; Cedrus reads them from here */
        static const uint8_t kf_ymode[4] = { 145, 156, 163, 128 }, kf_uvmode[3] = { 142, 114, 183 };
        memcpy(h->e.y_mode, kf_ymode, sizeof kf_ymode);
        memcpy(h->e.uv_mode, kf_uvmode, sizeof kf_uvmode);
    }
    if (!refresh_entropy) v->e = v->saved;
    memcpy(h->seg_q, v->seg_q, sizeof h->seg_q);
    memcpy(h->seg_lf, v->seg_lf, sizeof h->seg_lf);
    memcpy(h->seg_probs, v->seg_probs, sizeof h->seg_probs);
    memcpy(h->ref_delta, v->ref_delta, sizeof h->ref_delta);
    memcpy(h->mode_delta, v->mode_delta, sizeof h->mode_delta);
    h->seg_abs = v->seg_abs;

    b.code = renorm(&b);                        /* where the macroblock data starts */
    h->coder_range = b.high;
    h->coder_value = b.code >> 16;
    h->coder_bits = -b.bits % 8;
    h->header_bits = 8 * (int)(b.p - (-b.bits / 8) - part) - h->coder_bits - 8;
}

/* 0, or -1 when the partition table does not fit the frame */
static int partitions(Vp8Hdr *h, const uint8_t *d, int size)
{
    int n = 1 << h->log2_parts, table = h->header_size + h->first_part_size, pos = table + 3 * (n - 1);
    if (pos > size) return -1;
    for (int i = 0; i < n - 1; i++) {
        const uint8_t *t = d + table + 3 * i;
        uint32_t len = t[0] | t[1] << 8 | (uint32_t)t[2] << 16;
        if (len > (uint32_t)(size - pos)) return -1;
        h->part_size[i] = len;
        pos += len;
    }
    h->part_size[n - 1] = size - pos;
    return 0;
}

/* the reference buffers after a frame: alt-ref first, then golden, as libvpx does */
static void update_refs(SL *s, Vp8State *v, const Vp8Hdr *h, int cur)
{
    if (h->copy_to_alt == 1) v->alt = v->last;
    else if (h->copy_to_alt == 2) v->alt = v->golden;
    if (h->copy_to_golden == 1) v->golden = v->last;
    else if (h->copy_to_golden == 2) v->golden = v->alt;
    if (h->refresh_golden) v->golden = cur;
    if (h->refresh_alt) v->alt = cur;
    if (h->refresh_last) v->last = cur;
    for (int i = 0; i < s->nslots; i++) s->slot[i].ref = i == v->last || i == v->golden || i == v->alt;
}

int vp8_frame(SL *s, const uint8_t *d, int size, int64_t pts)
{
    Vp8State *v = s->codec_state;
    if (size < 3) return PMHWDEC_ERROR;
    uint32_t tag = d[0] | d[1] << 8 | (uint32_t)d[2] << 16;
    Vp8Hdr h;
    memset(&h, 0, sizeof h);
    h.key = !(tag & 1);
    h.version = tag >> 1 & 7;
    h.show = tag >> 4 & 1;
    h.first_part_size = tag >> 5;
    h.header_size = h.key ? 10 : 3;
    if (h.version > 3) { ERR("vp8: bitstream version %d is not defined", h.version); return PMHWDEC_UNSUPPORTED; }
    if (size < h.header_size || h.first_part_size > size - h.header_size) return PMHWDEC_ERROR;
    if (s->need_key && !h.key) return PMHWDEC_OK;  /* wait for a key frame */
    if (h.key) {
        if (d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a) return PMHWDEC_ERROR;
        h.width = (d[6] | d[7] << 8) & 0x3fff;
        h.height = (d[8] | d[9] << 8) & 0x3fff;
        h.hscale = d[7] >> 6;
        h.vscale = d[9] >> 6;
        if (!h.width || !h.height) return PMHWDEC_ERROR;
        Geom g;
        memset(&g, 0, sizeof g);
        g.codec = PMHWDEC_CODEC_VP8;
        g.width = ALIGN(h.width, 16);
        g.height = ALIGN(h.height, 16);
        g.stride = g.width;
        g.coded_h = g.height;
        g.vis_w = h.width;
        g.vis_h = h.height;
        g.bit_depth = 8;
        g.nslots = 4 + SL_EXTRA_SLOTS + 1;      /* three references, the new frame, the caller's, one to spare */
        int rc = sl_configure(s, &g, 0);
        if (rc != PMHWDEC_OK) return rc;           /* nothing has changed yet: PMHWDEC_AGAIN resends this frame */
        vp8_reset(s);
    }
    if (!s->configured) return PMHWDEC_ERROR;
    h.width = s->geom.vis_w;
    h.height = s->geom.vis_h;
    int cur = sl_free_slot(s);
    if (cur < 0) { s->bump = 1; return PMHWDEC_AGAIN; }
    parse_header(v, &h, d + h.header_size, h.first_part_size);
    if (partitions(&h, d, size)) { s->need_key = 1; return PMHWDEC_ERROR; }
    Vp8Job j = { .h = &h, .slots = s->slot, .nslots = s->nslots, .cur = cur, .last = v->last, .golden = v->golden,
                 .alt = v->alt, .data = d, .len = size };
    int r = s->hw->vp8_frame(s->hwctx, &j);
    if (r < 0) { s->need_key = 1; return PMHWDEC_ERROR; }
    s->need_key = 0;
    Slot *t = &s->slot[cur];
    t->corrupt = r > 0;
    t->pts = pts;
    t->poc = v->count++;
    update_refs(s, v, &h, cur);
    if (h.show) sl_queue(s, cur);
    return r > 0 ? PMHWDEC_ERROR : PMHWDEC_OK;
}
