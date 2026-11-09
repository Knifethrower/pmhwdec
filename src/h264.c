// SPDX-License-Identifier: LGPL-2.1-or-later
/* H.264 codec layer for stateless backends: parameter sets, slice headers, POC (8.2.1), reference marking (8.2.5),
 * reference lists (8.2.4), output reordering depth (VUI max_num_reorder_frames). Frame pictures only. */
#include <stdlib.h>
#include <string.h>
#include "bits.h"
#include "codec.h"

typedef struct H264State {
    H264Sps *sps[32];
    H264Pps *pps[256];
    int sps_id;                                      /* of the configured stream, -1 = none */
    int reorder_vui[32];                             /* per SPS id: max_num_reorder_frames, -1 = absent */
    int dec_buffering[32];
    /* POC state */
    int prev_ref_poc_msb, prev_ref_poc_lsb, prev_frame_num, prev_frame_num_offset;
    /* picture in progress */
    H264Slice first;                                 /* first slice's header: marking needs it at the end */
    int cur_poc, frame_num_offset, skipping, mmco5;
} H264State;

static int profile_has_chroma_info(int p)
{
    return p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 ||
           p == 138 || p == 139 || p == 134 || p == 135;
}

static void skip_hrd(Bits *b)
{
    int n = ue(b) + 1;
    u(b, 4); u(b, 4);
    for (int i = 0; i < n && i < 32; i++) { ue(b); ue(b); u(b, 1); }
    u(b, 5); u(b, 5); u(b, 5); u(b, 5);
}

static int parse_sps(Bits *b, H264Sps *s, int *reorder, int *dec_buf)
{
    memset(s, 0, sizeof *s);
    *reorder = -1; *dec_buf = -1;
    s->profile = u(b, 8);
    int cf = u(b, 8);
    s->constraint_flags = 0;
    for (int i = 0; i < 6; i++) if (cf & (0x80 >> i)) s->constraint_flags |= 1 << i;   /* V4L2: set0 = bit 0 */
    s->level = u(b, 8);
    int id = ue(b);
    if (id > 31) return -1;
    s->id = id;
    s->chroma_format_idc = 1;
    if (profile_has_chroma_info(s->profile)) {
        s->chroma_format_idc = ue(b);
        if (s->chroma_format_idc != 1) { ERR("H.264: only 4:2:0 is supported"); return -2; }
        if (ue(b) || ue(b)) { ERR("H.264: only 8-bit streams"); return -2; }
        s->qpprime_bypass = u(b, 1);
        if (u(b, 1)) { ERR("H.264: scaling matrices are not supported yet"); return -2; }
    }
    s->log2_max_frame_num = ue(b) + 4;
    s->poc_type = ue(b);
    if (s->poc_type == 0) s->log2_max_poc_lsb = ue(b) + 4;
    else if (s->poc_type == 1) {
        s->delta_pic_order_always_zero = u(b, 1);
        s->offset_non_ref = se(b); s->offset_top_bottom = se(b);
        s->num_ref_cycle = ue(b);
        if (s->num_ref_cycle > 255) return -1;
        for (int i = 0; i < s->num_ref_cycle; i++) s->offset_ref[i] = se(b);
    } else if (s->poc_type > 2) return -1;
    s->max_num_ref_frames = ue(b);
    s->gaps_allowed = u(b, 1);
    s->width_mbs = ue(b) + 1;
    s->height_map_units = ue(b) + 1;
    s->frame_mbs_only = u(b, 1);
    if (!s->frame_mbs_only) { ERR("H.264: interlaced streams are not supported"); return -2; }
    s->direct_8x8 = u(b, 1);
    if (u(b, 1)) { s->crop_l = ue(b); s->crop_r = ue(b); s->crop_t = ue(b); s->crop_b = ue(b); }
    if (u(b, 1)) {                                   /* VUI, for the reorder depth */
        if (u(b, 1) && u(b, 8) == 255) { u(b, 16); u(b, 16); }
        if (u(b, 1)) u(b, 1);
        if (u(b, 1)) { u(b, 4); if (u(b, 1)) { u(b, 8); u(b, 8); u(b, 8); } }
        if (u(b, 1)) { ue(b); ue(b); }
        if (u(b, 1)) { u(b, 32); u(b, 32); u(b, 1); }
        int nal_hrd = u(b, 1);
        if (nal_hrd) skip_hrd(b);
        int vcl_hrd = u(b, 1);
        if (vcl_hrd) skip_hrd(b);
        if (nal_hrd || vcl_hrd) u(b, 1);
        u(b, 1);
        if (u(b, 1)) {
            u(b, 1); ue(b); ue(b); ue(b); ue(b);
            *reorder = ue(b); *dec_buf = ue(b);
            if (bits_overrun(b)) *reorder = *dec_buf = -1;
        }
    }
    s->ok = 1;
    return id;
}

