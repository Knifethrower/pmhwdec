// SPDX-License-Identifier: LGPL-2.1-or-later
/* HEVC codec layer for stateless backends: VPS/SPS/PPS, slice segment headers, POC (8.3.1), RPS (8.3.2, 7.4.8),
 * reference lists (8.3.4), WPP entry points, output reordering depth (sps_max_num_reorder_pics).
 * Independent slice segments only; no tiles, no long-term references, no scaling lists (refused). */
#include <stdlib.h>
#include <string.h>
#include "bits.h"
#include "codec.h"

typedef struct HevcState {
    HevcSps *sps[16];
    HevcPps *pps[64];
    int prev_tid0_poc, skip_rasl, first_pic, cur_poc, cur_nal;
    uint32_t ep[1024];
} HevcState;

static int is_irap(int t) { return t >= 16 && t <= 23; }
static int is_idr(int t) { return t == 19 || t == 20; }

static void parse_ptl(Bits *b, int max_sub)
{
    u(b, 8); u(b, 32); u(b, 32); u(b, 16); u(b, 8);
    int pp[8] = {0}, lp[8] = {0};
    for (int i = 0; i < max_sub; i++) { pp[i] = u(b, 1); lp[i] = u(b, 1); }
    if (max_sub > 0) for (int i = max_sub; i < 8; i++) u(b, 2);
    for (int i = 0; i < max_sub; i++) {
        if (pp[i]) { u(b, 32); u(b, 32); u(b, 24); }
        if (lp[i]) u(b, 8);
    }
}

static int parse_rps(Bits *b, const HevcRps *sets, int idx, int num, HevcRps *out)
{
    memset(out, 0, sizeof *out);
    int inter = idx ? u(b, 1) : 0;
    if (inter) {
        int delta_idx = idx == num ? (int)ue(b) + 1 : 1;
        int sign = u(b, 1), abs_d = ue(b) + 1, delta = (1 - 2 * sign) * abs_d;
        if (delta_idx > idx) return -1;
        const HevcRps *r = &sets[idx - delta_idx];
        int n = r->num_neg + r->num_pos, used[33], use_delta[33];
        for (int j = 0; j <= n; j++) { used[j] = u(b, 1); use_delta[j] = used[j] ? 1 : u(b, 1); }
        int i = 0;
        for (int j = r->num_pos - 1; j >= 0; j--) {
            int d = r->d1[j] + delta;
            if (d < 0 && use_delta[r->num_neg + j] && i < 16) { out->d0[i] = d; out->u0[i++] = used[r->num_neg + j]; }
        }
        if (delta < 0 && use_delta[n] && i < 16) { out->d0[i] = delta; out->u0[i++] = used[n]; }
        for (int j = 0; j < r->num_neg; j++) {
            int d = r->d0[j] + delta;
            if (d < 0 && use_delta[j] && i < 16) { out->d0[i] = d; out->u0[i++] = used[j]; }
        }
        out->num_neg = i;
        i = 0;
        for (int j = r->num_neg - 1; j >= 0; j--) {
            int d = r->d0[j] + delta;
            if (d > 0 && use_delta[j] && i < 16) { out->d1[i] = d; out->u1[i++] = used[j]; }
        }
        if (delta > 0 && use_delta[n] && i < 16) { out->d1[i] = delta; out->u1[i++] = used[n]; }
        for (int j = 0; j < r->num_pos; j++) {
            int d = r->d1[j] + delta;
            if (d > 0 && use_delta[r->num_neg + j] && i < 16) { out->d1[i] = d; out->u1[i++] = used[r->num_neg + j]; }
        }
        out->num_pos = i;
    } else {
        out->num_neg = ue(b); out->num_pos = ue(b);
        if (out->num_neg > 16 || out->num_pos > 16) return -1;
        for (int i = 0, p = 0; i < out->num_neg; i++) { p -= ue(b) + 1; out->d0[i] = p; out->u0[i] = u(b, 1); }
        for (int i = 0, p = 0; i < out->num_pos; i++) { p += ue(b) + 1; out->d1[i] = p; out->u1[i] = u(b, 1); }
    }
    return 0;
}

