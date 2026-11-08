/* SPDX-License-Identifier: GPL-2.0-or-later */
/* hevc_seq: decodes an Annex-B HEVC stream (Main / Main 10, I/P/B, short-term RPS, weighted prediction, WPP) on the
 * Allwinner video engine with no Allwinner userspace code. HEVC counterpart of h264_seq.c.
 *
 * Sources (GPL-2.0, no libcedarc): mainline cedrus_h265.c (register sequence, frame info / ref list / weight SRAM,
 * side buffer sizes), cedrus_regs.h HEVC block (copied in cedrus_regs_h265.h), cedrus_hw.c (output format); vendor
 * 4.9 cedar_ve.{c,h} (ioctls, mmap, IOMMU map, HEVC IRQ path: status 0x538 / ctrl 0x530); 4.9 uapi ion.h.
 * Reference handling per the HEVC spec: POC 8.3.1, RPS 8.3.2 (7.4.8 for inter RPS prediction), lists 8.3.4.
 *
 * Usage: hevc_seq in.hevc out.nv12 > log
 *   Every frame gets "FRAME <decode#> <epoch> <poc> <type> ... adler=<hex>" (FFmpeg framecrc's adler32 of the visible
 *   NV12, Y then UV). out.nv12 gets the first DUMP_FRAMES (default 30) frames in decode order, ALIGN16 layout.
 *   MAX_FRAMES=n stops early. PARSE_ONLY=1: parser only (qemu). EP_RAW=1: entry points without EPB adjustment.
 * 10-bit streams: the engine's primary output is 8-bit NV12 (+ a 2-bit plane we ignore), as in Cedrus.
 * Limits (test only): one slice segment per picture, no tiles, no long-term refs, no scaling lists, 4:2:0.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "cedrus_regs_h265.h"

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* ---- cedar_ve.h (vendor 4.9) ---- */
#define IOCTL_WAIT_VE_DE      0x102
#define IOCTL_RESET_VE        0x104
#define IOCTL_ENABLE_VE       0x105
#define IOCTL_DISABLE_VE      0x106
#define IOCTL_ENGINE_REQ      0x206
#define IOCTL_ENGINE_REL      0x207
#define IOCTL_GET_IOMMU_ADDR  0x502
#define IOCTL_FREE_IOMMU_ADDR 0x503
struct user_iommu_param { int fd; unsigned int iommu_addr; };

/* ---- uapi/ion.h (4.9) ---- */
typedef int ion_user_handle_t;
struct ion_allocation_data { size_t len, align; unsigned int heap_id_mask, flags; ion_user_handle_t handle; };
struct ion_fd_data { ion_user_handle_t handle; int fd; };
struct ion_handle_data { ion_user_handle_t handle; };
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE  _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_MAP   _IOWR('I', 2, struct ion_fd_data)

/* ---- cedrus_regs.h, common part ---- */
#define VE_MODE                         0x00
#define VE_MODE_PIC_WIDTH_IS_4096       BIT(22)
#define VE_MODE_PIC_WIDTH_MORE_2048     BIT(21)
#define VE_MODE_REC_WR_MODE_2MB         (0x01 << 20)
#define VE_MODE_DDR_MODE_BW_128         (0x03 << 16)
#define VE_MODE_DISABLED                (0x07 << 0)
#define VE_MODE_DEC_H265                (0x04 << 0)
#define VE_PRIMARY_CHROMA_BUF_LEN       0xc4
#define VE_PRIMARY_FB_LINE_STRIDE       0xc8
#define VE_PRIMARY_OUT_FMT              0xec
#define VE_PRIMARY_OUT_FMT_NV12         (0x04 << 4)

/* cedrus_h265.c */
#define NEIGHBOR_INFO_BUF_SIZE  (794 * 1024)
#define ENTRY_POINTS_BUF_SIZE   (4 * 1024)
#define MV_COL_BUF_UNIT_CTB     160
#define OUTPUT_PIC_INDEX        16      /* V4L2_HEVC_DPB_ENTRIES_NUM_MAX */

enum { SLICE_B = 0, SLICE_P = 1, SLICE_I = 2 };

static volatile uint8_t *regs;
static void wr(unsigned off, uint32_t v) { *(volatile uint32_t *)(regs + off) = v; }
static uint32_t rd(unsigned off) { return *(volatile uint32_t *)(regs + off); }

/* ---------------- memory: ION buffer + VE IOMMU address ---------------- */
typedef struct { int handle, fd; size_t size; uint8_t *cpu; uint32_t ve; } Buf;
static int ion_fd = -1, ve_fd = -1, heap_mask;

static int ion_try(Buf *b, size_t size, unsigned mask)
{
    memset(b, 0, sizeof *b);
    b->fd = -1;
    b->size = ALIGN(size, 4096);
    struct ion_allocation_data a = { .len = b->size, .align = 4096, .heap_id_mask = mask };
    if (ioctl(ion_fd, ION_IOC_ALLOC, &a) < 0) { b->size = 0; return -1; }
    b->handle = a.handle;
    struct ion_fd_data f = { .handle = a.handle };
    if (ioctl(ion_fd, ION_IOC_MAP, &f) < 0) return -1;
    b->fd = f.fd;
    b->cpu = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (b->cpu == MAP_FAILED) { b->cpu = NULL; return -1; }
    struct user_iommu_param p = { .fd = b->fd };
    if (ioctl(ve_fd, IOCTL_GET_IOMMU_ADDR, &p) < 0) return -1;
    b->ve = p.iommu_addr;
    return 0;
}

static void buf_free(Buf *b)
{
    if (!b->size) return;
    if (b->ve) { struct user_iommu_param p = { .fd = b->fd, .iommu_addr = b->ve }; ioctl(ve_fd, IOCTL_FREE_IOMMU_ADDR, &p); }
    if (b->cpu) munmap(b->cpu, b->size);
    if (b->fd >= 0) close(b->fd);
    struct ion_handle_data h = { .handle = b->handle };
    ioctl(ion_fd, ION_IOC_FREE, &h);
    b->size = 0;
}

