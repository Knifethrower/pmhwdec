/* SPDX-License-Identifier: GPL-2.0-or-later */
/* h264_frame: decodes the first IDR picture of an Annex-B H.264 file on the Allwinner video engine with no
 * Allwinner userspace code, and writes it as NV12. Proves (or not) that mainline Cedrus' register programming
 * works from userspace on a vendor 4.9 kernel (/dev/cedar_dev), per chip.
 *
 * Sources (all GPL-2.0, no libcedarc):
 *  - register sequence: mainline drivers/staging/media/sunxi/cedrus (cedrus_h264.c, cedrus_hw.c, cedrus_regs.h)
 *  - device interface: vendor 4.9 drivers/media/cedar-ve/cedar_ve.{c,h} (ioctls, mmap, IOMMU map, H.264 IRQ path)
 *  - memory: 4.9 drivers/staging/android/uapi/ion.h (legacy ION ABI with heap query)
 *
 * Usage: h264_frame in.h264 out.nv12  -> prints geometry; out = ALIGN16(w) x ALIGN16(h) luma + interleaved chroma.
 * Limits (test only): one slice per picture, no scaling matrices, frame pictures (frame_mbs_only), width <= 2048.
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
struct ion_heap_data { char name[32]; uint32_t type, heap_id, reserved0, reserved1, reserved2; };
struct ion_heap_query { uint32_t cnt, reserved0; uint64_t heaps; uint32_t reserved1, reserved2; };
#define ION_IOC_ALLOC      _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE       _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_MAP        _IOWR('I', 2, struct ion_fd_data)
#define ION_IOC_HEAP_QUERY _IOWR('I', 8, struct ion_heap_query)
#define ION_HEAP_TYPE_SYSTEM 0
#define ION_HEAP_TYPE_CARVEOUT 2
#define ION_HEAP_TYPE_DMA 4

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
#define VE_H264_SHS2                    0x20c
#define VE_H264_SHS2_NUM_REF_IDX_ACTIVE_OVRD BIT(12)
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
#define VE_H264_ERROR_CASE              0x2b8
#define VE_AVC_SRAM_PORT_OFFSET         0x2e0
#define VE_AVC_SRAM_PORT_DATA           0x2e4
#define SRAM_H264_FRAMEBUFFER_LIST      0x100
#define H264_FRAME_NUM                  18
#define PIC_TYPE_FRAME                  0

static volatile uint8_t *regs;
static void wr(unsigned off, uint32_t v) { *(volatile uint32_t *)(regs + off) = v; }
static uint32_t rd(unsigned off) { return *(volatile uint32_t *)(regs + off); }

/* ---------------- memory: ION buffer + VE IOMMU address ---------------- */
typedef struct { int handle, fd; size_t size; uint8_t *cpu; uint32_t ve; } Buf;
static int ion_fd = -1, ve_fd = -1, heap_mask;

static int pick_heap(void)
{
    struct ion_heap_data h[16];
    struct ion_heap_query q = { .cnt = 16, .heaps = (uintptr_t)h };
    if (ioctl(ion_fd, ION_IOC_HEAP_QUERY, &q) < 0) {
        /* older sunxi ION without heap query: first heap id that allocates and maps for the VE */
        printf("ION heap query: %s, trying heap ids\n", strerror(errno));
        for (int id = 0; id < 16; id++) {
            struct ion_allocation_data a = { .len = 4096, .align = 4096, .heap_id_mask = 1u << id };
            if (ioctl(ion_fd, ION_IOC_ALLOC, &a) < 0) continue;
            struct ion_fd_data f = { .handle = a.handle };
            int ok = ioctl(ion_fd, ION_IOC_MAP, &f) == 0;
            struct user_iommu_param p = { .fd = ok ? f.fd : -1 };
            ok = ok && ioctl(ve_fd, IOCTL_GET_IOMMU_ADDR, &p) == 0;
            if (ok) ioctl(ve_fd, IOCTL_FREE_IOMMU_ADDR, &p);
            if (f.fd > 0) close(f.fd);
            struct ion_handle_data h = { .handle = a.handle };
            ioctl(ion_fd, ION_IOC_FREE, &h);
            printf("  heap id %d: allocates, VE map %s\n", id, ok ? "ok" : "failed");
            if (ok) { heap_mask = 1 << id; return 0; }
        }
        return -1;
    }
    int pick = -1;
    for (unsigned i = 0; i < q.cnt; i++) {
        printf("ION heap %u: '%s' type %u\n", h[i].heap_id, h[i].name, h[i].type);
        /* VE sits behind the IOMMU on these chips: any dma-buf works, plain system heap is enough */
        if (h[i].type == ION_HEAP_TYPE_SYSTEM && pick < 0) pick = h[i].heap_id;
    }
    if (pick < 0) for (unsigned i = 0; i < q.cnt; i++)
        if (h[i].type == ION_HEAP_TYPE_DMA || h[i].type == ION_HEAP_TYPE_CARVEOUT) { pick = h[i].heap_id; break; }
    if (pick < 0) return -1;
    printf("using ION heap %d\n", pick);
    heap_mask = 1 << pick;
    return 0;
}