static int parse_sps(Bits *b, HevcSps *s)
{
    memset(s, 0, sizeof *s);
    u(b, 4);
    int max_sub = u(b, 3);
    u(b, 1);
    parse_ptl(b, max_sub);
    int id = ue(b);
    if (id > 15) return -1;
    s->chroma_format_idc = ue(b);
    if (s->chroma_format_idc != 1) { ERR("HEVC: only 4:2:0 is supported"); return -2; }
    s->width = ue(b); s->height = ue(b);
    if (u(b, 1)) { s->conf_l = ue(b); s->conf_r = ue(b); s->conf_t = ue(b); s->conf_b = ue(b); }
    s->bit_depth = ue(b) + 8; s->bit_depth_c = ue(b) + 8;
    s->log2_max_poc_lsb = ue(b) + 4;
    int ordering = u(b, 1);
    for (int i = ordering ? 0 : max_sub; i <= max_sub; i++) {
        s->max_dec_pic_buffering = ue(b) + 1; s->max_num_reorder = ue(b); ue(b);   /* the highest sub-layer's wins */
    }
    s->log2_min_cb = ue(b) + 3; s->log2_diff_cb = ue(b);
    s->log2_min_tb = ue(b) + 2; s->log2_diff_tb = ue(b);
    s->depth_inter = ue(b); s->depth_intra = ue(b);
    if (u(b, 1)) { ERR("HEVC: scaling lists are not supported yet"); return -2; }
    s->amp = u(b, 1); s->sao = u(b, 1);
    if ((s->pcm = u(b, 1))) {
        s->pcm_bd = u(b, 4) + 1; s->pcm_bd_c = u(b, 4) + 1;
        s->log2_min_pcm = ue(b) + 3; s->log2_diff_pcm = ue(b); s->pcm_lf_disabled = u(b, 1);
    }
    s->num_rps = ue(b);
    if (s->num_rps > 64) return -1;
    for (int i = 0; i < s->num_rps; i++) if (parse_rps(b, s->rps, i, s->num_rps, &s->rps[i])) return -1;
    if (u(b, 1)) { ERR("HEVC: long-term references are not supported"); return -2; }
    s->temporal_mvp = u(b, 1); s->strong_intra = u(b, 1);
    if (bits_overrun(b)) return -1;
    s->ok = 1;
    return id;
}

static int parse_pps(Bits *b, HevcPps *p)
{
    memset(p, 0, sizeof *p);
    int id = ue(b);
    if (id > 63) return -1;
    p->sps_id = ue(b);
    if (p->sps_id > 15) return -1;
    p->dependent_slices = u(b, 1); p->output_flag = u(b, 1); p->extra_bits = u(b, 3);
    p->sign_hiding = u(b, 1); p->cabac_init_present = u(b, 1);
    p->num_ref_l0 = ue(b); p->num_ref_l1 = ue(b);
    p->init_qp = se(b);
    p->constrained_intra = u(b, 1); p->transform_skip = u(b, 1);
    if ((p->cu_qp_delta = u(b, 1))) p->diff_cu_qp_delta_depth = ue(b);
    p->cb_qp = se(b); p->cr_qp = se(b);
    p->slice_chroma_qp_offsets = u(b, 1);
    p->weighted_pred = u(b, 1); p->weighted_bipred = u(b, 1);
    p->transquant_bypass = u(b, 1);
    p->tiles = u(b, 1); p->wpp = u(b, 1);
    if (p->tiles) { ERR("HEVC: tiles are not supported yet"); return -2; }
    p->lf_across_slices = u(b, 1);
    if (u(b, 1)) {
        p->deblock_override_enabled = u(b, 1);
        if (!(p->deblock_disabled = u(b, 1))) { p->beta = se(b); p->tc = se(b); }
    }
    if (u(b, 1)) { ERR("HEVC: PPS scaling lists are not supported yet"); return -2; }
    p->lists_mod = u(b, 1);
    p->log2_pml = ue(b);
    p->slice_ext = u(b, 1);
    p->ok = 1;
    return id;
}