static int buf_alloc(Buf *b, size_t size)
{
    if (!heap_mask) {
        for (int id = 0; id < 16 && !heap_mask; id++) {
            Buf t;
            if (ion_try(&t, 4096, 1u << id) == 0) heap_mask = 1u << id;
            buf_free(&t);
        }
        if (!heap_mask) { printf("no ION heap maps for the VE\n"); return -1; }
        printf("ION heap mask 0x%x\n", heap_mask);
    }
    if (ion_try(b, size, heap_mask)) { printf("buffer of %zu bytes failed: %s\n", size, strerror(errno)); return -1; }
    memset(b->cpu, 0, b->size);
    return 0;
}

/* ---------------- bit reader over RBSP ---------------- */
typedef struct { const uint8_t *d; int n, pos; } Bits;
static unsigned u(Bits *b, int k)
{
    unsigned v = 0;
    while (k--) { v = v << 1 | (b->pos < b->n * 8 ? b->d[b->pos >> 3] >> (7 - (b->pos & 7)) & 1 : 0); b->pos++; }
    return v;
}
static unsigned ue(Bits *b) { int z = 0; while (!u(b, 1) && z < 32) z++; return (1u << z) - 1 + u(b, z); }
static int se(Bits *b) { unsigned k = ue(b); return k & 1 ? (int)(k + 1) / 2 : -(int)(k / 2); }

static int epb_pos[1 << 16], n_epb;    /* raw byte positions of removed emulation prevention bytes */
static int unescape(const uint8_t *s, int n, uint8_t *d)
{
    int o = 0, zeros = 0;
    n_epb = 0;
    for (int i = 0; i < n; i++) {
        if (zeros >= 2 && s[i] == 3) { zeros = 0; if (n_epb < (1 << 16)) epb_pos[n_epb++] = i; continue; }
        zeros = s[i] == 0 ? zeros + 1 : 0;
        d[o++] = s[i];
    }
    return o;
}
static int ceil_log2(int v) { int r = 0; while ((1 << r) < v) r++; return r; }

/* ---------------- parameter sets ---------------- */
typedef struct { int num_neg, num_pos, d0[16], d1[16], u0[16], u1[16]; } Rps;
typedef struct {
    int ok, chroma_format_idc, width, height, conf_l, conf_r, conf_t, conf_b, bit_depth, bit_depth_c;
    int log2_max_poc_lsb, log2_min_cb, log2_diff_cb, log2_min_tb, log2_diff_tb, depth_inter, depth_intra;
    int amp, sao, pcm, pcm_bd, pcm_bd_c, log2_min_pcm, log2_diff_pcm, pcm_lf_disabled;
    int num_rps; Rps rps[65];
    int temporal_mvp, strong_intra;
} Sps;
typedef struct {
    int ok, dependent_slices, output_flag, extra_bits, sign_hiding, cabac_init_present, num_ref_l0, num_ref_l1;
    int init_qp, constrained_intra, transform_skip, cu_qp_delta, diff_cu_qp_delta_depth, cb_qp, cr_qp;
    int slice_chroma_qp_offsets, weighted_pred, weighted_bipred, transquant_bypass, tiles, wpp, lf_across_slices;
    int deblock_override_enabled, deblock_disabled, beta, tc, lists_mod, log2_pml, slice_ext;
} Pps;
static Sps sps;
static Pps pps;

static void parse_ptl(Bits *b, int max_sub)
{
    u(b, 8); u(b, 32); u(b, 32); u(b, 16); u(b, 8);   /* general profile (88 bits) + general_level_idc */
    int pp[8] = {0}, lp[8] = {0};
    for (int i = 0; i < max_sub; i++) { pp[i] = u(b, 1); lp[i] = u(b, 1); }
    if (max_sub > 0) for (int i = max_sub; i < 8; i++) u(b, 2);
    for (int i = 0; i < max_sub; i++) {
        if (pp[i]) { u(b, 32); u(b, 32); u(b, 24); }
        if (lp[i]) u(b, 8);
    }
}