static int buf_alloc(Buf *b, size_t size)
{
    memset(b, 0, sizeof *b);
    b->size = ALIGN(size, 4096);
    struct ion_allocation_data a = { .len = b->size, .align = 4096, .heap_id_mask = heap_mask, .flags = 0 };
    if (ioctl(ion_fd, ION_IOC_ALLOC, &a) < 0) { printf("ION alloc %zu: %s\n", size, strerror(errno)); return -1; }
    b->handle = a.handle;
    struct ion_fd_data f = { .handle = a.handle };
    if (ioctl(ion_fd, ION_IOC_MAP, &f) < 0) { printf("ION map: %s\n", strerror(errno)); return -1; }
    b->fd = f.fd;
    b->cpu = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (b->cpu == MAP_FAILED) { printf("mmap dmabuf: %s\n", strerror(errno)); return -1; }
    memset(b->cpu, 0, b->size);
    struct user_iommu_param p = { .fd = b->fd };
    if (ioctl(ve_fd, IOCTL_GET_IOMMU_ADDR, &p) < 0) { printf("GET_IOMMU_ADDR: %s\n", strerror(errno)); return -1; }
    b->ve = p.iommu_addr;
    return 0;
}

static void buf_free(Buf *b)
{
    if (!b->size) return;
    struct user_iommu_param p = { .fd = b->fd, .iommu_addr = b->ve };
    ioctl(ve_fd, IOCTL_FREE_IOMMU_ADDR, &p);
    munmap(b->cpu, b->size);
    close(b->fd);
    struct ion_handle_data h = { .handle = b->handle };
    ioctl(ion_fd, ION_IOC_FREE, &h);
}

/* ---------------- H.264 headers (just what Cedrus needs for one I slice) ---------------- */
typedef struct { const uint8_t *d; int n, pos; } Bits;   /* over RBSP (emulation bytes removed) */
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

typedef struct {
    int profile, chroma_format_idc, log2_max_frame_num, poc_type, log2_max_poc_lsb, delta_pic_order_always_zero;
    int width_mbs, height_map_units, frame_mbs_only, mbaff, direct_8x8, crop_l, crop_r, crop_t, crop_b;
} Sps;
typedef struct {
    int entropy, bottom_field_poc_present, num_ref_l0, num_ref_l1, weighted_pred, weighted_bipred, init_qp,
        chroma_qp_offset, second_chroma_qp_offset, deblock_ctrl, constrained_intra, redundant_pic_cnt, t8x8;
} Pps;
typedef struct { int first_mb, type, qp_delta, cabac_init, disable_deblock, alpha, beta, header_bits; } Slice;

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
        if (u(b, 1)) { printf("SPS scaling matrices not handled in this test\n"); return -1; }
    }
    s->log2_max_frame_num = ue(b) + 4;
    s->poc_type = ue(b);
    if (s->poc_type == 0) s->log2_max_poc_lsb = ue(b) + 4;
    else if (s->poc_type == 1) {
        s->delta_pic_order_always_zero = u(b, 1); se(b); se(b);
        for (unsigned i = 0, n = ue(b); i < n; i++) se(b);
    }
    ue(b); u(b, 1);
    s->width_mbs = ue(b) + 1;
    s->height_map_units = ue(b) + 1;
    s->frame_mbs_only = u(b, 1);
    if (!s->frame_mbs_only) s->mbaff = u(b, 1);
    s->direct_8x8 = u(b, 1);
    if (u(b, 1)) { s->crop_l = ue(b); s->crop_r = ue(b); s->crop_t = ue(b); s->crop_b = ue(b); }
    return 0;
}