static int num_pic_total_curr(const HevcRps *r)
{
    int n = 0;
    for (int i = 0; i < r->num_neg; i++) n += r->u0[i];
    for (int i = 0; i < r->num_pos; i++) n += r->u1[i];
    return n;
}

static int parse_slice(Bits *b, HevcState *h, HevcSlice *sl, const HevcSps **sps_out, const HevcPps **pps_out)
{
    memset(sl, 0, sizeof *sl);
    u(b, 1); sl->nal_type = u(b, 6); u(b, 6); sl->tid = (int)u(b, 3) - 1;
    sl->first = u(b, 1);
    if (is_irap(sl->nal_type)) u(b, 1);
    sl->pps_id = ue(b);
    if (sl->pps_id > 63 || !h->pps[sl->pps_id]) return -1;
    const HevcPps *pps = h->pps[sl->pps_id];
    const HevcSps *sps = h->sps[pps->sps_id];
    if (!sps) return -1;
    *sps_out = sps; *pps_out = pps;
    if (!sl->first) {
        if (pps->dependent_slices && u(b, 1)) { ERR("HEVC: dependent slice segments are not supported"); return -1; }
        int ctb = 1 << (sps->log2_min_cb + sps->log2_diff_cb);
        int ctbs = ((sps->width + ctb - 1) / ctb) * ((sps->height + ctb - 1) / ctb);
        sl->address = u(b, ceil_log2(ctbs));
    }
    u(b, pps->extra_bits);
    sl->type = ue(b);
    if (sl->type > 2) return -1;
    if (pps->output_flag) u(b, 1);
    if (!is_idr(sl->nal_type)) {
        sl->poc_lsb = u(b, sps->log2_max_poc_lsb);
        if (!u(b, 1)) {
            if (parse_rps(b, sps->rps, sps->num_rps, sps->num_rps, &sl->rps)) return -1;
        } else {
            int idx = sps->num_rps > 1 ? (int)u(b, ceil_log2(sps->num_rps)) : 0;
            if (idx >= sps->num_rps) return -1;
            sl->rps = sps->rps[idx];
        }
        if (sps->temporal_mvp) sl->temporal_mvp = u(b, 1);
    }
    if (sps->sao) { sl->sao_luma = u(b, 1); sl->sao_chroma = u(b, 1); }
    sl->col_from_l0 = 1;
    if (sl->type != HEVC_I) {
        sl->num_ref[0] = pps->num_ref_l0; sl->num_ref[1] = pps->num_ref_l1;
        if (u(b, 1)) { sl->num_ref[0] = ue(b); if (sl->type == HEVC_B) sl->num_ref[1] = ue(b); }
        if (sl->num_ref[0] > 15 || sl->num_ref[1] > 15) return -1;
        int total = num_pic_total_curr(&sl->rps);
        if (pps->lists_mod && total > 1)
            for (int l = 0; l < (sl->type == HEVC_B ? 2 : 1); l++)
                if ((sl->mod[l] = u(b, 1)))
                    for (int i = 0; i <= sl->num_ref[l]; i++) sl->list_entry[l][i] = u(b, ceil_log2(total));
        if (sl->type == HEVC_B) sl->mvd_l1_zero = u(b, 1);
        if (pps->cabac_init_present) sl->cabac_init = u(b, 1);
        if (sl->temporal_mvp) {
            if (sl->type == HEVC_B) sl->col_from_l0 = u(b, 1);
            if ((sl->col_from_l0 && sl->num_ref[0] > 0) || (!sl->col_from_l0 && sl->num_ref[1] > 0)) sl->col_ref_idx = ue(b);
        }
        if ((pps->weighted_pred && sl->type == HEVC_P) || (pps->weighted_bipred && sl->type == HEVC_B)) {
            sl->luma_denom = ue(b);
            sl->chroma_denom = sl->luma_denom + se(b);
            if (sl->luma_denom > 7 || sl->chroma_denom < 0 || sl->chroma_denom > 7) return -1;
            for (int l = 0; l < (sl->type == HEVC_B ? 2 : 1); l++) {
                int lf[16], cf[16];
                for (int i = 0; i <= sl->num_ref[l]; i++) lf[i] = u(b, 1);
                for (int i = 0; i <= sl->num_ref[l]; i++) cf[i] = u(b, 1);
                for (int i = 0; i <= sl->num_ref[l]; i++) {
                    if (lf[i]) { sl->dlw[l][i] = se(b); sl->lo[l][i] = se(b); }
                    for (int j = 0; j < 2; j++) {
                        int dw = 0, doff = 0;
                        if (cf[i]) { dw = se(b); doff = se(b); }
                        int w = (1 << sl->chroma_denom) + dw;
                        int off = 128 + doff - ((128 * w) >> sl->chroma_denom);
                        sl->dcw[l][i][j] = dw;
                        sl->co[l][i][j] = off < -128 ? -128 : off > 127 ? 127 : off;
                    }
                }
            }
        }
        sl->max_merge = ue(b);
    }
    sl->qp_delta = se(b);
    if (pps->slice_chroma_qp_offsets) { sl->cb_qp = se(b); sl->cr_qp = se(b); }
    sl->deblock_disabled = pps->deblock_disabled; sl->beta = pps->beta; sl->tc = pps->tc;
    if (pps->deblock_override_enabled && u(b, 1)) {
        if (!(sl->deblock_disabled = u(b, 1))) { sl->beta = se(b); sl->tc = se(b); }
    }
    sl->lf_across = pps->lf_across_slices;
    if (pps->lf_across_slices && (sl->sao_luma || sl->sao_chroma || !sl->deblock_disabled)) sl->lf_across = u(b, 1);
    if (pps->tiles || pps->wpp) {
        sl->num_ep = ue(b);
        if (sl->num_ep > 1024) return -1;
        if (sl->num_ep) {
            int len = ue(b) + 1;
            if (len > 32) return -1;
            for (int i = 0; i < sl->num_ep; i++) sl->ep[i] = u(b, len);
        }
    }
    if (pps->slice_ext) { int n = ue(b); while (n-- > 0) u(b, 8); }
    sl->header_bits = b->pos;
    sl->data_byte = (b->pos + 8) / 8;
    return bits_overrun(b) ? -1 : 0;
}