/* 7.3.7 / 7.4.8; idx == num for the slice header's own set */
static int parse_rps(Bits *b, Rps *sets, int idx, int num, Rps *out)
{
    memset(out, 0, sizeof *out);
    int inter = idx ? u(b, 1) : 0;
    if (inter) {
        int delta_idx = idx == num ? ue(b) + 1 : 1;
        int sign = u(b, 1), abs_d = ue(b) + 1, delta = (1 - 2 * sign) * abs_d;
        if (delta_idx > idx) return -1;
        const Rps *r = &sets[idx - delta_idx];
        int n = r->num_neg + r->num_pos, used[33], use_delta[33];
        for (int j = 0; j <= n; j++) {
            used[j] = u(b, 1);
            use_delta[j] = used[j] ? 1 : u(b, 1);
        }
        int i = 0;
        for (int j = r->num_pos - 1; j >= 0; j--) {
            int d = r->d1[j] + delta;
            if (d < 0 && use_delta[r->num_neg + j]) { out->d0[i] = d; out->u0[i++] = used[r->num_neg + j]; }
        }
        if (delta < 0 && use_delta[n]) { out->d0[i] = delta; out->u0[i++] = used[n]; }
        for (int j = 0; j < r->num_neg; j++) {
            int d = r->d0[j] + delta;
            if (d < 0 && use_delta[j]) { out->d0[i] = d; out->u0[i++] = used[j]; }
        }
        out->num_neg = i;
        i = 0;
        for (int j = r->num_neg - 1; j >= 0; j--) {
            int d = r->d0[j] + delta;
            if (d > 0 && use_delta[j]) { out->d1[i] = d; out->u1[i++] = used[j]; }
        }
        if (delta > 0 && use_delta[n]) { out->d1[i] = delta; out->u1[i++] = used[n]; }
        for (int j = 0; j < r->num_pos; j++) {
            int d = r->d1[j] + delta;
            if (d > 0 && use_delta[r->num_neg + j]) { out->d1[i] = d; out->u1[i++] = used[r->num_neg + j]; }
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

static int parse_sps(Bits *b, Sps *s)
{
    memset(s, 0, sizeof *s);
    u(b, 4);
    int max_sub = u(b, 3);
    u(b, 1);
    parse_ptl(b, max_sub);
    ue(b);
    s->chroma_format_idc = ue(b);
    if (s->chroma_format_idc != 1) { printf("only 4:2:0 handled\n"); return -1; }
    s->width = ue(b); s->height = ue(b);
    if (u(b, 1)) { s->conf_l = ue(b); s->conf_r = ue(b); s->conf_t = ue(b); s->conf_b = ue(b); }
    s->bit_depth = ue(b) + 8; s->bit_depth_c = ue(b) + 8;
    s->log2_max_poc_lsb = ue(b) + 4;
    int ordering = u(b, 1);
    for (int i = ordering ? 0 : max_sub; i <= max_sub; i++) { ue(b); ue(b); ue(b); }
    s->log2_min_cb = ue(b) + 3; s->log2_diff_cb = ue(b);
    s->log2_min_tb = ue(b) + 2; s->log2_diff_tb = ue(b);
    s->depth_inter = ue(b); s->depth_intra = ue(b);
    if (u(b, 1)) { printf("scaling lists not handled\n"); return -1; }
    s->amp = u(b, 1); s->sao = u(b, 1);
    if ((s->pcm = u(b, 1))) {
        s->pcm_bd = u(b, 4) + 1; s->pcm_bd_c = u(b, 4) + 1;
        s->log2_min_pcm = ue(b) + 3; s->log2_diff_pcm = ue(b); s->pcm_lf_disabled = u(b, 1);
    }
    s->num_rps = ue(b);
    if (s->num_rps > 64) return -1;
    for (int i = 0; i < s->num_rps; i++) if (parse_rps(b, s->rps, i, s->num_rps, &s->rps[i])) { printf("bad SPS RPS\n"); return -1; }
    if (u(b, 1)) { printf("long-term references not handled\n"); return -1; }
    s->temporal_mvp = u(b, 1); s->strong_intra = u(b, 1);
    s->ok = 1;
    return 0;
}

static int parse_pps(Bits *b, Pps *p)
{
    memset(p, 0, sizeof *p);
    ue(b); ue(b);
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
    if (p->tiles) { printf("tiles not handled\n"); return -1; }
    p->lf_across_slices = u(b, 1);
    if (u(b, 1)) {
        p->deblock_override_enabled = u(b, 1);
        if (!(p->deblock_disabled = u(b, 1))) { p->beta = se(b); p->tc = se(b); }
    }
    if (u(b, 1)) { printf("PPS scaling lists not handled\n"); return -1; }
    p->lists_mod = u(b, 1);
    p->log2_pml = ue(b);
    p->slice_ext = u(b, 1);
    p->ok = 1;
    return 0;
}

/* ---------------- slice segment header ---------------- */
typedef struct {
    int nal_type, tid, type, poc_lsb, sao_luma, sao_chroma, temporal_mvp, num_ref[2], mvd_l1_zero, cabac_init;
    int col_from_l0, col_ref_idx, max_merge, qp_delta, cb_qp, cr_qp, deblock_disabled, beta, tc, lf_across;
    int mod[2], list_entry[2][16];
    Rps rps;
    int luma_denom, chroma_denom;
    int8_t dlw[2][16], lo[2][16], dcw[2][16][2], co[2][16][2];
    int num_ep, ep[1024];
    int header_bits, data_byte;       /* RBSP bits before byte_alignment(); RBSP byte where slice data starts */
} Slice;

static int is_irap(int t) { return t >= 16 && t <= 23; }
static int is_idr(int t) { return t == 19 || t == 20; }

static int num_pic_total_curr(const Rps *r)
{
    int n = 0;
    for (int i = 0; i < r->num_neg; i++) n += r->u0[i];
    for (int i = 0; i < r->num_pos; i++) n += r->u1[i];
    return n;
}

static int parse_slice(Bits *b, Slice *sl)
{
    memset(sl, 0, sizeof *sl);
    u(b, 1); sl->nal_type = u(b, 6); u(b, 6); sl->tid = u(b, 3) - 1;
    if (!u(b, 1)) { printf("multiple slice segments per picture not handled\n"); return -1; }
    if (is_irap(sl->nal_type)) u(b, 1);
    ue(b);
    u(b, pps.extra_bits);
    sl->type = ue(b);
    if (pps.output_flag) u(b, 1);
    if (!is_idr(sl->nal_type)) {
        sl->poc_lsb = u(b, sps.log2_max_poc_lsb);
        if (!u(b, 1)) {
            if (parse_rps(b, sps.rps, sps.num_rps, sps.num_rps, &sl->rps)) { printf("bad slice RPS\n"); return -1; }
        } else {
            int idx = sps.num_rps > 1 ? u(b, ceil_log2(sps.num_rps)) : 0;
            sl->rps = sps.rps[idx];
        }
        if (sps.temporal_mvp) sl->temporal_mvp = u(b, 1);
    }
    if (sps.sao) { sl->sao_luma = u(b, 1); sl->sao_chroma = u(b, 1); }
    sl->col_from_l0 = 1;
    if (sl->type != SLICE_I) {
        sl->num_ref[0] = pps.num_ref_l0; sl->num_ref[1] = pps.num_ref_l1;
        if (u(b, 1)) { sl->num_ref[0] = ue(b); if (sl->type == SLICE_B) sl->num_ref[1] = ue(b); }
        if (sl->num_ref[0] > 15 || sl->num_ref[1] > 15) return -1;
        int total = num_pic_total_curr(&sl->rps);
        if (pps.lists_mod && total > 1) {
            for (int l = 0; l < (sl->type == SLICE_B ? 2 : 1); l++)
                if ((sl->mod[l] = u(b, 1)))
                    for (int i = 0; i <= sl->num_ref[l]; i++) sl->list_entry[l][i] = u(b, ceil_log2(total));
        }
        if (sl->type == SLICE_B) sl->mvd_l1_zero = u(b, 1);
        if (pps.cabac_init_present) sl->cabac_init = u(b, 1);
        if (sl->temporal_mvp) {
            if (sl->type == SLICE_B) sl->col_from_l0 = u(b, 1);
            if ((sl->col_from_l0 && sl->num_ref[0] > 0) || (!sl->col_from_l0 && sl->num_ref[1] > 0)) sl->col_ref_idx = ue(b);
        }
        if ((pps.weighted_pred && sl->type == SLICE_P) || (pps.weighted_bipred && sl->type == SLICE_B)) {
            sl->luma_denom = ue(b);
            sl->chroma_denom = sl->luma_denom + se(b);
            for (int l = 0; l < (sl->type == SLICE_B ? 2 : 1); l++) {
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
    if (pps.slice_chroma_qp_offsets) { sl->cb_qp = se(b); sl->cr_qp = se(b); }
    sl->deblock_disabled = pps.deblock_disabled; sl->beta = pps.beta; sl->tc = pps.tc;
    if (pps.deblock_override_enabled && u(b, 1)) {
        if (!(sl->deblock_disabled = u(b, 1))) { sl->beta = se(b); sl->tc = se(b); }
    }
    sl->lf_across = pps.lf_across_slices;
    if (pps.lf_across_slices && (sl->sao_luma || sl->sao_chroma || !sl->deblock_disabled)) sl->lf_across = u(b, 1);
    if (pps.tiles || pps.wpp) {
        sl->num_ep = ue(b);
        if (sl->num_ep > 1024) { printf("too many entry points\n"); return -1; }
        if (sl->num_ep) {
            int len = ue(b) + 1;
            for (int i = 0; i < sl->num_ep; i++) sl->ep[i] = u(b, len);   /* entry_point_offset_minus1 */
        }
    }
    if (pps.slice_ext) { int n = ue(b); while (n--) u(b, 8); }
    sl->header_bits = b->pos;
    sl->data_byte = (b->pos + 8) / 8;    /* byte_alignment(): a 1 bit, then zeros to the byte boundary */
    return 0;
}

/* ---------------- picture buffers + DPB ---------------- */
typedef struct { Buf mem; uint32_t luma, chroma, mv; int ref, poc; } Pic;
#define MAX_PICS 16
static Pic pics[MAX_PICS];
static int npics;

static uint32_t adler(const uint8_t *p, int n, uint32_t s)
{
    uint32_t a = s & 0xffff, c = s >> 16;
    for (int i = 0; i < n; i++) { a = (a + p[i]) % 65521; c = (c + a) % 65521; }
    return c << 16 | a;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static Buf bs, neigh, epbuf;
static int W, H, stride, coded_h, nv12_bytes, bit2_bytes, ctb_log2;

static void frame_info(int index, const Pic *p, int poc)
{
    uint32_t fi[8] = { (uint32_t)poc, (uint32_t)poc, p->mv >> 8, p->mv >> 8, p->luma >> 8, p->chroma >> 8, 0, 0 };
    wr(VE_DEC_H265_SRAM_OFFSET, VE_DEC_H265_SRAM_OFFSET_FRAME_INFO + VE_DEC_H265_SRAM_OFFSET_FRAME_INFO_UNIT * index);
    for (int i = 0; i < 6; i++) wr(VE_DEC_H265_SRAM_DATA, fi[i]);
}

static void ref_list_write(const int *list, int n, uint32_t off)
{
    uint32_t word = 0;
    wr(VE_DEC_H265_SRAM_OFFSET, off);
    for (int i = 0; i < n; i++) {
        word |= (uint32_t)(list[i] & 0x7f) << (i % 4) * 8;
        if (i % 4 == 3 || i == n - 1) { wr(VE_DEC_H265_SRAM_DATA, word); word = 0; }
    }
}

static void weights_write(const int8_t *dlw, const int8_t *lo, const int8_t (*dcw)[2], const int8_t (*co)[2], int n,
                          uint32_t lum, uint32_t chr)
{
    wr(VE_DEC_H265_SRAM_OFFSET, lum);
    uint8_t pw[4] = {0};
    for (int i = 0; i < n; i++) {
        pw[(i % 2) * 2] = dlw[i]; pw[(i % 2) * 2 + 1] = lo[i];
        if (i % 2 == 1 || i == n - 1) { uint32_t w; memcpy(&w, pw, 4); wr(VE_DEC_H265_SRAM_DATA, w); memset(pw, 0, 4); }
    }
    wr(VE_DEC_H265_SRAM_OFFSET, chr);
    for (int i = 0; i < n; i++) {
        uint8_t c[4] = { (uint8_t)dcw[i][0], (uint8_t)co[i][0], (uint8_t)dcw[i][1], (uint8_t)co[i][1] };
        uint32_t w; memcpy(&w, c, 4); wr(VE_DEC_H265_SRAM_DATA, w);
    }
}

static int decode(const Slice *sl, int cur, int poc, int slice_len, const int *l0, const int *l1, double *ve_ms)
{
    __sync_synchronize();
    double t0 = now();
    uint32_t mode = VE_MODE_REC_WR_MODE_2MB | VE_MODE_DDR_MODE_BW_128 | VE_MODE_DEC_H265;
    if (W == 4096) mode |= VE_MODE_PIC_WIDTH_IS_4096;
    if (W > 2048) mode |= VE_MODE_PIC_WIDTH_MORE_2048;
    wr(VE_MODE, mode);
    wr(VE_PRIMARY_OUT_FMT, VE_PRIMARY_OUT_FMT_NV12);
    wr(VE_PRIMARY_CHROMA_BUF_LEN, stride * coded_h / 4);
    wr(VE_PRIMARY_FB_LINE_STRIDE, (stride / 2) << 16 | stride);

    wr(VE_DEC_H265_BITS_OFFSET, 0);
    wr(VE_DEC_H265_BITS_LEN, slice_len * 8);
    wr(VE_DEC_H265_BITS_ADDR, VE_DEC_H265_BITS_ADDR_BASE(bs.ve) | VE_DEC_H265_BITS_ADDR_VALID_SLICE_DATA |
                              VE_DEC_H265_BITS_ADDR_LAST_SLICE_DATA | VE_DEC_H265_BITS_ADDR_FIRST_SLICE_DATA);
    wr(VE_DEC_H265_BITS_END_ADDR, VE_DEC_H265_BITS_END_ADDR_BASE(bs.ve + slice_len));
    wr(VE_DEC_H265_DEC_CTB_ADDR, 0);
    wr(VE_DEC_H265_TILE_START_CTB, 0);
    wr(VE_DEC_H265_TILE_END_CTB, 0);
    wr(VE_DEC_H265_DEC_CTB_NUM, 0);
    wr(VE_DEC_H265_TRIGGER, VE_DEC_H265_TRIGGER_INIT_SWDEC);
    /* to the end of the slice header (before byte_alignment), as Cedrus' skip + show_bits ends up */
    for (int count = 0; count < sl->header_bits;) {
        int n = sl->header_bits - count < 32 ? sl->header_bits - count : 32;
        wr(VE_DEC_H265_TRIGGER, VE_DEC_H265_TRIGGER_FLUSH_BITS | VE_DEC_H265_TRIGGER_TYPE_N_BITS(n));
        for (int spin = 0; rd(VE_DEC_H265_STATUS) & VE_DEC_H265_STATUS_VLD_BUSY; spin++)
            if (spin > 1000000) { printf("VLD busy timeout during bit skip\n"); return -1; }
        count += n;
    }
    wr(VE_DEC_H265_DEC_NAL_HDR, VE_DEC_H265_DEC_NAL_HDR_NAL_UNIT_TYPE(sl->nal_type) |
                                VE_DEC_H265_DEC_NAL_HDR_NUH_TEMPORAL_ID_PLUS1(sl->tid + 1));
    uint32_t reg = VE_DEC_H265_DEC_SPS_HDR_MAX_TRANSFORM_HIERARCHY_DEPTH_INTRA(sps.depth_intra) |
        VE_DEC_H265_DEC_SPS_HDR_MAX_TRANSFORM_HIERARCHY_DEPTH_INTER(sps.depth_inter) |
        VE_DEC_H265_DEC_SPS_HDR_LOG2_DIFF_MAX_MIN_TRANSFORM_BLOCK_SIZE(sps.log2_diff_tb) |
        VE_DEC_H265_DEC_SPS_HDR_LOG2_MIN_TRANSFORM_BLOCK_SIZE_MINUS2(sps.log2_min_tb - 2) |
        VE_DEC_H265_DEC_SPS_HDR_LOG2_DIFF_MAX_MIN_LUMA_CODING_BLOCK_SIZE(sps.log2_diff_cb) |
        VE_DEC_H265_DEC_SPS_HDR_LOG2_MIN_LUMA_CODING_BLOCK_SIZE_MINUS3(sps.log2_min_cb - 3) |
        VE_DEC_H265_DEC_SPS_HDR_BIT_DEPTH_CHROMA_MINUS8(sps.bit_depth_c - 8) |
        VE_DEC_H265_DEC_SPS_HDR_BIT_DEPTH_LUMA_MINUS8(sps.bit_depth - 8) |
        VE_DEC_H265_DEC_SPS_HDR_CHROMA_FORMAT_IDC(sps.chroma_format_idc);
    if (sps.strong_intra) reg |= VE_DEC_H265_DEC_SPS_HDR_FLAG_STRONG_INTRA_SMOOTHING_ENABLE;
    if (sps.temporal_mvp) reg |= VE_DEC_H265_DEC_SPS_HDR_FLAG_SPS_TEMPORAL_MVP_ENABLED;
    if (sps.sao) reg |= VE_DEC_H265_DEC_SPS_HDR_FLAG_SAMPLE_ADAPTIVE_OFFSET_ENABLED;
    if (sps.amp) reg |= VE_DEC_H265_DEC_SPS_HDR_FLAG_AMP_ENABLED;
    wr(VE_DEC_H265_DEC_SPS_HDR, reg);
    reg = 0;
    if (sps.pcm)
        reg = VE_DEC_H265_DEC_PCM_CTRL_LOG2_DIFF_MAX_MIN_PCM_LUMA_CODING_BLOCK_SIZE(sps.log2_diff_pcm) |
              VE_DEC_H265_DEC_PCM_CTRL_LOG2_MIN_PCM_LUMA_CODING_BLOCK_SIZE_MINUS3(sps.log2_min_pcm - 3) |
              VE_DEC_H265_DEC_PCM_CTRL_PCM_SAMPLE_BIT_DEPTH_CHROMA_MINUS1(sps.pcm_bd_c - 1) |
              VE_DEC_H265_DEC_PCM_CTRL_PCM_SAMPLE_BIT_DEPTH_LUMA_MINUS1(sps.pcm_bd - 1) |
              VE_DEC_H265_DEC_PCM_CTRL_FLAG_PCM_ENABLED |
              (sps.pcm_lf_disabled ? VE_DEC_H265_DEC_PCM_CTRL_FLAG_PCM_LOOP_FILTER_DISABLED : 0);
    wr(VE_DEC_H265_DEC_PCM_CTRL, reg);
    reg = VE_DEC_H265_DEC_PPS_CTRL0_PPS_CR_QP_OFFSET(pps.cr_qp) | VE_DEC_H265_DEC_PPS_CTRL0_PPS_CB_QP_OFFSET(pps.cb_qp) |
          VE_DEC_H265_DEC_PPS_CTRL0_INIT_QP_MINUS26(pps.init_qp) |
          VE_DEC_H265_DEC_PPS_CTRL0_DIFF_CU_QP_DELTA_DEPTH(pps.diff_cu_qp_delta_depth);
    if (pps.cu_qp_delta) reg |= VE_DEC_H265_DEC_PPS_CTRL0_FLAG_CU_QP_DELTA_ENABLED;
    if (pps.transform_skip) reg |= VE_DEC_H265_DEC_PPS_CTRL0_FLAG_TRANSFORM_SKIP_ENABLED;
    if (pps.constrained_intra) reg |= VE_DEC_H265_DEC_PPS_CTRL0_FLAG_CONSTRAINED_INTRA_PRED;
    if (pps.sign_hiding) reg |= VE_DEC_H265_DEC_PPS_CTRL0_FLAG_SIGN_DATA_HIDING_ENABLED;
    wr(VE_DEC_H265_DEC_PPS_CTRL0, reg);
    reg = VE_DEC_H265_DEC_PPS_CTRL1_LOG2_PARALLEL_MERGE_LEVEL_MINUS2(pps.log2_pml);
    if (pps.lf_across_slices) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED;
    if (pps.wpp) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_ENTROPY_CODING_SYNC_ENABLED;
    if (pps.transquant_bypass) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_TRANSQUANT_BYPASS_ENABLED;
    if (pps.weighted_bipred) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_WEIGHTED_BIPRED;
    if (pps.weighted_pred) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_WEIGHTED_PRED;
    wr(VE_DEC_H265_DEC_PPS_CTRL1, reg);

    reg = VE_DEC_H265_DEC_SLICE_HDR_INFO0_PICTURE_TYPE(0) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_FIVE_MINUS_MAX_NUM_MERGE_CAND(sl->max_merge) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_NUM_REF_IDX_L1_ACTIVE_MINUS1(sl->num_ref[1]) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_NUM_REF_IDX_L0_ACTIVE_MINUS1(sl->num_ref[0]) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_COLLOCATED_REF_IDX(sl->col_ref_idx) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_SLICE_TYPE(sl->type) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_FIRST_SLICE_SEGMENT_IN_PIC;
    if (sl->col_from_l0) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_COLLOCATED_FROM_L0;
    if (sl->cabac_init) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_CABAC_INIT;
    if (sl->mvd_l1_zero) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_MVD_L1_ZERO;
    if (sl->sao_chroma) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_SLICE_SAO_CHROMA;
    if (sl->sao_luma) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_SLICE_SAO_LUMA;
    if (sl->temporal_mvp) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_SLICE_TEMPORAL_MVP_ENABLE;
    wr(VE_DEC_H265_DEC_SLICE_HDR_INFO0, reg);
    reg = VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_TC_OFFSET_DIV2(sl->tc) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_BETA_OFFSET_DIV2(sl->beta) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_CR_QP_OFFSET(sl->cr_qp) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_CB_QP_OFFSET(sl->cb_qp) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_QP_DELTA(sl->qp_delta);
    if (sl->deblock_disabled) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO1_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED;
    if (sl->lf_across) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO1_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED;
    if (sl->type != SLICE_I) {
        /* cedrus_h265_is_low_delay(): 1 when a reference is AFTER the current picture; the flag is set when it is 0 */
        int future = 0;
        for (int i = 0; i <= sl->num_ref[0]; i++) if (pics[l0[i]].poc > poc) future = 1;
        if (sl->type == SLICE_B) for (int i = 0; i <= sl->num_ref[1]; i++) if (pics[l1[i]].poc > poc) future = 1;
        if (!future) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO1_FLAG_SLICE_NOT_LOW_DELAY;
    }
    wr(VE_DEC_H265_DEC_SLICE_HDR_INFO1, reg);
    wr(VE_DEC_H265_DEC_SLICE_HDR_INFO2, VE_DEC_H265_DEC_SLICE_HDR_INFO2_NUM_ENTRY_POINT_OFFSETS(sl->num_ep) |
       VE_DEC_H265_DEC_SLICE_HDR_INFO2_CHROMA_LOG2_WEIGHT_DENOM(sl->chroma_denom) |
       VE_DEC_H265_DEC_SLICE_HDR_INFO2_LUMA_LOG2_WEIGHT_DENOM(sl->luma_denom));
    wr(VE_DEC_H265_ENTRY_POINT_OFFSET_ADDR, epbuf.ve >> 8);
    wr(VE_DEC_H265_DEC_PIC_SIZE, VE_DEC_H265_DEC_PIC_SIZE_WIDTH(W) | VE_DEC_H265_DEC_PIC_SIZE_HEIGHT(H));
    wr(VE_DEC_H265_SCALING_LIST_CTRL0, VE_DEC_H265_SCALING_LIST_CTRL0_DEFAULT);
    wr(VE_DEC_H265_NEIGHBOR_INFO_ADDR, VE_DEC_H265_NEIGHBOR_INFO_ADDR_BASE(neigh.ve));
    for (int i = 0; i < npics; i++) if (pics[i].ref && i != cur) frame_info(i, &pics[i], pics[i].poc);
    frame_info(OUTPUT_PIC_INDEX, &pics[cur], poc);
    wr(VE_DEC_H265_OUTPUT_FRAME_IDX, OUTPUT_PIC_INDEX);
    if (sl->type != SLICE_I) {
        ref_list_write(l0, sl->num_ref[0] + 1, VE_DEC_H265_SRAM_OFFSET_REF_PIC_LIST0);
        if (pps.weighted_pred || pps.weighted_bipred)
            weights_write(sl->dlw[0], sl->lo[0], sl->dcw[0], sl->co[0], sl->num_ref[0] + 1,
                          VE_DEC_H265_SRAM_OFFSET_PRED_WEIGHT_LUMA_L0, VE_DEC_H265_SRAM_OFFSET_PRED_WEIGHT_CHROMA_L0);
    }
    if (sl->type == SLICE_B) {
        ref_list_write(l1, sl->num_ref[1] + 1, VE_DEC_H265_SRAM_OFFSET_REF_PIC_LIST1);
        if (pps.weighted_bipred)
            weights_write(sl->dlw[1], sl->lo[1], sl->dcw[1], sl->co[1], sl->num_ref[1] + 1,
                          VE_DEC_H265_SRAM_OFFSET_PRED_WEIGHT_LUMA_L1, VE_DEC_H265_SRAM_OFFSET_PRED_WEIGHT_CHROMA_L1);
    }
    if (sps.bit_depth > 8) {
        wr(VE_DEC_H265_OFFSET_ADDR_FIRST_OUT, nv12_bytes);
        wr(VE_DEC_H265_10BIT_CONFIGURE, VE_DEC_H265_10BIT_CONFIGURE_FIRST_2BIT_STRIDE(ALIGN(stride / 4, 32)));
    }
    wr(VE_DEC_H265_STATUS, VE_DEC_H265_STATUS_CHECK_MASK);
    wr(VE_DEC_H265_CTRL, VE_DEC_H265_CTRL_IRQ_MASK);
    wr(VE_DEC_H265_TRIGGER, VE_DEC_H265_TRIGGER_DEC_SLICE);
    int irq = ioctl(ve_fd, IOCTL_WAIT_VE_DE, 2);
    *ve_ms = (now() - t0) * 1e3;
    uint32_t st = rd(VE_DEC_H265_STATUS), ctbs = rd(VE_DEC_H265_DEC_CTB_NUM);
    wr(VE_DEC_H265_STATUS, VE_DEC_H265_STATUS_CHECK_MASK);
    wr(VE_DEC_H265_CTRL, rd(VE_DEC_H265_CTRL) & ~VE_DEC_H265_CTRL_IRQ_MASK);
    if (irq != 1 || !(st & VE_DEC_H265_STATUS_SUCCESS) || (st & VE_DEC_H265_STATUS_CHECK_ERROR)) {
        printf("  engine: irq %d status 0x%08x CTBs %u\n", irq, st, ctbs);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.hevc out.nv12\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int parse_only = getenv("PARSE_ONLY") != NULL, ep_raw = getenv("EP_RAW") != NULL;
    int dump = getenv("DUMP_FRAMES") ? atoi(getenv("DUMP_FRAMES")) : 30;
    int max_frames = getenv("MAX_FRAMES") ? atoi(getenv("MAX_FRAMES")) : 1 << 30;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long es_len = ftell(f);
    rewind(f);
    uint8_t *es = malloc(es_len), *rbsp = malloc(4 << 20);
    if (!es || !rbsp || fread(es, 1, es_len, f) != (size_t)es_len) { printf("read failed\n"); return 1; }
    fclose(f);
    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    if (!parse_only) {
        ve_fd = open("/dev/cedar_dev", O_RDWR);
        ion_fd = open("/dev/ion", O_RDWR);
        if (ve_fd < 0 || ion_fd < 0) { printf("open /dev/cedar_dev or /dev/ion: %s\n", strerror(errno)); return 1; }
        if (ioctl(ve_fd, IOCTL_ENGINE_REQ, 0) < 0) { printf("ENGINE_REQ: %s\n", strerror(errno)); return 1; }
        ioctl(ve_fd, IOCTL_ENABLE_VE, 0);
        ioctl(ve_fd, IOCTL_RESET_VE, 0);
        regs = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, ve_fd, 0);
        if (regs == MAP_FAILED) { printf("mmap regs: %s\n", strerror(errno)); return 1; }
        printf("VE id e0=0x%08x e4=0x%08x\n", rd(0xe0), rd(0xe4));
    }

    int rc = 0, frames = 0, failed = 0, epoch = -1, vis_w = 0, vis_h = 0;
    int prev_tid0_poc = 0, skip_rasl = 0;
    double ve_total = 0, ve_max = 0, wall_total = 0;
    uint8_t *row = NULL;
    for (long i = 0; i + 3 < es_len && frames < max_frames;) {
        if (!(es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1)) { i++; continue; }
        long s = i + 3, e = s;
        while (e + 3 < es_len && !(es[e] == 0 && es[e + 1] == 0 && (es[e + 2] == 1 || (es[e + 2] == 0 && es[e + 3] == 1)))) e++;
        if (e + 3 >= es_len) e = es_len;
        i = e;
        int t = es[s] >> 1 & 0x3f, n = e - s;
        if (n > (4 << 20)) { printf("NAL too large\n"); rc = 1; break; }
        if (t != 33 && t != 34 && !(t <= 9) && !(t >= 16 && t <= 21)) continue;
        double w0 = now();
        Bits b = { rbsp, unescape(es + s, n, rbsp), 16 };
        if (t == 33) {
            if (parse_sps(&b, &sps)) { rc = 1; break; }
            if (!npics) {
                W = sps.width; H = sps.height;
                vis_w = W - 2 * (sps.conf_l + sps.conf_r); vis_h = H - 2 * (sps.conf_t + sps.conf_b);
                stride = ALIGN(W, 16); coded_h = ALIGN(H, 16);
                nv12_bytes = stride * coded_h * 3 / 2;
                bit2_bytes = sps.bit_depth > 8 ? ALIGN(stride / 4, 32) * coded_h * 3 / 2 : 0;
                ctb_log2 = sps.log2_min_cb + sps.log2_diff_cb;
                int ctb = 1 << ctb_log2;
                int mv_size = ((W + ctb - 1) / ctb) * ((H + ctb - 1) / ctb) * MV_COL_BUF_UNIT_CTB + 1024;
                npics = MAX_PICS;
                printf("stream: %dx%d (visible %dx%d), %d-bit, CTB %d, %s, %d RPS in SPS, %s\n", W, H, vis_w, vis_h,
                       sps.bit_depth, ctb, sps.temporal_mvp ? "TMVP" : "no TMVP", sps.num_rps, sps.sao ? "SAO" : "no SAO");
                if (!parse_only) {
                    if (buf_alloc(&bs, 4 << 20) || buf_alloc(&neigh, NEIGHBOR_INFO_BUF_SIZE) || buf_alloc(&epbuf, ENTRY_POINTS_BUF_SIZE)) { rc = 1; break; }
                    for (int p = 0; p < npics; p++) {
                        if (buf_alloc(&pics[p].mem, nv12_bytes + bit2_bytes + ALIGN(mv_size, 1024) + 1024)) { rc = 1; break; }
                        pics[p].luma = pics[p].mem.ve; pics[p].chroma = pics[p].mem.ve + stride * coded_h;
                        pics[p].mv = ALIGN(pics[p].mem.ve + nv12_bytes + bit2_bytes, 1024);
                    }
                    if (rc) break;
                }
                row = malloc(vis_w);
            }
            continue;
        }
        if (t == 34) { if (parse_pps(&b, &pps)) { rc = 1; break; } continue; }
        if (!sps.ok || !pps.ok) continue;
        b.pos = 0;
        Slice sl;
        if (parse_slice(&b, &sl)) { rc = 1; break; }
        /* RASL pictures are only undecodable after an IRAP that starts the sequence (NoRaslOutputFlag) */
        if (is_irap(t)) skip_rasl = !is_idr(t) && frames == 0;
        if ((t == 8 || t == 9) && skip_rasl) { printf("RASL picture skipped\n"); continue; }

        /* POC 8.3.1: every IRAP here starts a new coded video sequence (IDR, or CRA at the start) */
        int poc;
        if (is_irap(t) && (is_idr(t) || frames == 0)) {
            epoch++;
            poc = is_idr(t) ? 0 : sl.poc_lsb;
            for (int p = 0; p < npics; p++) pics[p].ref = 0;
        } else {
            int max_lsb = 1 << sps.log2_max_poc_lsb, prev_lsb = prev_tid0_poc & (max_lsb - 1), prev_msb = prev_tid0_poc - prev_lsb, msb;
            if (sl.poc_lsb < prev_lsb && prev_lsb - sl.poc_lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
            else if (sl.poc_lsb > prev_lsb && sl.poc_lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
            else msb = prev_msb;
            poc = msb + sl.poc_lsb;
        }
        if (sl.tid == 0 && !(t <= 14 && (t % 2 == 0)) && !(t >= 6 && t <= 9)) prev_tid0_poc = poc;

        /* RPS 8.3.2: keep pictures named in the set, drop the rest */
        int before[16], after[16], nb = 0, na = 0;
        if (!is_idr(t)) {
            int keep[MAX_PICS] = {0};
            for (int k = 0; k < sl.rps.num_neg + sl.rps.num_pos; k++) {
                int neg = k < sl.rps.num_neg, d = neg ? sl.rps.d0[k] : sl.rps.d1[k - sl.rps.num_neg];
                int used = neg ? sl.rps.u0[k] : sl.rps.u1[k - sl.rps.num_neg], found = -1;
                for (int p = 0; p < npics; p++) if (pics[p].ref && pics[p].poc == poc + d) found = p;
                if (found < 0) { if (used) { printf("missing reference POC %d for POC %d\n", poc + d, poc); rc = 1; } continue; }
                keep[found] = 1;
                if (used) { if (neg) before[nb++] = found; else after[na++] = found; }
            }
            if (rc) break;
            for (int p = 0; p < npics; p++) if (!keep[p]) pics[p].ref = 0;
        }
        /* lists 8.3.4 */
        int l0[16] = {0}, l1[16] = {0}, total = nb + na;
        if (sl.type != SLICE_I) {
            if (!total) { printf("P/B slice without current references\n"); rc = 1; break; }
            int tmp[32], nt;
            for (int l = 0; l < (sl.type == SLICE_B ? 2 : 1); l++) {
                int need = sl.num_ref[l] + 1 > total ? sl.num_ref[l] + 1 : total;
                nt = 0;
                while (nt < need) {
                    for (int k = 0; k < (l ? na : nb) && nt < need; k++) tmp[nt++] = l ? after[k] : before[k];
                    for (int k = 0; k < (l ? nb : na) && nt < need; k++) tmp[nt++] = l ? before[k] : after[k];
                }
                int *list = l ? l1 : l0;
                for (int k = 0; k <= sl.num_ref[l]; k++) list[k] = tmp[sl.mod[l] ? sl.list_entry[l][k] : k];
            }
        }
        int cur = -1;
        for (int p = 0; p < npics && cur < 0; p++) if (!pics[p].ref) cur = p;
        if (cur < 0) { printf("no free picture buffer\n"); rc = 1; break; }
        pics[cur].poc = poc;

        double ms = 0;
        int r = 0;
        if (parse_only) {
            printf("SLICE nal %d type %c poc %d header %d bits data byte %d L0 [", t, "BPI"[sl.type], poc, sl.header_bits, sl.data_byte);
            for (int k = 0; sl.type != SLICE_I && k <= sl.num_ref[0]; k++) printf(" %d", pics[l0[k]].poc);
            printf(" ] L1 [");
            for (int k = 0; sl.type == SLICE_B && k <= sl.num_ref[1]; k++) printf(" %d", pics[l1[k]].poc);
            printf(" ] ep %d\n", sl.num_ep);
        } else {
            memcpy(bs.cpu, es + s, n);
            /* entry points: syntax gives raw byte offsets (EPBs included); the VE works on unescaped data */
            if (sl.num_ep) {
                uint32_t *ep = (uint32_t *)epbuf.cpu;
                int data_raw = sl.data_byte, k = 0;
                while (k < n_epb && epb_pos[k] <= data_raw + k) k++;   /* raw position of the slice data start */
                data_raw += k;
                int start = data_raw;
                for (int q = 0; q < sl.num_ep; q++) {
                    int len = sl.ep[q] + 1, end = start + len, in = 0;
                    for (int m = 0; m < n_epb; m++) if (epb_pos[m] >= start && epb_pos[m] < end) in++;
                    ep[q] = ep_raw ? (uint32_t)sl.ep[q] : (uint32_t)(len - in - 1);
                    start = end;
                }
            }
            r = decode(&sl, cur, poc, n, l0, l1, &ms);
            if (r < 0) { rc = 1; break; }
        }
        double wall = (now() - w0) * 1e3;
        failed += r;
        ve_total += ms; wall_total += wall; if (ms > ve_max) ve_max = ms;
        pics[cur].ref = 1;     /* every decoded picture is "used for short-term reference" until a later RPS drops it */
        if (parse_only) { frames++; continue; }
        uint32_t ad = 0;
        const uint8_t *y = pics[cur].mem.cpu, *uv = y + stride * coded_h;
        for (int rr = 0; rr < vis_h; rr++) { memcpy(row, y + rr * stride, vis_w); ad = adler(row, vis_w, ad); }
        for (int rr = 0; rr < vis_h / 2; rr++) { memcpy(row, uv + rr * stride, vis_w); ad = adler(row, vis_w, ad); }
        if (frames < dump) fwrite(pics[cur].mem.cpu, 1, nv12_bytes, out);
        printf("FRAME %d %d %d %c nal%d %.2f ms wall %.2f ms adler=%08x%s\n", frames, epoch, poc, "BPI"[sl.type], t,
               ms, wall, ad, r ? " ENGINE ERROR" : "");
        frames++;
    }
    fclose(out);
    if (parse_only) { printf("parsed %d pictures%s\n", frames, rc ? " (stopped on error)" : ""); return rc; }
    if (frames)
        printf("RESULT %s frames=%d failed=%d geometry=%dx%d visible=%dx%d engine avg %.2f ms max %.2f ms, per frame incl. parse+copy %.2f ms\n",
               rc || failed ? "FAIL" : "OK", frames, failed, W, H, vis_w, vis_h, ve_total / frames, ve_max, wall_total / frames);
    buf_free(&bs); buf_free(&neigh); buf_free(&epbuf);
    for (int p = 0; p < npics; p++) buf_free(&pics[p].mem);
    wr(VE_MODE, VE_MODE_DISABLED);
    munmap((void *)regs, 0x1000);
    ioctl(ve_fd, IOCTL_DISABLE_VE, 0);
    ioctl(ve_fd, IOCTL_ENGINE_REL, 0);
    close(ve_fd); close(ion_fd);
    return rc || failed;
}
