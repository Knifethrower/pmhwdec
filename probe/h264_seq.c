/* SPDX-License-Identifier: GPL-2.0-or-later */
/* h264_seq: decodes a whole Annex-B H.264 stream (I/P/B, references, weighted prediction) on the Allwinner video
 * engine with no Allwinner userspace code. Next step after h264_frame.c (one IDR picture).
 *
 * Sources (all GPL-2.0, no libcedarc): mainline cedrus (cedrus_h264.c set_params/setup/write_frame_list/ref lists/
 * pred weights, cedrus_hw.c output format, cedrus_regs.h), vendor 4.9 cedar_ve.{c,h} (ioctls, mmap, IOMMU map,
 * H.264 IRQ path), 4.9 uapi ion.h. Reference handling (POC, sliding window, list init/modification) per the H.264
 * spec, sections 8.2.1, 8.2.4, 8.2.5.
 *
 * Usage: h264_seq in.h264 out.nv12 > log
 *   out = the first DUMP_FRAMES (default all) frames in DECODE order, each ALIGN16(w) x ALIGN16(h) NV12; the log has
 *   "FRAME <n> <epoch> <poc> ... adler=<hex>" lines (FFmpeg framecrc's adler32 of the visible NV12) so the PC side can
 *   put them in display order (compare_seq.py, compare_crc.py). MAX_FRAMES=n stops early.
 * Limits (test only): one slice per picture, frame pictures, no scaling matrices, POC type 0/2, MMCO 1 only,
 * no long-term references, width <= 2048.
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

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define BIT(n) (1u << (n))

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

/* ---- cedrus_regs.h ---- */
#define VE_MODE                         0x00
#define VE_MODE_REC_WR_MODE_2MB         (0x01 << 20)
#define VE_MODE_DDR_MODE_BW_128         (0x03 << 16)
#define VE_MODE_DISABLED                (0x07 << 0)
#define VE_MODE_DEC_H264                (0x01 << 0)
#define VE_BUF_CTRL                     0x50
#define VE_PRIMARY_CHROMA_BUF_LEN       0xc4
#define VE_PRIMARY_FB_LINE_STRIDE       0xc8
#define VE_PRIMARY_OUT_FMT              0xec
#define VE_PRIMARY_OUT_FMT_NV12         (0x04 << 4)
#define VE_H264_SPS                     0x200
#define VE_H264_SPS_MBS_ONLY            BIT(18)
#define VE_H264_SPS_MB_ADAPTIVE_FRAME_FIELD BIT(17)
#define VE_H264_SPS_DIRECT_8X8_INFERENCE BIT(16)
#define VE_H264_PPS                     0x204
#define VE_H264_PPS_ENTROPY_CODING_MODE BIT(15)
#define VE_H264_PPS_WEIGHTED_PRED       BIT(4)
#define VE_H264_PPS_CONSTRAINED_INTRA_PRED BIT(1)
#define VE_H264_PPS_TRANSFORM_8X8_MODE  BIT(0)
#define VE_H264_SHS                     0x208
#define VE_H264_SHS_FIRST_SLICE_IN_PIC  BIT(5)
#define VE_H264_SHS_DIRECT_SPATIAL_MV_PRED BIT(2)
#define VE_H264_SHS2                    0x20c
#define VE_H264_SHS2_NUM_REF_IDX_ACTIVE_OVRD BIT(12)
#define VE_H264_SHS_WP                  0x210
#define VE_H264_SHS_QP                  0x21c
#define VE_H264_SHS_QP_SCALING_MATRIX_DEFAULT BIT(24)
#define VE_H264_CTRL                    0x220
#define VE_H264_CTRL_INT_MASK           0x7
#define VE_H264_TRIGGER_TYPE            0x224
#define VE_H264_TRIGGER_TYPE_N_BITS(x)  (((x) & 0x3f) << 8)
#define VE_H264_TRIGGER_TYPE_AVC_SLICE_DECODE (8 << 0)
#define VE_H264_TRIGGER_TYPE_INIT_SWDEC (7 << 0)
#define VE_H264_TRIGGER_TYPE_FLUSH_BITS (3 << 0)
#define VE_H264_STATUS                  0x228
#define VE_H264_STATUS_VLD_BUSY         BIT(8)
#define VE_H264_CUR_MB_NUM              0x22c
#define VE_H264_VLD_ADDR                0x230
#define VE_H264_VLD_ADDR_FIRST          BIT(30)
#define VE_H264_VLD_ADDR_LAST           BIT(29)
#define VE_H264_VLD_ADDR_VALID          BIT(28)
#define VE_H264_VLD_ADDR_VAL(x)         (((x) & 0x0ffffff0) | ((x) >> 28))
#define VE_H264_VLD_OFFSET              0x234
#define VE_H264_VLD_LEN                 0x238
#define VE_H264_VLD_END                 0x23c
#define VE_H264_SDROT_CTRL              0x240
#define VE_H264_OUTPUT_FRAME_IDX        0x24c
#define VE_H264_EXTRA_BUFFER1           0x250
#define VE_H264_EXTRA_BUFFER2           0x254
#define VE_AVC_SRAM_PORT_OFFSET         0x2e0
#define VE_AVC_SRAM_PORT_DATA           0x2e4
#define SRAM_PRED_WEIGHT_TABLE          0x000
#define SRAM_FRAMEBUFFER_LIST           0x100
#define SRAM_REF_LIST_0                 0x190
#define SRAM_REF_LIST_1                 0x199
#define H264_FRAME_NUM                  18
#define PIC_TYPE_FRAME                  0