static int parse_pps(Bits *b, H264Pps *p, H264Sps **spss)
{
    memset(p, 0, sizeof *p);
    int id = ue(b);
    if (id > 255) return -1;
    p->id = id;
    p->sps_id = ue(b);
    if (p->sps_id > 31) return -1;
    p->entropy = u(b, 1);
    p->bottom_field_poc_present = u(b, 1);
    if (ue(b)) { ERR("H.264: slice groups (FMO) are not supported"); return -2; }
    p->num_ref_l0 = ue(b); p->num_ref_l1 = ue(b);
    if (p->num_ref_l0 > 31 || p->num_ref_l1 > 31) return -1;
    p->weighted_pred = u(b, 1); p->weighted_bipred = u(b, 2);
    p->init_qp = se(b); p->init_qs = se(b);
    p->chroma_qp_offset = p->second_chroma_qp_offset = se(b);
    p->deblock_ctrl = u(b, 1); p->constrained_intra = u(b, 1); p->redundant_pic_cnt = u(b, 1);
    if (more_rbsp(b)) {
        p->t8x8 = u(b, 1);
        if (u(b, 1)) { ERR("H.264: PPS scaling matrices are not supported yet"); return -2; }
        p->second_chroma_qp_offset = se(b);
    }
    (void)spss;
    p->ok = 1;
    return id;
}

static int parse_slice(Bits *b, H264State *h, H264Slice *sl, const H264Sps **sps_out, const H264Pps **pps_out)
{
    memset(sl, 0, sizeof *sl);
    u(b, 1);
    sl->nal_ref_idc = u(b, 2);
    sl->nal_type = u(b, 5);
    sl->idr = sl->nal_type == 5;
    sl->first_mb = ue(b);
    sl->type = ue(b) % 5;
    if (sl->type > 2) { ERR("H.264: SP/SI slices are not supported"); return -1; }
    sl->pps_id = ue(b);
    if (sl->pps_id > 255 || !h->pps[sl->pps_id]) return -1;
    const H264Pps *pps = h->pps[sl->pps_id];
    const H264Sps *sps = h->sps[pps->sps_id];
    if (!sps) return -1;
    *sps_out = sps; *pps_out = pps;
    sl->frame_num = u(b, sps->log2_max_frame_num);
    if (sl->idr) sl->idr_pic_id = ue(b);
    int poc_start = b->pos;
    if (sps->poc_type == 0) {
        sl->poc_lsb = u(b, sps->log2_max_poc_lsb);
        if (pps->bottom_field_poc_present) sl->delta_poc_bottom = se(b);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
        sl->delta_poc[0] = se(b);
        if (pps->bottom_field_poc_present) sl->delta_poc[1] = se(b);
    }
    sl->poc_bits = b->pos - poc_start;
    if (pps->redundant_pic_cnt && ue(b)) return -1;   /* redundant pictures: dropped */
    if (sl->type == H264_B) sl->direct_spatial = u(b, 1);
    sl->num_ref[0] = pps->num_ref_l0; sl->num_ref[1] = pps->num_ref_l1;
    if (sl->type != H264_I && u(b, 1)) {
        sl->num_ref[0] = ue(b);
        if (sl->type == H264_B) sl->num_ref[1] = ue(b);
    }
    if (sl->num_ref[0] > 31 || sl->num_ref[1] > 31) return -1;
    for (int l = 0; l < (sl->type == H264_B ? 2 : sl->type == H264_P ? 1 : 0); l++) {
        if (!u(b, 1)) continue;
        for (;;) {
            int idc = ue(b);
            if (idc == 3) break;
            if (idc > 5 || sl->nmod[l] >= 33) return -1;
            sl->mod_idc[l][sl->nmod[l]] = idc;
            sl->mod_val[l][sl->nmod[l]++] = ue(b);
        }
    }
    if ((pps->weighted_pred && sl->type == H264_P) || (pps->weighted_bipred == 1 && sl->type == H264_B)) {
        sl->luma_denom = ue(b);
        if (sps->chroma_format_idc) sl->chroma_denom = ue(b);
        for (int l = 0; l < (sl->type == H264_B ? 2 : 1); l++)
            for (int i = 0; i <= sl->num_ref[l]; i++) {
                sl->lw[l][i] = 1 << sl->luma_denom; sl->lo[l][i] = 0;
                if (u(b, 1)) { sl->lw[l][i] = se(b); sl->lo[l][i] = se(b); }
                for (int c = 0; c < 2; c++) { sl->cw[l][i][c] = 1 << sl->chroma_denom; sl->co[l][i][c] = 0; }
                if (sps->chroma_format_idc && u(b, 1))
                    for (int c = 0; c < 2; c++) { sl->cw[l][i][c] = se(b); sl->co[l][i][c] = se(b); }
            }
    }
    int marking_start = b->pos;
    if (sl->nal_ref_idc) {
        if (sl->idr) { u(b, 1); if (u(b, 1)) { ERR("H.264: long-term IDR is not supported"); return -1; } }
        else if ((sl->adaptive = u(b, 1))) {
            for (;;) {
                int op = ue(b);
                if (op == 0) break;
                if (op > 6 || sl->nmmco >= 33) return -1;
                sl->mmco[sl->nmmco][0] = op;
                sl->mmco[sl->nmmco][1] = (op == 1 || op == 3) ? (int)ue(b) : 0;
                if (op == 2) sl->mmco[sl->nmmco][1] = ue(b);
                if (op == 3 || op == 6) ue(b);
                if (op == 4) ue(b);
                sl->nmmco++;
            }
        }
    }
    sl->ref_marking_bits = b->pos - marking_start;
    if (pps->entropy && sl->type != H264_I) sl->cabac_init = ue(b);
    sl->qp_delta = se(b);
    if (pps->deblock_ctrl) {
        sl->disable_deblock = ue(b);
        if (sl->disable_deblock != 1) { sl->alpha = se(b); sl->beta = se(b); }
    }
    sl->header_bits = b->pos;
    return bits_overrun(b) ? -1 : 0;
}