/* ---------------- entry points ---------------- */
void *hevc_new(void)
{
    HevcState *h = calloc(1, sizeof *h);
    if (h) h->first_pic = 1;
    return h;
}

void hevc_free(void *st)
{
    HevcState *h = st;
    if (!h) return;
    for (int i = 0; i < 16; i++) free(h->sps[i]);
    for (int i = 0; i < 64; i++) free(h->pps[i]);
    free(h);
}

void hevc_reset(SL *s)
{
    HevcState *h = s->codec_state;
    h->prev_tid0_poc = 0;
    h->first_pic = 1;
    h->skip_rasl = 0;
}

static void geometry(const HevcSps *sps, Geom *g, int *reorder)
{
    memset(g, 0, sizeof *g);
    g->codec = PMHW_CODEC_HEVC;
    g->width = sps->width; g->height = sps->height;
    g->stride = ALIGN(g->width, 16); g->coded_h = ALIGN(g->height, 16);
    g->vis_x = 2 * sps->conf_l; g->vis_y = 2 * sps->conf_t;
    g->vis_w = sps->width - 2 * (sps->conf_l + sps->conf_r);
    g->vis_h = sps->height - 2 * (sps->conf_t + sps->conf_b);
    g->bit_depth = sps->bit_depth;
    g->hevc_ctb_log2 = sps->log2_min_cb + sps->log2_diff_cb;
    int r = sps->max_num_reorder > 15 ? 15 : sps->max_num_reorder;
    int n = sps->max_dec_pic_buffering + r + SL_EXTRA_SLOTS;
    if (n > 16) n = 16;
    if (n < 4) n = 4;
    g->nslots = n;
    *reorder = r;
}