static int more_rbsp(Bits *b)
{
    int last = b->n * 8 - 1;
    while (last >= 0 && !(b->d[last >> 3] >> (7 - (last & 7)) & 1)) last--;   /* rbsp stop bit */
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
        if (u(b, 1)) { printf("PPS scaling matrices not handled in this test\n"); return -1; }
        p->second_chroma_qp_offset = se(b);
    }
    return 0;
}

/* b starts at the NAL header byte, as the VE sees the buffer; header_bits counts it (as FFmpeg does) */
static int parse_slice(Bits *b, const Sps *s, const Pps *p, int nal_type, int nal_ref_idc, Slice *sl)
{
    memset(sl, 0, sizeof *sl);
    u(b, 8);
    sl->first_mb = ue(b);
    sl->type = ue(b) % 5;
    ue(b);
    u(b, s->log2_max_frame_num);
    if (!s->frame_mbs_only && u(b, 1)) { printf("field pictures not handled in this test\n"); return -1; }
    if (nal_type == 5) ue(b);
    if (s->poc_type == 0) {
        u(b, s->log2_max_poc_lsb);
        if (p->bottom_field_poc_present) se(b);
    } else if (s->poc_type == 1 && !s->delta_pic_order_always_zero) {
        se(b);
        if (p->bottom_field_poc_present) se(b);
    }
    if (p->redundant_pic_cnt) ue(b);
    if (sl->type != 2) { printf("first picture is not an I slice\n"); return -1; }
    if (nal_ref_idc) {
        if (nal_type == 5) { u(b, 1); u(b, 1); }
        else if (u(b, 1)) { printf("MMCO in first slice not handled\n"); return -1; }
    }
    sl->qp_delta = se(b);
    if (p->deblock_ctrl) {
        sl->disable_deblock = ue(b);
        if (sl->disable_deblock != 1) { sl->alpha = se(b); sl->beta = se(b); }
    }
    sl->header_bits = b->pos;
    return 0;
}

/* ---------------- VE programming (cedrus_h264.c order) ---------------- */
static void sram_write(unsigned off, const uint32_t *data, int words)
{
    wr(VE_AVC_SRAM_PORT_OFFSET, off << 2);
    while (words--) wr(VE_AVC_SRAM_PORT_DATA, *data++);
}