/* ---------------- references ---------------- */
static void pic_nums(SL *s, int frame_num, int max_fn)
{
    for (int i = 0; i < s->nslots; i++)
        if (s->slot[i].ref) s->slot[i].pic_num = s->slot[i].frame_num > frame_num ? s->slot[i].frame_num - max_fn : s->slot[i].frame_num;
}

static SL *sort_sl;
static int by_picnum_desc(const void *a, const void *b) { return sort_sl->slot[*(int *)b].pic_num - sort_sl->slot[*(int *)a].pic_num; }
static int by_poc_desc(const void *a, const void *b) { return sort_sl->slot[*(int *)b].poc - sort_sl->slot[*(int *)a].poc; }
static int by_poc_asc(const void *a, const void *b) { return sort_sl->slot[*(int *)a].poc - sort_sl->slot[*(int *)b].poc; }

/* 0 = complete, 1 = some entries substituted (missing references) */
static int build_lists(SL *s, const H264Sps *sps, const H264Slice *sl, int cur_poc, int list[2][32], int len[2])
{
    int refs[MAX_SLOTS], n = 0, missing = 0, max_fn = 1 << sps->log2_max_frame_num;
    pic_nums(s, sl->frame_num, max_fn);
    for (int i = 0; i < s->nslots; i++) if (s->slot[i].ref && i != s->cur) refs[n++] = i;
    sort_sl = s;
    len[0] = len[1] = 0;
    if (sl->type == H264_P) {
        qsort(refs, n, sizeof *refs, by_picnum_desc);
        memcpy(list[0], refs, n * sizeof(int)); len[0] = n;
    } else if (sl->type == H264_B) {
        int before[MAX_SLOTS], after[MAX_SLOTS], nb = 0, na = 0;
        for (int i = 0; i < n; i++) {
            if (s->slot[refs[i]].poc < cur_poc) before[nb++] = refs[i];
            else after[na++] = refs[i];
        }
        qsort(before, nb, sizeof *before, by_poc_desc);
        qsort(after, na, sizeof *after, by_poc_asc);
        memcpy(list[0], before, nb * sizeof(int)); memcpy(list[0] + nb, after, na * sizeof(int));
        memcpy(list[1], after, na * sizeof(int)); memcpy(list[1] + na, before, nb * sizeof(int));
        len[0] = len[1] = n;
        if (n > 1 && !memcmp(list[0], list[1], n * sizeof(int))) { int t = list[1][0]; list[1][0] = list[1][1]; list[1][1] = t; }
    }
    for (int l = 0; l < (sl->type == H264_B ? 2 : sl->type == H264_P ? 1 : 0); l++) {
        int active = sl->num_ref[l] + 1, pred = sl->frame_num;
        if (len[l] > active) len[l] = active;
        for (int m = 0; m < sl->nmod[l]; m++) {
            int idc = sl->mod_idc[l][m], d = sl->mod_val[l][m] + 1, nowrap;
            if (idc > 1) { missing = 1; continue; }  /* long-term / MVC: not supported, keep the default order */
            if (idc == 0) { nowrap = pred - d; if (nowrap < 0) nowrap += max_fn; }
            else { nowrap = pred + d; if (nowrap >= max_fn) nowrap -= max_fn; }
            pred = nowrap;
            int pn = nowrap > sl->frame_num ? nowrap - max_fn : nowrap, target = -1;
            for (int i = 0; i < s->nslots; i++) if (s->slot[i].ref && i != s->cur && s->slot[i].pic_num == pn) target = i;
            if (target < 0) { missing = 1; continue; }
            int tmp[34], k = 0;
            for (int i = 0; i < m && i < len[l]; i++) tmp[k++] = list[l][i];
            tmp[k++] = target;
            for (int i = m; i < len[l]; i++) if (list[l][i] != target) tmp[k++] = list[l][i];
            if (k > active) k = active;
            memcpy(list[l], tmp, k * sizeof(int)); len[l] = k;
        }
        if (len[l] < active) {                       /* missing references: repeat what there is */
            if (!len[l]) return -1;
            missing = 1;
            for (int i = len[l]; i < active; i++) list[l][i] = list[l][len[l] - 1];
            len[l] = active;
        }
    }
    return missing;
}