enum { SLICE_P = 0, SLICE_B = 1, SLICE_I = 2 };

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
    if (!heap_mask) {   /* first heap id that allocates and maps for the VE (sunxi ION may lack heap query) */
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

static int unescape(const uint8_t *s, int n, uint8_t *d)
{
    int o = 0, zeros = 0;
    for (int i = 0; i < n; i++) {
        if (zeros >= 2 && s[i] == 3) { zeros = 0; continue; }
        zeros = s[i] == 0 ? zeros + 1 : 0;
        d[o++] = s[i];
    }
    return o;
}

/* ---------------- parameter sets ---------------- */
typedef struct {
    int ok, profile, chroma_format_idc, log2_max_frame_num, poc_type, log2_max_poc_lsb, max_num_ref_frames;
    int width_mbs, height_map_units, frame_mbs_only, mbaff, direct_8x8, crop_l, crop_r, crop_t, crop_b;
} Sps;
typedef struct {
    int ok, entropy, bottom_field_poc_present, num_ref_l0, num_ref_l1, weighted_pred, weighted_bipred, init_qp,
        chroma_qp_offset, second_chroma_qp_offset, deblock_ctrl, constrained_intra, redundant_pic_cnt, t8x8;
} Pps;
static Sps sps;
static Pps pps;

static int parse_sps(Bits *b, Sps *s)
{
    memset(s, 0, sizeof *s);
    s->profile = u(b, 8); u(b, 16); ue(b);
    s->chroma_format_idc = 1;
    if (s->profile == 100 || s->profile == 110 || s->profile == 122 || s->profile == 244 || s->profile == 44 ||
        s->profile == 83 || s->profile == 86 || s->profile == 118 || s->profile == 128) {
        s->chroma_format_idc = ue(b);
        if (s->chroma_format_idc == 3) u(b, 1);
        if (ue(b) || ue(b)) { printf("only 8-bit streams\n"); return -1; }
        u(b, 1);
        if (u(b, 1)) { printf("SPS scaling matrices not handled\n"); return -1; }
    }
    s->log2_max_frame_num = ue(b) + 4;
    s->poc_type = ue(b);
    if (s->poc_type == 0) s->log2_max_poc_lsb = ue(b) + 4;
    else if (s->poc_type == 1) { printf("POC type 1 not handled\n"); return -1; }
    s->max_num_ref_frames = ue(b);
    u(b, 1);
    s->width_mbs = ue(b) + 1;
    s->height_map_units = ue(b) + 1;
    s->frame_mbs_only = u(b, 1);
    if (!s->frame_mbs_only) s->mbaff = u(b, 1);
    s->direct_8x8 = u(b, 1);
    if (u(b, 1)) { s->crop_l = ue(b); s->crop_r = ue(b); s->crop_t = ue(b); s->crop_b = ue(b); }
    s->ok = 1;
    return 0;
}

static int more_rbsp(Bits *b)
{
    int last = b->n * 8 - 1;
    while (last >= 0 && !(b->d[last >> 3] >> (7 - (last & 7)) & 1)) last--;
    return b->pos < last;
}

static int parse_pps(Bits *b, Pps *p)
{
    memset(p, 0, sizeof *p);
    ue(b); ue(b);
    p->entropy = u(b, 1);
    p->bottom_field_poc_present = u(b, 1);
    if (ue(b)) { printf("slice groups not supported\n"); return -1; }
    p->num_ref_l0 = ue(b); p->num_ref_l1 = ue(b);
    p->weighted_pred = u(b, 1); p->weighted_bipred = u(b, 2);
    p->init_qp = se(b); se(b);
    p->chroma_qp_offset = p->second_chroma_qp_offset = se(b);
    p->deblock_ctrl = u(b, 1); p->constrained_intra = u(b, 1); p->redundant_pic_cnt = u(b, 1);
    if (more_rbsp(b)) {
        p->t8x8 = u(b, 1);
        if (u(b, 1)) { printf("PPS scaling matrices not handled\n"); return -1; }
        p->second_chroma_qp_offset = se(b);
    }
    p->ok = 1;
    return 0;
}

/* ---------------- slice header ---------------- */
typedef struct { int idc, val; } Mod;
typedef struct {
    int nal_type, nal_ref_idc, first_mb, type, frame_num, poc_lsb, delta_poc_bottom, direct_spatial;
    int num_ref[2];                       /* active - 1 */
    int nmod[2]; Mod mod[2][32];
    int luma_denom, chroma_denom;
    int16_t lw[2][32], lo[2][32], cw[2][32][2], co[2][32][2];
    int idr_long_term, adaptive, nmmco; int mmco[32][2];
    int cabac_init, qp_delta, disable_deblock, alpha, beta, header_bits;
} Slice;

static int parse_slice(Bits *b, Slice *sl)
{
    memset(sl, 0, sizeof *sl);
    u(b, 1);
    sl->nal_ref_idc = u(b, 2);
    sl->nal_type = u(b, 5);
    sl->first_mb = ue(b);
    sl->type = ue(b) % 5;
    if (sl->type > 2) { printf("SP/SI slices not handled\n"); return -1; }
    ue(b);
    sl->frame_num = u(b, sps.log2_max_frame_num);
    if (!sps.frame_mbs_only && u(b, 1)) { printf("field pictures not handled\n"); return -1; }
    if (sl->nal_type == 5) ue(b);
    if (sps.poc_type == 0) {
        sl->poc_lsb = u(b, sps.log2_max_poc_lsb);
        if (pps.bottom_field_poc_present) sl->delta_poc_bottom = se(b);
    }
    if (pps.redundant_pic_cnt) ue(b);
    if (sl->type == SLICE_B) sl->direct_spatial = u(b, 1);
    sl->num_ref[0] = pps.num_ref_l0; sl->num_ref[1] = pps.num_ref_l1;
    if (sl->type != SLICE_I && u(b, 1)) {
        sl->num_ref[0] = ue(b);
        if (sl->type == SLICE_B) sl->num_ref[1] = ue(b);
    }
    for (int l = 0; l < (sl->type == SLICE_B ? 2 : sl->type == SLICE_P ? 1 : 0); l++) {
        if (!u(b, 1)) continue;
        for (;;) {
            int idc = ue(b);
            if (idc == 3) break;
            if (idc == 2 || sl->nmod[l] >= 32) { printf("long-term list modification not handled\n"); return -1; }
            sl->mod[l][sl->nmod[l]].idc = idc;
            sl->mod[l][sl->nmod[l]++].val = ue(b);
        }
    }
    if ((pps.weighted_pred && sl->type == SLICE_P) || (pps.weighted_bipred == 1 && sl->type == SLICE_B)) {
        sl->luma_denom = ue(b);
        if (sps.chroma_format_idc) sl->chroma_denom = ue(b);
        for (int l = 0; l < (sl->type == SLICE_B ? 2 : 1); l++)
            for (int i = 0; i <= sl->num_ref[l]; i++) {
                sl->lw[l][i] = 1 << sl->luma_denom; sl->lo[l][i] = 0;
                if (u(b, 1)) { sl->lw[l][i] = se(b); sl->lo[l][i] = se(b); }
                for (int c = 0; c < 2; c++) { sl->cw[l][i][c] = 1 << sl->chroma_denom; sl->co[l][i][c] = 0; }
                if (sps.chroma_format_idc && u(b, 1))
                    for (int c = 0; c < 2; c++) { sl->cw[l][i][c] = se(b); sl->co[l][i][c] = se(b); }
            }
    }
    if (sl->nal_ref_idc) {
        if (sl->nal_type == 5) { u(b, 1); sl->idr_long_term = u(b, 1); }
        else if ((sl->adaptive = u(b, 1))) {
            for (;;) {
                int op = ue(b);
                if (op == 0) break;
                if (op != 1 || sl->nmmco >= 32) { printf("MMCO %d not handled\n", op); return -1; }
                sl->mmco[sl->nmmco][0] = op;
                sl->mmco[sl->nmmco++][1] = ue(b);
            }
        }
    }
    if (pps.entropy && sl->type != SLICE_I) sl->cabac_init = ue(b);
    sl->qp_delta = se(b);
    if (pps.deblock_ctrl) {
        sl->disable_deblock = ue(b);
        if (sl->disable_deblock != 1) { sl->alpha = se(b); sl->beta = se(b); }
    }
    sl->header_bits = b->pos;
    if (sl->idr_long_term) { printf("long-term IDR not handled\n"); return -1; }
    return 0;
}

/* ---------------- picture buffers + DPB ---------------- */
typedef struct {
    Buf mem;               /* NV12 + mv_col (two fields) */
    uint32_t luma, chroma, mv_top, mv_bot;
    int ref, frame_num, poc, pic_num;
} Pic;
#define MAX_PICS 18
static Pic pics[MAX_PICS];
static int npics;

static int cur_pic_num(const Slice *sl) { return sl->frame_num; }

static void compute_pic_nums(const Slice *sl)
{
    int max_fn = 1 << sps.log2_max_frame_num;
    for (int i = 0; i < npics; i++)
        if (pics[i].ref) pics[i].pic_num = pics[i].frame_num > sl->frame_num ? pics[i].frame_num - max_fn : pics[i].frame_num;
}

static int cmp_picnum_desc(const void *a, const void *b) { return pics[*(int *)b].pic_num - pics[*(int *)a].pic_num; }
static int cmp_poc_desc(const void *a, const void *b) { return pics[*(int *)b].poc - pics[*(int *)a].poc; }
static int cmp_poc_asc(const void *a, const void *b) { return pics[*(int *)a].poc - pics[*(int *)b].poc; }

/* 8.2.4.2 initial lists + 8.2.4.3 modification (short-term only); list entries are pics[] indices */
static int build_lists(const Slice *sl, int cur, int cur_poc, int list[2][33], int len[2])
{
    int refs[MAX_PICS], n = 0;
    compute_pic_nums(sl);
    for (int i = 0; i < npics; i++) if (pics[i].ref && i != cur) refs[n++] = i;
    len[0] = len[1] = 0;
    if (sl->type == SLICE_P) {
        qsort(refs, n, sizeof *refs, cmp_picnum_desc);
        memcpy(list[0], refs, n * sizeof *refs); len[0] = n;
    } else if (sl->type == SLICE_B) {
        int before[MAX_PICS], after[MAX_PICS], nb = 0, na = 0;
        for (int i = 0; i < n; i++) {
            if (pics[refs[i]].poc < cur_poc) before[nb++] = refs[i];
            else after[na++] = refs[i];
        }
        qsort(before, nb, sizeof *before, cmp_poc_desc);
        qsort(after, na, sizeof *after, cmp_poc_asc);
        memcpy(list[0], before, nb * sizeof(int)); memcpy(list[0] + nb, after, na * sizeof(int));
        memcpy(list[1], after, na * sizeof(int)); memcpy(list[1] + na, before, nb * sizeof(int));
        len[0] = len[1] = n;
        if (n > 1 && !memcmp(list[0], list[1], n * sizeof(int))) { int t = list[1][0]; list[1][0] = list[1][1]; list[1][1] = t; }
    }
    int max_fn = 1 << sps.log2_max_frame_num, curr = cur_pic_num(sl);
    for (int l = 0; l < 2; l++) {
        int active = sl->num_ref[l] + 1;
        if (len[l] > active) len[l] = active;
        int pred = curr;
        for (int m = 0; m < sl->nmod[l]; m++) {
            int d = sl->mod[l][m].val + 1, nowrap;
            if (sl->mod[l][m].idc == 0) { nowrap = pred - d; if (nowrap < 0) nowrap += max_fn; }
            else { nowrap = pred + d; if (nowrap >= max_fn) nowrap -= max_fn; }
            pred = nowrap;
            int pic_num = nowrap > curr ? nowrap - max_fn : nowrap, target = -1;
            for (int i = 0; i < npics; i++) if (pics[i].ref && i != cur && pics[i].pic_num == pic_num) target = i;
            if (target < 0) { printf("list modification names a missing picture (picnum %d)\n", pic_num); return -1; }
            /* insert at m, drop the later duplicate */
            int tmp[34], k = 0;
            for (int i = 0; i < m && i < len[l]; i++) tmp[k++] = list[l][i];
            tmp[k++] = target;
            for (int i = m; i < len[l]; i++) if (list[l][i] != target) tmp[k++] = list[l][i];
            if (k > active) k = active;
            memcpy(list[l], tmp, k * sizeof(int)); len[l] = k;
        }
        if (len[l] < active && (sl->type == SLICE_B || l == 0) && len[l])
            printf("warning: list %d has %d of %d active entries\n", l, len[l], active);
    }
    return 0;
}

/* 8.2.5 marking after the picture is decoded */
static void mark(const Slice *sl, int cur)
{
    if (!sl->nal_ref_idc) return;
    if (sl->nal_type == 5) {
        for (int i = 0; i < npics; i++) pics[i].ref = 0;
    } else if (sl->adaptive) {
        compute_pic_nums(sl);
        for (int m = 0; m < sl->nmmco; m++) {
            int pn = cur_pic_num(sl) - (sl->mmco[m][1] + 1);
            for (int i = 0; i < npics; i++) if (pics[i].ref && i != cur && pics[i].pic_num == pn) pics[i].ref = 0;
        }
    } else {
        int n = 0, oldest = -1;
        compute_pic_nums(sl);
        for (int i = 0; i < npics; i++)
            if (pics[i].ref && i != cur) { n++; if (oldest < 0 || pics[i].pic_num < pics[oldest].pic_num) oldest = i; }
        if (n >= (sps.max_num_ref_frames ? sps.max_num_ref_frames : 1) && oldest >= 0) pics[oldest].ref = 0;
    }
    pics[cur].ref = 1;
}

/* ---------------- VE programming (cedrus_h264.c) ---------------- */
static void sram_write(unsigned off, const void *data, int bytes)
{
    const uint32_t *w = data;
    wr(VE_AVC_SRAM_PORT_OFFSET, off << 2);
    for (int i = 0; i < (bytes + 3) / 4; i++) wr(VE_AVC_SRAM_PORT_DATA, w[i]);
}

static int skip_bits(int num)
{
    for (int count = 0; count < num;) {
        int n = num - count < 32 ? num - count : 32;
        wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_FLUSH_BITS | VE_H264_TRIGGER_TYPE_N_BITS(n));
        for (int spin = 0; rd(VE_H264_STATUS) & VE_H264_STATUS_VLD_BUSY; spin++)
            if (spin > 1000000) { printf("VLD busy timeout during bit skip\n"); return -1; }
        count += n;
    }
    return 0;
}