static void skip_bits(int num)
{
    for (int count = 0; count < num;) {
        int n = num - count < 32 ? num - count : 32;
        wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_FLUSH_BITS | VE_H264_TRIGGER_TYPE_N_BITS(n));
        for (int spin = 0; rd(VE_H264_STATUS) & VE_H264_STATUS_VLD_BUSY; spin++)
            if (spin > 1000000) { printf("VLD busy timeout during bit skip\n"); return; }
        count += n;
    }
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.h264 out.nv12\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static uint8_t es[8 << 20];
    int es_len = fread(es, 1, sizeof es, f);
    fclose(f);

    /* NAL walk: SPS, PPS, then the first IDR slice */
    Sps sps; Pps pps; Slice sl;
    int have = 0, slice_off = -1, slice_len = 0, nal_type = 0, nal_ref = 0;
    static uint8_t rbsp[8 << 20];
    for (int i = 0; i + 3 < es_len && slice_off < 0;) {
        if (!(es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1)) { i++; continue; }
        int s = i + 3, e = s;
        while (e + 3 < es_len && !(es[e] == 0 && es[e + 1] == 0 && (es[e + 2] == 1 || (es[e + 2] == 0 && es[e + 3] == 1)))) e++;
        if (e + 3 >= es_len) e = es_len;
        int t = es[s] & 0x1f;
        Bits b = { rbsp, unescape(es + s, e - s, rbsp), 8 };
        if (t == 7 && parse_sps(&b, &sps) == 0) have |= 1;
        else if (t == 8 && parse_pps(&b, &pps) == 0) have |= 2;
        else if (t == 5 && have == 3) {
            b.pos = 0;
            if (parse_slice(&b, &sps, &pps, t, es[s] >> 5 & 3, &sl)) return 1;
            slice_off = s; slice_len = e - s; nal_type = t; nal_ref = es[s] >> 5 & 3;
        }
        i = e;
    }
    if (slice_off < 0) { printf("no SPS/PPS/IDR slice found\n"); return 1; }
    (void)nal_type;
    int W = sps.width_mbs * 16, H = sps.height_map_units * 16 * (2 - sps.frame_mbs_only);
    printf("stream: profile %d, %dx%d coded, crop l%d r%d t%d b%d, %s, 8x8 %d, slice %d bytes, header %d bits, qp %d\n",
           sps.profile, W, H, sps.crop_l, sps.crop_r, sps.crop_t, sps.crop_b, pps.entropy ? "CABAC" : "CAVLC",
           pps.t8x8, slice_len, sl.header_bits, 26 + pps.init_qp + sl.qp_delta);
    if (W > 2048 || !sps.frame_mbs_only || sps.chroma_format_idc != 1) { printf("outside this test's limits\n"); return 1; }

    ve_fd = open("/dev/cedar_dev", O_RDWR);
    ion_fd = open("/dev/ion", O_RDWR);
    if (ve_fd < 0 || ion_fd < 0) { printf("open /dev/cedar_dev or /dev/ion: %s\n", strerror(errno)); return 1; }
    if (pick_heap()) return 1;
    if (ioctl(ve_fd, IOCTL_ENGINE_REQ, 0) < 0) { printf("ENGINE_REQ: %s\n", strerror(errno)); return 1; }
    ioctl(ve_fd, IOCTL_ENABLE_VE, 0);
    ioctl(ve_fd, IOCTL_RESET_VE, 0);
    regs = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, ve_fd, 0);
    if (regs == MAP_FAILED) { printf("mmap regs: %s\n", strerror(errno)); return 1; }
    printf("VE id e0=0x%08x e4=0x%08x\n", rd(0xe0), rd(0xe4));

    /* buffers: sizes from cedrus_h264.c / cedrus_hw.c */
    int luma = ALIGN(W, 16) * ALIGN(H, 16), chroma = luma / 2;
    int field_size = (W / 16) * (H / 16) * 16;
    if (!sps.direct_8x8) field_size *= 2;
    if (!sps.frame_mbs_only) field_size *= 2;
    int pic_info = H264_FRAME_NUM * 0x1000 + H * 2 * 64;
    if (pic_info < 130 * 1024) pic_info = 130 * 1024;
    Buf bs = {0}, out = {0}, mv = {0}, pinfo = {0}, neigh = {0};
    int rc = 1;
    if (buf_alloc(&bs, slice_len + 1024) || buf_alloc(&out, luma + chroma) || buf_alloc(&mv, field_size * 2) ||
        buf_alloc(&pinfo, pic_info) || buf_alloc(&neigh, 32 * 1024))
        goto done;
    printf("VE addresses: bs 0x%08x out 0x%08x mv 0x%08x pinfo 0x%08x neigh 0x%08x\n", bs.ve, out.ve, mv.ve, pinfo.ve, neigh.ve);
    memcpy(bs.cpu, es + slice_off, slice_len);
    memset(out.cpu, 0x80, luma + chroma);   /* grey: shows what the engine did not write */

    double t0 = now();
    /* cedrus_engine_enable + cedrus_dst_format_set (untiled NV12, H6 family) */
    wr(VE_MODE, VE_MODE_REC_WR_MODE_2MB | VE_MODE_DDR_MODE_BW_128 | VE_MODE_DEC_H264);
    wr(VE_PRIMARY_OUT_FMT, VE_PRIMARY_OUT_FMT_NV12);
    wr(VE_PRIMARY_CHROMA_BUF_LEN, chroma / 2);
    wr(VE_PRIMARY_FB_LINE_STRIDE, (ALIGN(W, 16) / 2) << 16 | ALIGN(W, 16));
    /* cedrus_h264_setup */
    wr(VE_H264_SDROT_CTRL, 0);
    wr(VE_H264_EXTRA_BUFFER1, pinfo.ve);
    wr(VE_H264_EXTRA_BUFFER2, neigh.ve);
    uint32_t list[H264_FRAME_NUM * 8];
    memset(list, 0, sizeof list);
    /* position 0: top/bottom POC 0 (IDR), frame_info = pic_type << 8, luma, chroma, mv top, mv bottom */
    list[2] = PIC_TYPE_FRAME << 8;
    list[3] = out.ve; list[4] = out.ve + luma;
    list[5] = mv.ve;  list[6] = mv.ve + field_size;
    sram_write(SRAM_H264_FRAMEBUFFER_LIST, list, H264_FRAME_NUM * 8);
    wr(VE_H264_OUTPUT_FRAME_IDX, 0);
    /* cedrus_set_params */
    wr(VE_H264_VLD_LEN, slice_len * 8);
    wr(VE_H264_VLD_OFFSET, 0);
    wr(VE_H264_VLD_END, bs.ve + slice_len);
    wr(VE_H264_VLD_ADDR, VE_H264_VLD_ADDR_VAL(bs.ve) | VE_H264_VLD_ADDR_FIRST | VE_H264_VLD_ADDR_VALID | VE_H264_VLD_ADDR_LAST);
    wr(VE_BUF_CTRL, 0);   /* intra pred + deblock in internal SRAM (width <= 2048) */
    wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_INIT_SWDEC);
    skip_bits(sl.header_bits);
    uint32_t reg = (pps.num_ref_l0 & 0x1f) << 10 | (pps.num_ref_l1 & 0x1f) << 5 | (pps.weighted_bipred & 3) << 2;
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
    reg = (sl.first_mb % sps.width_mbs & 0xff) << 24 | (sl.first_mb / sps.width_mbs & 0xff) << 16 |
          (nal_ref ? BIT(12) : 0) | (sl.type & 0xf) << 8 | VE_H264_SHS_FIRST_SLICE_IN_PIC;
    wr(VE_H264_SHS, reg);
    wr(VE_H264_SHS2, VE_H264_SHS2_NUM_REF_IDX_ACTIVE_OVRD | (sl.disable_deblock & 3) << 8 |
                     (sl.alpha & 0xf) << 4 | (sl.beta & 0xf));
    wr(VE_H264_SHS_QP, (pps.second_chroma_qp_offset & 0x3f) << 16 | (pps.chroma_qp_offset & 0x3f) << 8 |
                       ((26 + pps.init_qp + sl.qp_delta) & 0x3f) | VE_H264_SHS_QP_SCALING_MATRIX_DEFAULT);
    wr(VE_H264_STATUS, rd(VE_H264_STATUS));
    wr(VE_H264_CTRL, VE_H264_CTRL_INT_MASK);
    /* trigger, wait for the vendor driver's H.264 IRQ path (0x220/0x228) */
    wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_AVC_SLICE_DECODE);
    int irq = ioctl(ve_fd, IOCTL_WAIT_VE_DE, 2);
    double t1 = now();
    uint32_t st = rd(VE_H264_STATUS), mbs = rd(VE_H264_CUR_MB_NUM), err = rd(VE_H264_ERROR_CASE);
    printf("WAIT_VE_DE %d, status 0x%08x (%s%s%s), cur MB %u of %d, error case 0x%08x, %.2f ms\n", irq, st,
           st & 1 ? "slice done " : "", st & 2 ? "DECODE ERROR " : "", st & 4 ? "VLD DATA REQ" : "", mbs,
           sps.width_mbs * sps.height_map_units, err, (t1 - t0) * 1e3);
    wr(VE_H264_STATUS, VE_H264_CTRL_INT_MASK);
    wr(VE_H264_CTRL, rd(VE_H264_CTRL) & ~VE_H264_CTRL_INT_MASK);
    wr(VE_MODE, VE_MODE_DISABLED);

    f = fopen(argv[2], "wb");
    if (f) { fwrite(out.cpu, 1, luma + chroma, f); fclose(f); }
    printf("RESULT %s geometry=%dx%d stride=%d crop=%d,%d,%d,%d\n", (st & 1) && !(st & 6) ? "OK" : "FAIL",
           W, H, ALIGN(W, 16), sps.crop_l * 2, sps.crop_r * 2, sps.crop_t * 2, sps.crop_b * 2);
    rc = (st & 1) && !(st & 6) ? 0 : 1;
done:
    buf_free(&neigh); buf_free(&pinfo); buf_free(&mv); buf_free(&out); buf_free(&bs);
    if (regs != MAP_FAILED && regs) munmap((void *)regs, 0x1000);
    ioctl(ve_fd, IOCTL_DISABLE_VE, 0);
    ioctl(ve_fd, IOCTL_ENGINE_REL, 0);
    close(ve_fd); close(ion_fd);
    return rc;
}