/* ---------------- codec layer entry points ---------------- */
void *h264_new(void)
{
    H264State *h = calloc(1, sizeof *h);
    if (h) h->sps_id = -1;
    return h;
}

void h264_free(void *st)
{
    H264State *h = st;
    if (!h) return;
    for (int i = 0; i < 32; i++) free(h->sps[i]);
    for (int i = 0; i < 256; i++) free(h->pps[i]);
    free(h);
}

void h264_reset(SL *s)
{
    H264State *h = s->codec_state;
    h->prev_ref_poc_msb = h->prev_ref_poc_lsb = h->prev_frame_num = h->prev_frame_num_offset = 0;
    h->skipping = 0;
}

static int compute_poc(H264State *h, const H264Sps *sps, const H264Slice *sl, int *frame_num_offset)
{
    int max_fn = 1 << sps->log2_max_frame_num, top, bottom;
    if (sl->idr) *frame_num_offset = 0;
    else *frame_num_offset = h->prev_frame_num > sl->frame_num ? h->prev_frame_num_offset + max_fn : h->prev_frame_num_offset;
    if (sps->poc_type == 0) {
        int max_lsb = 1 << sps->log2_max_poc_lsb, pm = sl->idr ? 0 : h->prev_ref_poc_msb, pl = sl->idr ? 0 : h->prev_ref_poc_lsb, msb;
        if (sl->poc_lsb < pl && pl - sl->poc_lsb >= max_lsb / 2) msb = pm + max_lsb;
        else if (sl->poc_lsb > pl && sl->poc_lsb - pl > max_lsb / 2) msb = pm - max_lsb;
        else msb = pm;
        top = msb + sl->poc_lsb;
        bottom = top + sl->delta_poc_bottom;
    } else if (sps->poc_type == 1) {
        int abs_fn = sps->num_ref_cycle ? *frame_num_offset + sl->frame_num : 0, expected = 0, delta_cycle = 0;
        if (!sl->nal_ref_idc && abs_fn > 0) abs_fn--;
        for (int i = 0; i < sps->num_ref_cycle; i++) delta_cycle += sps->offset_ref[i];
        if (abs_fn > 0) {
            int cycle = (abs_fn - 1) / sps->num_ref_cycle, in_cycle = (abs_fn - 1) % sps->num_ref_cycle;
            expected = cycle * delta_cycle;
            for (int i = 0; i <= in_cycle; i++) expected += sps->offset_ref[i];
        }
        if (!sl->nal_ref_idc) expected += sps->offset_non_ref;
        top = expected + sl->delta_poc[0];
        bottom = top + sps->offset_top_bottom + sl->delta_poc[1];
    } else {
        top = bottom = sl->idr ? 0 : 2 * (*frame_num_offset + sl->frame_num) - (sl->nal_ref_idc == 0);
    }
    return top < bottom ? top : bottom;
}