static void ref_pic_entry(uint32_t *e, const Pic *p, int poc)
{
    e[0] = poc; e[1] = poc;            /* frame: top = bottom POC (no delta_pic_order_cnt_bottom in our clips) */
    e[2] = PIC_TYPE_FRAME << 8;
    e[3] = p->luma; e[4] = p->chroma; e[5] = p->mv_top; e[6] = p->mv_bot; e[7] = 0;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* FFmpeg framecrc's checksum: adler32 starting from 0 */
static uint32_t adler(const uint8_t *p, int n, uint32_t s)
{
    uint32_t a = s & 0xffff, c = s >> 16;
    for (int i = 0; i < n; i++) { a = (a + p[i]) % 65521; c = (c + a) % 65521; }
    return c << 16 | a;
}

static Buf bs, pinfo, neigh;

static int decode(const Slice *sl, int cur, int cur_poc, int slice_len, double *ve_ms)
{
    int W = sps.width_mbs * 16, H = sps.height_map_units * 16;
    int list[2][33], len[2];
    if (build_lists(sl, cur, cur_poc, list, len)) return -1;
    __sync_synchronize();   /* bitstream copy (write-combined) before the engine reads it */
    double t0 = now();
    wr(VE_MODE, VE_MODE_REC_WR_MODE_2MB | VE_MODE_DDR_MODE_BW_128 | VE_MODE_DEC_H264);
    wr(VE_PRIMARY_OUT_FMT, VE_PRIMARY_OUT_FMT_NV12);
    wr(VE_PRIMARY_CHROMA_BUF_LEN, ALIGN(W, 16) * ALIGN(H, 16) / 4);
    wr(VE_PRIMARY_FB_LINE_STRIDE, (ALIGN(W, 16) / 2) << 16 | ALIGN(W, 16));
    wr(VE_H264_SDROT_CTRL, 0);
    wr(VE_H264_EXTRA_BUFFER1, pinfo.ve);
    wr(VE_H264_EXTRA_BUFFER2, neigh.ve);
    /* frame list: every short-term reference at its buffer position, plus the output picture */
    uint32_t fl[H264_FRAME_NUM][8];
    memset(fl, 0, sizeof fl);
    for (int i = 0; i < npics; i++) if (pics[i].ref && i != cur) ref_pic_entry(fl[i], &pics[i], pics[i].poc);
    ref_pic_entry(fl[cur], &pics[cur], cur_poc);
    sram_write(SRAM_FRAMEBUFFER_LIST, fl, sizeof fl);
    wr(VE_H264_OUTPUT_FRAME_IDX, cur);
    /* set_params */
    wr(VE_H264_VLD_LEN, slice_len * 8);
    wr(VE_H264_VLD_OFFSET, 0);
    wr(VE_H264_VLD_END, bs.ve + slice_len);
    wr(VE_H264_VLD_ADDR, VE_H264_VLD_ADDR_VAL(bs.ve) | VE_H264_VLD_ADDR_FIRST | VE_H264_VLD_ADDR_VALID | VE_H264_VLD_ADDR_LAST);
    wr(VE_BUF_CTRL, 0);
    wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_INIT_SWDEC);
    if (skip_bits(sl->header_bits)) return -1;
    if ((pps.weighted_pred && sl->type == SLICE_P) || (pps.weighted_bipred == 1 && sl->type == SLICE_B)) {
        wr(VE_H264_SHS_WP, (sl->chroma_denom & 7) << 4 | (sl->luma_denom & 7));
        wr(VE_AVC_SRAM_PORT_OFFSET, SRAM_PRED_WEIGHT_TABLE << 2);
        for (int l = 0; l < 2; l++) {
            for (int i = 0; i < 32; i++)
                wr(VE_AVC_SRAM_PORT_DATA, ((uint32_t)sl->lo[l][i] & 0x1ff) << 16 | (sl->lw[l][i] & 0x1ff));
            for (int i = 0; i < 32; i++)
                for (int c = 0; c < 2; c++)
                    wr(VE_AVC_SRAM_PORT_DATA, ((uint32_t)sl->co[l][i][c] & 0x1ff) << 16 | (sl->cw[l][i][c] & 0x1ff));
        }
    }
    for (int l = 0; l < (sl->type == SLICE_B ? 2 : sl->type == SLICE_P ? 1 : 0); l++) {
        uint8_t ra[32];
        memset(ra, 0, sizeof ra);
        for (int i = 0; i < len[l]; i++) ra[i] = list[l][i] << 1;
        int active = sl->num_ref[l] + 1;
        sram_write(l ? SRAM_REF_LIST_1 : SRAM_REF_LIST_0, ra, ALIGN(active, 4) < 32 ? ALIGN(active, 4) : 32);
    }
    uint32_t reg = (sl->num_ref[0] & 0x1f) << 10 | (sl->num_ref[1] & 0x1f) << 5 | (pps.weighted_bipred & 3) << 2;
    if (pps.entropy) reg |= VE_H264_PPS_ENTROPY_CODING_MODE;
    if (pps.weighted_pred) reg |= VE_H264_PPS_WEIGHTED_PRED;
    if (pps.constrained_intra) reg |= VE_H264_PPS_CONSTRAINED_INTRA_PRED;
    if (pps.t8x8) reg |= VE_H264_PPS_TRANSFORM_8X8_MODE;
    wr(VE_H264_PPS, reg);
    reg = (sps.chroma_format_idc & 7) << 19 | ((sps.width_mbs - 1) & 0xff) << 8 | ((sps.height_map_units - 1) & 0xff);
    if (sps.frame_mbs_only) reg |= VE_H264_SPS_MBS_ONLY;
    if (sps.mbaff) reg |= VE_H264_SPS_MB_ADAPTIVE_FRAME_FIELD;
    if (sps.direct_8x8) reg |= VE_H264_SPS_DIRECT_8X8_INFERENCE;
    wr(VE_H264_SPS, reg);
    reg = (sl->first_mb % sps.width_mbs & 0xff) << 24 | (sl->first_mb / sps.width_mbs & 0xff) << 16 |
          (sl->nal_ref_idc ? BIT(12) : 0) | (sl->type & 0xf) << 8 | (sl->cabac_init & 3) | VE_H264_SHS_FIRST_SLICE_IN_PIC;
    if (sl->direct_spatial) reg |= VE_H264_SHS_DIRECT_SPATIAL_MV_PRED;
    wr(VE_H264_SHS, reg);
    wr(VE_H264_SHS2, VE_H264_SHS2_NUM_REF_IDX_ACTIVE_OVRD | (sl->num_ref[0] & 0x1f) << 24 | (sl->num_ref[1] & 0x1f) << 16 |
                     (sl->disable_deblock & 3) << 8 | (sl->alpha & 0xf) << 4 | (sl->beta & 0xf));
    wr(VE_H264_SHS_QP, (pps.second_chroma_qp_offset & 0x3f) << 16 | (pps.chroma_qp_offset & 0x3f) << 8 |
                       ((26 + pps.init_qp + sl->qp_delta) & 0x3f) | VE_H264_SHS_QP_SCALING_MATRIX_DEFAULT);
    wr(VE_H264_STATUS, rd(VE_H264_STATUS));
    wr(VE_H264_CTRL, VE_H264_CTRL_INT_MASK);
    wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_AVC_SLICE_DECODE);
    int irq = ioctl(ve_fd, IOCTL_WAIT_VE_DE, 2);
    *ve_ms = (now() - t0) * 1e3;
    uint32_t st = rd(VE_H264_STATUS), mbs = rd(VE_H264_CUR_MB_NUM);
    wr(VE_H264_STATUS, VE_H264_CTRL_INT_MASK);
    wr(VE_H264_CTRL, rd(VE_H264_CTRL) & ~VE_H264_CTRL_INT_MASK);
    if (irq != 1 || !(st & 1) || (st & 6)) {
        printf("  engine: irq %d status 0x%08x MB %u\n", irq, st, mbs);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.h264 out.nv12\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long es_len = ftell(f);
    rewind(f);
    uint8_t *es = malloc(es_len), *rbsp = malloc(1 << 20);
    if (!es || !rbsp || fread(es, 1, es_len, f) != (size_t)es_len) { printf("read failed\n"); return 1; }
    fclose(f);
    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }

    int parse_only = getenv("PARSE_ONLY") != NULL;   /* PC check: headers, POC, lists; no engine */
    int dump = getenv("DUMP_FRAMES") ? atoi(getenv("DUMP_FRAMES")) : 1 << 30;
    int max_frames = getenv("MAX_FRAMES") ? atoi(getenv("MAX_FRAMES")) : 1 << 30;
    if (parse_only) goto parse;
    ve_fd = open("/dev/cedar_dev", O_RDWR);
    ion_fd = open("/dev/ion", O_RDWR);
    if (ve_fd < 0 || ion_fd < 0) { printf("open /dev/cedar_dev or /dev/ion: %s\n", strerror(errno)); return 1; }
    if (ioctl(ve_fd, IOCTL_ENGINE_REQ, 0) < 0) { printf("ENGINE_REQ: %s\n", strerror(errno)); return 1; }
    ioctl(ve_fd, IOCTL_ENABLE_VE, 0);
    ioctl(ve_fd, IOCTL_RESET_VE, 0);
    regs = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, ve_fd, 0);
    if (regs == MAP_FAILED) { printf("mmap regs: %s\n", strerror(errno)); return 1; }
    printf("VE id e0=0x%08x e4=0x%08x\n", rd(0xe0), rd(0xe4));