int hevc_nal(SL *s, const uint8_t *nal, int len, int64_t pts)
{
    HevcState *h = s->codec_state;
    if (len < 3) return PMHW_OK;
    int t = nal[0] >> 1 & 0x3f;
    if (t != 33 && t != 34 && !(t <= 9) && !(t >= 16 && t <= 21)) return PMHW_OK;
    int n = sl_unescape(s, nal, len);
    if (n < 0) return PMHW_ERROR;
    Bits b = { s->rbsp, n, 16 };
    if (t == 33 || t == 34) {
        if (t == 33) {
            HevcSps tmp;
            int id = parse_sps(&b, &tmp);
            if (id < 0) return id == -2 ? PMHW_UNSUPPORTED : PMHW_ERROR;
            if (!h->sps[id] && !(h->sps[id] = malloc(sizeof tmp))) return PMHW_ERROR;
            *h->sps[id] = tmp;
        } else {
            HevcPps tmp;
            int id = parse_pps(&b, &tmp);
            if (id < 0) return id == -2 ? PMHW_UNSUPPORTED : PMHW_ERROR;
            if (!h->pps[id] && !(h->pps[id] = malloc(sizeof tmp))) return PMHW_ERROR;
            *h->pps[id] = tmp;
        }
        return PMHW_OK;
    }
    b.pos = 0;
    HevcSlice *sl = malloc(sizeof *sl);
    if (!sl) return PMHW_ERROR;
    const HevcSps *sps; const HevcPps *pps;
    int rc = PMHW_OK;
    if (parse_slice(&b, h, sl, &sps, &pps)) { rc = PMHW_ERROR; goto out; }
    if (sl->first) {
        if (s->cur >= 0) hevc_end_picture(s);
        if (s->need_key && !is_irap(t)) goto out;    /* wait for a random access point */
        int new_cvs = is_irap(t) && (is_idr(t) || h->first_pic || s->need_key);
        if ((t == 8 || t == 9) && h->skip_rasl) goto out;
        Geom g;
        int reorder;
        geometry(sps, &g, &reorder);
        if ((rc = sl_configure(s, &g, reorder)) != PMHW_OK) goto out;
        int poc;
        if (new_cvs) poc = is_idr(t) ? 0 : sl->poc_lsb;
        else {
            int max_lsb = 1 << sps->log2_max_poc_lsb, pl = h->prev_tid0_poc & (max_lsb - 1), pm = h->prev_tid0_poc - pl, msb;
            if (sl->poc_lsb < pl && pl - sl->poc_lsb >= max_lsb / 2) msb = pm + max_lsb;
            else if (sl->poc_lsb > pl && sl->poc_lsb - pl > max_lsb / 2) msb = pm - max_lsb;
            else msb = pm;
            poc = msb + sl->poc_lsb;
        }
        if (new_cvs) for (int i = 0; i < s->nslots; i++) s->slot[i].ref = 0;
        /* RPS: keep only the pictures it names (this marking is valid even if we must ask for a resend) */
        if (!is_idr(t)) {
            int keep[MAX_SLOTS] = {0};
            for (int k = 0; k < sl->rps.num_neg + sl->rps.num_pos; k++) {
                int d = k < sl->rps.num_neg ? sl->rps.d0[k] : sl->rps.d1[k - sl->rps.num_neg];
                for (int p = 0; p < s->nslots; p++) if (s->slot[p].ref && s->slot[p].poc == poc + d) keep[p] = 1;
            }
            for (int p = 0; p < s->nslots; p++) if (!keep[p]) s->slot[p].ref = 0;
        }
        int cur = sl_free_slot(s);
        if (cur < 0) { s->bump = 1; rc = PMHW_AGAIN; goto out; }
        /* commit */
        if (new_cvs) { s->epoch++; h->skip_rasl = !is_idr(t); }
        else if (is_irap(t)) h->skip_rasl = 0;
        h->first_pic = 0;
        s->need_key = 0;
        if (sl->tid == 0 && !(t <= 14 && t % 2 == 0) && !(t >= 6 && t <= 9)) h->prev_tid0_poc = poc;
        h->cur_poc = poc;
        h->cur_nal = t;
        s->cur = cur;
        Slot *c = &s->slot[cur];
        c->poc = poc; c->pts = pts; c->corrupt = 0; c->ref = 0;
    } else if (s->cur < 0) {
        goto out;
    }
    {
        /* lists 8.3.4 from the slice's RPS */
        int before[16], after[16], nb = 0, na = 0, missing = 0;
        for (int k = 0; k < sl->rps.num_neg + sl->rps.num_pos; k++) {
            int neg = k < sl->rps.num_neg;
            if (!(neg ? sl->rps.u0[k] : sl->rps.u1[k - sl->rps.num_neg])) continue;
            int target = h->cur_poc + (neg ? sl->rps.d0[k] : sl->rps.d1[k - sl->rps.num_neg]), found = -1;
            for (int p = 0; p < s->nslots; p++) if (s->slot[p].ref && p != s->cur && s->slot[p].poc == target) found = p;
            if (found < 0) { missing = 1; continue; }
            if (neg) before[nb++] = found; else after[na++] = found;
        }
        HevcJob j = { .sps = sps, .pps = pps, .sl = sl, .slots = s->slot, .nslots = s->nslots, .cur = s->cur,
                      .cur_poc = h->cur_poc, .first_slice = sl->first, .ep_unescaped = h->ep, .nal = nal, .nal_len = len };
        int total = nb + na;
        if (sl->type != HEVC_I) {
            if (!total) { s->slot[s->cur].corrupt = 1; rc = PMHW_ERROR; goto out; }
            int tmp[32];
            for (int l = 0; l < (sl->type == HEVC_B ? 2 : 1); l++) {
                int need = sl->num_ref[l] + 1 > total ? sl->num_ref[l] + 1 : total, nt = 0;
                if (need > 32) need = 32;
                while (nt < need) {
                    for (int k = 0; k < (l ? na : nb) && nt < need; k++) tmp[nt++] = l ? after[k] : before[k];
                    for (int k = 0; k < (l ? nb : na) && nt < need; k++) tmp[nt++] = l ? before[k] : after[k];
                }
                int *list = l ? j.l1 : j.l0;
                for (int k = 0; k <= sl->num_ref[l]; k++) {
                    int e = sl->mod[l] ? sl->list_entry[l][k] : k;
                    list[k] = tmp[e < nt ? e : 0];
                }
            }
        }
        if (missing) s->slot[s->cur].corrupt = 1;
        /* entry points: coded as raw byte offsets (emulation prevention bytes included); the engine wants them
         * in unescaped bytes, minus 1 */
        if (sl->num_ep) {
            int data_raw = sl->data_byte, k = 0;
            while (k < s->nepb && s->epb[k] <= data_raw + k) k++;
            int start = data_raw + k;
            for (int q = 0; q < sl->num_ep; q++) {
                int l = sl->ep[q] + 1, end = start + l, in = 0;
                for (int m = 0; m < s->nepb; m++) if (s->epb[m] >= start && s->epb[m] < end) in++;
                h->ep[q] = (uint32_t)(l - in - 1);
                start = end;
            }
        }
        int r = s->hw->hevc_slice(s->hwctx, &j);
        if (r) s->slot[s->cur].corrupt = 1;
        if (r < 0) rc = PMHW_ERROR;
    }
out:
    free(sl);
    return rc;
}

void hevc_end_picture(SL *s)
{
    if (s->cur < 0) return;
    if (s->hw->end_picture && s->hw->end_picture(s->hwctx, s->cur)) s->slot[s->cur].corrupt = 1;
    s->slot[s->cur].ref = 1;     /* "used for short-term reference" until a later RPS drops it */
    sl_queue(s, s->cur);
    s->cur = -1;
}