static void geometry(const H264Sps *sps, int reorder, int dec_buf, Geom *g, int *reorder_out)
{
    memset(g, 0, sizeof *g);
    g->codec = PMHWDEC_CODEC_H264;
    g->width = sps->width_mbs * 16;
    g->height = sps->height_map_units * 16;
    g->stride = ALIGN(g->width, 16);
    g->coded_h = ALIGN(g->height, 16);
    g->vis_x = 2 * sps->crop_l; g->vis_y = 2 * sps->crop_t;
    g->vis_w = g->width - 2 * (sps->crop_l + sps->crop_r);
    g->vis_h = g->height - 2 * (sps->crop_t + sps->crop_b);
    g->bit_depth = 8;
    g->h264_direct_8x8 = sps->direct_8x8;
    g->h264_frame_mbs_only = sps->frame_mbs_only;
    int refs = sps->max_num_ref_frames;
    if (dec_buf > refs) refs = dec_buf;
    int r = reorder >= 0 ? reorder : (sps->poc_type == 2 || sps->profile == 66) ? 0 : sps->max_num_ref_frames;
    if (r > 16) r = 16;
    int n = refs + r + 1 + SL_EXTRA_SLOTS;
    if (n > MAX_SLOTS) n = MAX_SLOTS;
    if (n < refs + 2) n = refs + 2 > MAX_SLOTS ? MAX_SLOTS : refs + 2;
    g->nslots = n;
    *reorder_out = r;
}

int h264_nal(SL *s, const uint8_t *nal, int len, int64_t pts)
{
    H264State *h = s->codec_state;
    int t = nal[0] & 0x1f;
    if (t != 1 && t != 5 && t != 7 && t != 8) return PMHWDEC_OK;
    int n = sl_unescape(s, nal, len);
    if (n < 0) return PMHWDEC_ERROR;
    Bits b = { s->rbsp, n, 8 };
    if (t == 7) {
        H264Sps tmp;
        int ro, db, id = parse_sps(&b, &tmp, &ro, &db);
        if (id < 0) return id == -2 ? PMHWDEC_UNSUPPORTED : PMHWDEC_ERROR;
        if (!h->sps[id] && !(h->sps[id] = malloc(sizeof tmp))) return PMHWDEC_ERROR;
        *h->sps[id] = tmp;
        h->reorder_vui[id] = ro; h->dec_buffering[id] = db;
        return PMHWDEC_OK;
    }
    if (t == 8) {
        H264Pps tmp;
        int id = parse_pps(&b, &tmp, h->sps);
        if (id < 0) return id == -2 ? PMHWDEC_UNSUPPORTED : PMHWDEC_ERROR;
        if (!h->pps[id] && !(h->pps[id] = malloc(sizeof tmp))) return PMHWDEC_ERROR;
        *h->pps[id] = tmp;
        return PMHWDEC_OK;
    }
    b.pos = 0;
    H264Slice sl;
    const H264Sps *sps; const H264Pps *pps;
    if (parse_slice(&b, h, &sl, &sps, &pps)) return PMHWDEC_ERROR;
    if (sl.first_mb == 0) {
        if (s->cur >= 0) h264_end_picture(s);
        h->skipping = 0;
        if (s->need_key && !sl.idr && sl.type != H264_I) { h->skipping = 1; return PMHWDEC_OK; }
        Geom g;
        int reorder, sps_id = pps->sps_id;
        geometry(sps, h->reorder_vui[sps_id], h->dec_buffering[sps_id], &g, &reorder);
        int r = sl_configure(s, &g, reorder);
        if (r != PMHWDEC_OK) return r;
        if (sl.idr) { for (int i = 0; i < s->nslots; i++) s->slot[i].ref = 0; }
        int cur = sl_free_slot(s);
        if (cur < 0) { s->bump = 1; return PMHWDEC_AGAIN; }
        /* nothing below can ask for a resend: commit the picture */
        if (sl.idr) s->epoch++;
        s->need_key = 0;
        int fno;
        h->cur_poc = compute_poc(h, sps, &sl, &fno);
        h->frame_num_offset = fno;
        h->first = sl;
        h->sps_id = sps_id;
        s->cur = cur;
        Slot *c = &s->slot[cur];
        c->poc = h->cur_poc; c->frame_num = sl.frame_num; c->pts = pts; c->corrupt = 0; c->ref = 0;
    } else if (h->skipping || s->cur < 0) {
        return PMHWDEC_OK;
    }
    H264Job j = { .sps = sps, .pps = pps, .sl = &sl, .slots = s->slot, .nslots = s->nslots, .cur = s->cur,
                  .cur_poc = h->cur_poc, .first_slice = sl.first_mb == 0, .nal = nal, .nal_len = len };
    int m = build_lists(s, sps, &sl, h->cur_poc, j.list, j.len);
    if (m < 0) { s->slot[s->cur].corrupt = 1; return PMHWDEC_ERROR; }
    if (m) s->slot[s->cur].corrupt = 1;
    int r = s->hw->h264_slice(s->hwctx, &j);
    if (r) s->slot[s->cur].corrupt = 1;
    return r < 0 ? PMHWDEC_ERROR : PMHWDEC_OK;
}