parse:;
    int rc = 0, frames = 0, failed = 0, epoch = -1, W = 0, H = 0, luma = 0, frame_bytes = 0, vis_w = 0, vis_h = 0, top = 0;
    uint8_t *row = NULL;
    int prev_ref_poc_msb = 0, prev_ref_poc_lsb = 0, prev_frame_num = 0, frame_num_offset = 0;
    double ve_total = 0, ve_max = 0, wall_total = 0;
    for (long i = 0; i + 3 < es_len && frames < max_frames;) {
        if (!(es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1)) { i++; continue; }
        long s = i + 3, e = s;
        while (e + 3 < es_len && !(es[e] == 0 && es[e + 1] == 0 && (es[e + 2] == 1 || (es[e + 2] == 0 && es[e + 3] == 1)))) e++;
        if (e + 3 >= es_len) e = es_len;
        i = e;
        int t = es[s] & 0x1f, n = e - s;
        double w0 = now();
        if (n > (1 << 20)) { printf("NAL too large\n"); rc = 1; break; }
        Bits b = { rbsp, unescape(es + s, n, rbsp), 0 };
        if (t == 7) {
            b.pos = 8;
            if (parse_sps(&b, &sps)) { rc = 1; break; }
            if (!npics) {
                W = sps.width_mbs * 16; H = sps.height_map_units * 16;
                if (W > 2048 || !sps.frame_mbs_only || sps.chroma_format_idc != 1) { printf("outside this test's limits\n"); rc = 1; break; }
                luma = ALIGN(W, 16) * ALIGN(H, 16); frame_bytes = luma * 3 / 2;
                vis_w = W - 2 * (sps.crop_l + sps.crop_r); vis_h = H - 2 * (sps.crop_t + sps.crop_b);
                top = 2 * sps.crop_t;
                if (sps.crop_l) { printf("left crop not handled\n"); rc = 1; break; }
                row = malloc(vis_w);
                int field = (W / 16) * (H / 16) * 16 * (sps.direct_8x8 ? 1 : 2);
                npics = sps.max_num_ref_frames + 2 > MAX_PICS ? MAX_PICS : sps.max_num_ref_frames + 2;
                int pinfo_size = H264_FRAME_NUM * 0x1000 + H * 2 * 64;
                if (pinfo_size < 130 * 1024) pinfo_size = 130 * 1024;
                if (!parse_only && (buf_alloc(&bs, 1 << 20) || buf_alloc(&pinfo, pinfo_size) || buf_alloc(&neigh, 32 * 1024))) { rc = 1; break; }
                for (int p = 0; p < npics && !parse_only; p++) {
                    if (buf_alloc(&pics[p].mem, frame_bytes + 2 * field)) { rc = 1; break; }
                    pics[p].luma = pics[p].mem.ve; pics[p].chroma = pics[p].mem.ve + luma;
                    pics[p].mv_top = pics[p].mem.ve + frame_bytes; pics[p].mv_bot = pics[p].mv_top + field;
                }
                if (rc) break;
                printf("stream: profile %d, %dx%d coded, max refs %d, POC type %d, %d picture buffers\n",
                       sps.profile, W, H, sps.max_num_ref_frames, sps.poc_type, npics);
            }
            continue;
        }
        if (t == 8) { b.pos = 8; if (parse_pps(&b, &pps)) { rc = 1; break; } continue; }
        if (t != 1 && t != 5) continue;
        if (!sps.ok || !pps.ok) continue;
        Slice sl;
        if (parse_slice(&b, &sl)) { rc = 1; break; }
        if (sl.first_mb != 0) { printf("multiple slices per picture not handled\n"); rc = 1; break; }

        /* POC (8.2.1.1 type 0, 8.2.1.3 type 2) */
        int poc, max_fn = 1 << sps.log2_max_frame_num;
        if (t == 5) { prev_ref_poc_msb = prev_ref_poc_lsb = 0; prev_frame_num = 0; frame_num_offset = 0; epoch++; }
        if (sps.poc_type == 0) {
            int max_lsb = 1 << sps.log2_max_poc_lsb, msb = prev_ref_poc_msb;
            if (sl.poc_lsb < prev_ref_poc_lsb && prev_ref_poc_lsb - sl.poc_lsb >= max_lsb / 2) msb += max_lsb;
            else if (sl.poc_lsb > prev_ref_poc_lsb && sl.poc_lsb - prev_ref_poc_lsb > max_lsb / 2) msb -= max_lsb;
            poc = msb + sl.poc_lsb;
            if (sl.nal_ref_idc) { prev_ref_poc_msb = msb; prev_ref_poc_lsb = sl.poc_lsb; }
        } else {
            if (t != 5 && prev_frame_num > sl.frame_num) frame_num_offset += max_fn;
            poc = t == 5 ? 0 : 2 * (frame_num_offset + sl.frame_num) - (sl.nal_ref_idc == 0);
            prev_frame_num = sl.frame_num;
        }
        if (t == 5) for (int p = 0; p < npics; p++) pics[p].ref = 0;

        int cur = -1;
        for (int p = 0; p < npics && cur < 0; p++) if (!pics[p].ref) cur = p;
        if (cur < 0) { printf("no free picture buffer\n"); rc = 1; break; }
        pics[cur].frame_num = sl.frame_num; pics[cur].poc = poc;
        double ms = 0;
        int r = 0;
        if (parse_only) {
            int list[2][33], len[2];
            if (build_lists(&sl, cur, poc, list, len)) { rc = 1; break; }
            printf("SLICE type %c frame_num %d poc %d header %d bits L0 [", "PBI"[sl.type], sl.frame_num, poc, sl.header_bits);
            for (int k = 0; k < len[0]; k++) printf(" %d", pics[list[0][k]].poc);
            printf(" ] L1 [");
            for (int k = 0; k < len[1]; k++) printf(" %d", pics[list[1][k]].poc);
            printf(" ] mods %d/%d mmco %d\n", sl.nmod[0], sl.nmod[1], sl.nmmco);
            mark(&sl, cur);
            frames++;
            continue;
        }
        memcpy(bs.cpu, es + s, n);
        r = decode(&sl, cur, poc, n, &ms);
        if (r < 0) { rc = 1; break; }
        double wall = (now() - w0) * 1e3;
        failed += r;
        ve_total += ms; wall_total += wall; if (ms > ve_max) ve_max = ms;
        mark(&sl, cur);
        uint32_t ad = 0;
        int stride = ALIGN(W, 16);
        const uint8_t *py = pics[cur].mem.cpu, *puv = py + luma;
        for (int rr = top; rr < top + vis_h; rr++) { memcpy(row, py + rr * stride, vis_w); ad = adler(row, vis_w, ad); }
        for (int rr = top / 2; rr < (top + vis_h) / 2; rr++) { memcpy(row, puv + rr * stride, vis_w); ad = adler(row, vis_w, ad); }
        if (frames < dump) fwrite(pics[cur].mem.cpu, 1, frame_bytes, out);
        printf("FRAME %d %d %d %c ref%d refs_l0 %d l1 %d %.2f ms wall %.2f ms adler=%08x%s\n", frames, epoch, poc, "PBI"[sl.type],
               sl.nal_ref_idc ? 1 : 0, sl.num_ref[0] + 1, sl.num_ref[1] + 1, ms, wall, ad, r ? " ENGINE ERROR" : "");
        frames++;
    }
    fclose(out);
    if (parse_only) { printf("parsed %d pictures%s\n", frames, rc ? " (stopped on error)" : ""); return rc; }
    if (frames)
        printf("RESULT %s frames=%d failed=%d geometry=%dx%d stride=%d engine avg %.2f ms max %.2f ms, per frame incl. parse+copy %.2f ms\n",
               rc || failed ? "FAIL" : "OK", frames, failed, W, H, ALIGN(W, 16), ve_total / frames, ve_max, wall_total / frames);
    buf_free(&bs); buf_free(&pinfo); buf_free(&neigh);
    for (int p = 0; p < npics; p++) buf_free(&pics[p].mem);
    wr(VE_MODE, VE_MODE_DISABLED);
    munmap((void *)regs, 0x1000);
    ioctl(ve_fd, IOCTL_DISABLE_VE, 0);
    ioctl(ve_fd, IOCTL_ENGINE_REL, 0);
    close(ve_fd); close(ion_fd);
    return rc || failed;
}