/* 8.2.5 marking, then into the output queue */
void h264_end_picture(SL *s)
{
    H264State *h = s->codec_state;
    int cur = s->cur;
    if (cur < 0) return;
    if (s->hw->end_picture && s->hw->end_picture(s->hwctx, cur)) s->slot[cur].corrupt = 1;
    const H264Slice *sl = &h->first;
    const H264Sps *sps = h->sps[h->sps_id];
    int max_fn = 1 << sps->log2_max_frame_num;
    h->mmco5 = 0;
    if (sl->nal_ref_idc) {
        if (sl->idr) {
            for (int i = 0; i < s->nslots; i++) s->slot[i].ref = 0;
        } else if (sl->adaptive) {
            pic_nums(s, sl->frame_num, max_fn);
            for (int m = 0; m < sl->nmmco; m++) {
                int op = sl->mmco[m][0];
                if (op == 1) {
                    int pn = sl->frame_num - (sl->mmco[m][1] + 1);
                    for (int i = 0; i < s->nslots; i++) if (s->slot[i].ref && i != cur && s->slot[i].pic_num == pn) s->slot[i].ref = 0;
                } else if (op == 5) {
                    for (int i = 0; i < s->nslots; i++) if (i != cur) s->slot[i].ref = 0;
                    h->mmco5 = 1;
                } else {
                    s->slot[cur].corrupt = 1;        /* long-term operations: not supported */
                }
            }
        } else {
            int n = 0, oldest = -1;
            pic_nums(s, sl->frame_num, max_fn);
            for (int i = 0; i < s->nslots; i++)
                if (s->slot[i].ref && i != cur) { n++; if (oldest < 0 || s->slot[i].pic_num < s->slot[oldest].pic_num) oldest = i; }
            if (n >= (sps->max_num_ref_frames ? sps->max_num_ref_frames : 1) && oldest >= 0) s->slot[oldest].ref = 0;
        }
        s->slot[cur].ref = 1;
    }
    /* POC state for the next picture */
    if (h->mmco5) {
        h->prev_ref_poc_msb = 0; h->prev_ref_poc_lsb = 0;
        h->prev_frame_num = 0; h->prev_frame_num_offset = 0;
    } else {
        if (sl->nal_ref_idc && sps->poc_type == 0) {
            int max_lsb = 1 << sps->log2_max_poc_lsb;
            h->prev_ref_poc_lsb = sl->poc_lsb;
            h->prev_ref_poc_msb = h->cur_poc - sl->poc_lsb - (sl->delta_poc_bottom < 0 ? sl->delta_poc_bottom : 0);
            (void)max_lsb;
        }
        h->prev_frame_num = sl->frame_num;
        h->prev_frame_num_offset = h->frame_num_offset;
    }
    sl_queue(s, cur);
    if (h->mmco5) s->epoch++;                        /* later pictures restart POC: output this sequence first */
    s->cur = -1;
}
