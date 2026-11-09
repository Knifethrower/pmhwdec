// SPDX-License-Identifier: GPL-2.0-or-later
/* "cedar": Allwinner video engine on vendor 4.9 kernels (/dev/cedar_dev), without any Allwinner userspace code.
 *
 * Decode programming ported to userspace from mainline Linux Cedrus (drivers/staging/media/sunxi/cedrus:
 * cedrus_h264.c, cedrus_h265.c, cedrus_vp8.c, GPL-2.0-or-later; Copyright 2013 Jens Kuske, 2018 Paul Kocialkowski /
 * Bootlin, 2019 Jernej Skrabec).
 * Register offsets and fields: ve_regs.h (hardware facts, generated and checked by tools/gen_ve_regs.py and
 * tools/check_ve_regs.sh). Device interface of the vendor driver (drivers/media/cedar-ve/cedar_ve.c): ioctl numbers,
 * mmap of the MACC registers, IOMMU mapping of dma-bufs, IRQ wait. Memory: 4.9 ION.
 * Verified bit-exact against FFmpeg on H700 (RG Cube XX, IP 0x33010) and A133P (TrimUI Smart Pro, IP 0x30210);
 * the A133P has no 10-bit HEVC. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "codec.h"
#include "ve_regs.h"
#include "vp8_tables.h"
#include "cedar_vp8_table.h"

/* ---- cedar_ve.h (vendor 4.9), enum IOCTL_CMD ---- */
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
struct ion_allocation_data { size_t len, align; unsigned int heap_id_mask, flags; int handle; };
struct ion_fd_data { int handle; int fd; };
struct ion_handle_data { int handle; };
struct ion_custom_data { unsigned int cmd; unsigned long arg; };
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE  _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_MAP   _IOWR('I', 2, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR('I', 6, struct ion_custom_data)
#define ION_FLAG_CACHED 1
/* sunxi ION (drivers/staging/android/ion/ion-ioctl.c, sunxi_ion.h, GPL-2.0): clean + invalidate a user VA range;
 * a plain ioctl number on the 4.9 BSP, a custom sub-command on others */
struct sunxi_cache_range { long start, end; };
#define ION_IOC_SUNXI_FLUSH_RANGE 5

#define VE_IP_H616   0x33010         /* H616 / H618 / H700: HEVC Main 10 */
#define DRM_FORMAT_NV12 0x3231564e

/* cedrus_h264.c / cedrus_h265.c side buffers */
#define H264_FRAME_NUM          18
#define H264_NEIGHBOR_INFO_SIZE (32 * 1024)
#define H265_NEIGHBOR_INFO_SIZE (794 * 1024)
#define H265_ENTRY_POINTS_SIZE  (4 * 1024)
#define H265_MV_COL_UNIT_CTB    160
#define H265_OUTPUT_INDEX       16
#define VP8_ENTROPY_PROBS_SIZE  0x2400
#define VP8_PROB_HALF           128
enum { SRAM_H264_PRED_WEIGHT_TABLE = 0x000, SRAM_H264_FRAMEBUFFER_LIST = 0x100, SRAM_H264_REF_LIST_0 = 0x190,
       SRAM_H264_REF_LIST_1 = 0x199 };

/* ---------------- device: one per process ---------------- */
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;
static int dev_users, ve_fd = -1, ion_fd = -1;
static unsigned heap_mask;
static volatile uint8_t *regs;
static uint32_t ip_version;

static void wr(unsigned off, uint32_t v) { *(volatile uint32_t *)(regs + off) = v; }
static uint32_t rd(unsigned off) { return *(volatile uint32_t *)(regs + off); }

typedef struct Buf { int handle, fd; size_t size; uint8_t *cpu; uint32_t ve; } Buf;

/* picture buffers are CPU-cached when the kernel lets us flush them (uncached reads cost ~32 ms per 1080p frame) */
static int cache_mode = -1;      /* -1 unknown, 0 uncached, 1 sunxi ioctl 5, 2 ION_IOC_CUSTOM */

static int cache_flush(const void *p, size_t len)
{
    struct sunxi_cache_range r = { (long)p, (long)p + (long)len };
    if (cache_mode == 1) return ioctl(ion_fd, ION_IOC_SUNXI_FLUSH_RANGE, &r);
    struct ion_custom_data c = { ION_IOC_SUNXI_FLUSH_RANGE, (unsigned long)&r };
    return ioctl(ion_fd, ION_IOC_CUSTOM, &c);
}

static int ion_try_flags(Buf *b, size_t size, unsigned mask, unsigned flags)
{
    memset(b, 0, sizeof *b);
    b->fd = -1;
    b->size = ALIGN(size, 4096);
    struct ion_allocation_data a = { .len = b->size, .align = 4096, .heap_id_mask = mask, .flags = flags };
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

static int ion_try(Buf *b, size_t size, unsigned mask) { return ion_try_flags(b, size, mask, 0); }

static void buf_free(Buf *b)
{
    if (!b->size) return;
    if (b->ve) { struct user_iommu_param p = { .fd = b->fd, .iommu_addr = b->ve }; ioctl(ve_fd, IOCTL_FREE_IOMMU_ADDR, &p); }
    if (b->cpu) munmap(b->cpu, b->size);
    if (b->fd >= 0) close(b->fd);
    struct ion_handle_data h = { .handle = b->handle };
    ioctl(ion_fd, ION_IOC_FREE, &h);
    memset(b, 0, sizeof *b);
}

static int buf_alloc(Buf *b, size_t size)
{
    if (ion_try(b, size, heap_mask)) { ERR("cedar: buffer of %zu bytes failed: %s", size, strerror(errno)); buf_free(b); return -1; }
    return 0;
}

/* a picture buffer: cached + flushed if possible (sets *cached), else uncached */
static int pic_alloc(Buf *b, size_t size, int *cached)
{
    *cached = 0;
    if (cache_mode != 0) {
        if (ion_try_flags(b, size, heap_mask, ION_FLAG_CACHED) == 0) {
            memset(b->cpu, 0, b->size);
            if (cache_mode < 0) {                  /* find out which flush call this kernel has */
                cache_mode = 1;
                if (cache_flush(b->cpu, b->size)) { cache_mode = 2; if (cache_flush(b->cpu, b->size)) cache_mode = 0; }
                LOG("cedar: picture buffers %s", cache_mode ? "cached (sunxi ION flush)" : "uncached (no ION flush call)");
            } else if (cache_flush(b->cpu, b->size)) {
                cache_mode = 0;
            }
            if (cache_mode) { *cached = 1; return 0; }
        }
        buf_free(b);
    }
    if (buf_alloc(b, size)) return -1;
    memset(b->cpu, 0, b->size);
    return 0;
}

static int dev_get(void)
{
    pthread_mutex_lock(&dev_lock);
    if (dev_users++) { pthread_mutex_unlock(&dev_lock); return 0; }
    ve_fd = open("/dev/cedar_dev", O_RDWR | O_CLOEXEC);
    ion_fd = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (ve_fd < 0 || ion_fd < 0) goto fail;
    if (ioctl(ve_fd, IOCTL_ENGINE_REQ, 0) < 0) goto fail;
    ioctl(ve_fd, IOCTL_ENABLE_VE, 0);
    ioctl(ve_fd, IOCTL_RESET_VE, 0);
    regs = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, ve_fd, 0);
    if (regs == MAP_FAILED) { regs = NULL; ioctl(ve_fd, IOCTL_ENGINE_REL, 0); goto fail; }
    ip_version = rd(0xe0);
    /* first ION heap that allocates and maps for the engine (sunxi ION may lack the heap query) */
    for (int id = 0; id < 16 && !heap_mask; id++) {
        Buf t;
        if (ion_try(&t, 4096, 1u << id) == 0) heap_mask = 1u << id;
        buf_free(&t);
    }
    if (!heap_mask) { ERR("cedar: no ION heap maps for the video engine"); goto fail_regs; }
    if (getenv("PMHWDEC_CACHED") && !strcmp(getenv("PMHWDEC_CACHED"), "0")) cache_mode = 0;
    LOG("cedar: engine IP 0x%05x (0xe4 0x%05x), ION heap mask 0x%x", ip_version, rd(0xe4), heap_mask);
    pthread_mutex_unlock(&dev_lock);
    return 0;
fail_regs:
    munmap((void *)regs, 0x1000); regs = NULL;
    ioctl(ve_fd, IOCTL_DISABLE_VE, 0);
    ioctl(ve_fd, IOCTL_ENGINE_REL, 0);
fail:
    LOG("cedar: device not usable: %s", strerror(errno));
    if (ve_fd >= 0) close(ve_fd);
    if (ion_fd >= 0) close(ion_fd);
    ve_fd = ion_fd = -1;
    dev_users = 0;
    pthread_mutex_unlock(&dev_lock);
    return -1;
}

static void dev_put(void)
{
    pthread_mutex_lock(&dev_lock);
    if (dev_users && !--dev_users) {
        wr(VE_MODE, VE_MODE_DISABLED);
        munmap((void *)regs, 0x1000); regs = NULL;
        ioctl(ve_fd, IOCTL_DISABLE_VE, 0);
        ioctl(ve_fd, IOCTL_ENGINE_REL, 0);
        close(ve_fd); close(ion_fd);
        ve_fd = ion_fd = -1;
        heap_mask = 0;
    }
    pthread_mutex_unlock(&dev_lock);
}

/* ---------------- capabilities ---------------- */
static int cedar_query(PmhwCodec codec, PmhwCaps *caps)
{
    if (codec != PMHW_CODEC_H264 && codec != PMHW_CODEC_HEVC && codec != PMHW_CODEC_VP8) return -1;
    if (access("/dev/cedar_dev", R_OK | W_OK) || access("/dev/ion", R_OK | W_OK)) return -1;
    if (!ip_version) { if (dev_get()) return -1; dev_put(); }
    /* Cedrus H6 family limits; the VE generation check: 0xf0 reads 0 and the IP sits in 0xe0 on these chips */
    if (ip_version < 0x30000) return -1;
    caps->max_width = 4096;
    caps->max_height = 2304;
    caps->max_bit_depth = codec == PMHW_CODEC_HEVC && ip_version == VE_IP_H616 ? 10 : 8;
    caps->backend = "cedar";
    return 0;
}

/* ---------------- per decoder ---------------- */
typedef struct Pic { Buf mem; uint32_t luma, chroma, mv, mv2; int cached; } Pic;

typedef struct Cedar {
    PmhwCodec codec;
    Geom g;
    Pic pic[MAX_SLOTS];
    Buf bs, pinfo, neigh, deblk, intra, ep, probs;
    int vp8_last_simple, vp8_last_p, vp8_last_sharpness;     /* the loop filter settings of the last filtered frame */
} Cedar;

static void *cedar_open(PmhwCodec codec)
{
    if (dev_get()) return NULL;
    Cedar *c = calloc(1, sizeof *c);
    if (!c) { dev_put(); return NULL; }
    c->codec = codec;
    return c;
}

static void free_bufs(Cedar *c)
{
    for (int i = 0; i < MAX_SLOTS; i++) buf_free(&c->pic[i].mem);
    buf_free(&c->pinfo); buf_free(&c->neigh); buf_free(&c->deblk); buf_free(&c->intra); buf_free(&c->ep); buf_free(&c->probs);
}

unsigned pmhw_cedar_gen;            /* pmhwdec-cedard: tells the client its old picture buffers are gone */

static int cedar_configure(void *hw, const Geom *g)
{
    Cedar *c = hw;
    free_bufs(c);
    pmhw_cedar_gen++;
    c->g = *g;
    int nv12 = g->stride * g->coded_h * 3 / 2, extra = 0, mv = 0;
    if (g->codec == PMHW_CODEC_H264) {
        int field = DIV_ROUND_UP(g->width, 16) * DIV_ROUND_UP(g->height, 16) * 16;
        if (!g->h264_direct_8x8) field *= 2;
        if (!g->h264_frame_mbs_only) field *= 2;
        mv = field * 2;
        int pinfo = H264_FRAME_NUM * (g->width > 2048 ? 0x4000 : 0x1000) + g->height * 2 * 64;
        if (pinfo < 130 * 1024) pinfo = 130 * 1024;
        if (buf_alloc(&c->pinfo, pinfo) || buf_alloc(&c->neigh, H264_NEIGHBOR_INFO_SIZE)) goto fail;
        if (g->width > 2048 && (buf_alloc(&c->deblk, ALIGN(g->width, 32) * 12) || buf_alloc(&c->intra, ALIGN(g->width, 64) * 5 * 2))) goto fail;
    } else if (g->codec == PMHW_CODEC_VP8) {
        if (buf_alloc(&c->probs, VP8_ENTROPY_PROBS_SIZE)) goto fail;
        memset(c->probs.cpu, 0, VP8_ENTROPY_PROBS_SIZE);
        memcpy(c->probs.cpu + 2048, cedar_vp8_prob_init, sizeof cedar_vp8_prob_init);
        c->vp8_last_simple = c->vp8_last_p = c->vp8_last_sharpness = 0;
    } else {
        int ctb = 1 << g->hevc_ctb_log2;
        mv = DIV_ROUND_UP(g->width, ctb) * DIV_ROUND_UP(g->height, ctb) * H265_MV_COL_UNIT_CTB + 1024;
        if (g->bit_depth > 8) extra = ALIGN(g->stride / 4, 32) * g->coded_h * 3 / 2;   /* the 2-bit plane */
        if (buf_alloc(&c->neigh, H265_NEIGHBOR_INFO_SIZE) || buf_alloc(&c->ep, H265_ENTRY_POINTS_SIZE)) goto fail;
    }
    for (int i = 0; i < g->nslots; i++) {
        Pic *p = &c->pic[i];
        if (pic_alloc(&p->mem, nv12 + extra + ALIGN(mv, 1024) + 1024, &p->cached)) goto fail;
        p->luma = p->mem.ve;
        p->chroma = p->mem.ve + g->stride * g->coded_h;
        p->mv = ALIGN(p->mem.ve + nv12 + extra, 1024);
        p->mv2 = p->mv + mv / 2;                    /* H.264: bottom field half */
    }
    if (!c->bs.size && buf_alloc(&c->bs, 1 << 20)) goto fail;
    return 0;
fail:
    free_bufs(c);
    return -1;
}

static int ensure_bs(Cedar *c, int len)
{
    if ((size_t)len + 64 <= c->bs.size) return 0;
    buf_free(&c->bs);
    return buf_alloc(&c->bs, ALIGN(len + 64, 1 << 20));
}

static void output_format(const Cedar *c, uint32_t mode)
{
    if (c->g.width == 4096) mode |= VE_MODE_PIC_WIDTH_IS_4096;
    if (c->g.width > 2048) mode |= VE_MODE_PIC_WIDTH_MORE_2048;
    wr(VE_MODE, mode | VE_MODE_REC_WR_MODE_2MB | VE_MODE_DDR_MODE_BW_128);
    wr(VE_PRIMARY_OUT_FMT, VE_PRIMARY_OUT_FMT_NV12);
    wr(VE_PRIMARY_CHROMA_BUF_LEN, c->g.stride * c->g.coded_h / 4);
    wr(VE_PRIMARY_FB_LINE_STRIDE, VE_PRIMARY_FB_LINE_STRIDE_LUMA(c->g.stride) | VE_PRIMARY_FB_LINE_STRIDE_CHROMA(c->g.stride / 2));
}

static int skip_bits(unsigned trigger, unsigned status, unsigned busy, unsigned flush, int num)
{
    for (int count = 0; count < num;) {
        int n = num - count < 32 ? num - count : 32;
        wr(trigger, flush | ((n & 0x3f) << 8));
        for (int spin = 0; rd(status) & busy; spin++)
            if (spin > 1000000) { ERR("cedar: bit skip timed out"); return -1; }
        count += n;
    }
    return 0;
}

/* runs the engine; 0 ok, 1 decoded with errors, -1 hung (engine reset) */
static int run(unsigned trigger, uint32_t go, unsigned status, uint32_t ok_bit, uint32_t err_bits, unsigned ctrl, uint32_t irq_mask)
{
    wr(trigger, go);
    int irq = ioctl(ve_fd, IOCTL_WAIT_VE_DE, 2);
    uint32_t st = rd(status);
    wr(status, irq_mask);
    wr(ctrl, rd(ctrl) & ~irq_mask);
    if (irq != 1) {
        ERR("cedar: engine timed out (status 0x%08x), resetting", st);
        ioctl(ve_fd, IOCTL_RESET_VE, 0);
        return -1;
    }
    if (!(st & ok_bit) || (st & err_bits)) { LOG("cedar: decode error, status 0x%08x", st); return 1; }
    return 0;
}

/* ---------------- H.264 (cedrus_h264.c) ---------------- */
static void sram_write(unsigned off, const void *data, int bytes)
{
    const uint32_t *w = data;
    wr(VE_AVC_SRAM_PORT_OFFSET, off << 2);
    for (int i = 0; i < (bytes + 3) / 4; i++) wr(VE_AVC_SRAM_PORT_DATA, w[i]);
}

static void h264_ref_entry(uint32_t *e, const Pic *p, int poc)
{
    e[0] = poc; e[1] = poc;                         /* frame: top = bottom field order count */
    e[2] = 0 << 8;                                  /* CEDRUS_H264_PIC_TYPE_FRAME */
    e[3] = p->luma; e[4] = p->chroma; e[5] = p->mv; e[6] = p->mv2; e[7] = 0;
}

static int cedar_h264_slice(void *hw, const H264Job *j)
{
    Cedar *c = hw;
    const H264Sps *sps = j->sps; const H264Pps *pps = j->pps; const H264Slice *sl = j->sl;
    if (ensure_bs(c, j->nal_len)) return -1;
    memcpy(c->bs.cpu, j->nal, j->nal_len);
    __sync_synchronize();
    pthread_mutex_lock(&dev_lock);
    output_format(c, VE_MODE_DEC_H264);
    wr(VE_H264_SDROT_CTRL, 0);
    wr(VE_H264_EXTRA_BUFFER1, c->pinfo.ve);
    wr(VE_H264_EXTRA_BUFFER2, c->neigh.ve);
    uint32_t fl[H264_FRAME_NUM][8];
    memset(fl, 0, sizeof fl);
    for (int i = 0; i < j->nslots; i++) if (j->slots[i].ref && i != j->cur) h264_ref_entry(fl[i], &c->pic[i], j->slots[i].poc);
    h264_ref_entry(fl[j->cur], &c->pic[j->cur], j->cur_poc);
    sram_write(SRAM_H264_FRAMEBUFFER_LIST, fl, sizeof fl);
    wr(VE_H264_OUTPUT_FRAME_IDX, j->cur);
    wr(VE_H264_VLD_LEN, j->nal_len * 8);
    wr(VE_H264_VLD_OFFSET, 0);
    wr(VE_H264_VLD_END, c->bs.ve + j->nal_len);
    wr(VE_H264_VLD_ADDR, VE_H264_VLD_ADDR_VAL(c->bs.ve) | VE_H264_VLD_ADDR_FIRST | VE_H264_VLD_ADDR_VALID | VE_H264_VLD_ADDR_LAST);
    if (c->g.width > 2048) {
        wr(VE_BUF_CTRL, VE_BUF_CTRL_INTRAPRED_MIXED_RAM | VE_BUF_CTRL_DBLK_MIXED_RAM);
        wr(VE_DBLK_DRAM_BUF_ADDR, c->deblk.ve);
        wr(VE_INTRAPRED_DRAM_BUF_ADDR, c->intra.ve);
    } else {
        wr(VE_BUF_CTRL, VE_BUF_CTRL_INTRAPRED_INT_SRAM | VE_BUF_CTRL_DBLK_INT_SRAM);
    }
    wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_INIT_SWDEC);
    if (skip_bits(VE_H264_TRIGGER_TYPE, VE_H264_STATUS, VE_H264_STATUS_VLD_BUSY, VE_H264_TRIGGER_TYPE_FLUSH_BITS, sl->header_bits)) {
        pthread_mutex_unlock(&dev_lock);
        return -1;
    }
    if ((pps->weighted_pred && sl->type == H264_P) || (pps->weighted_bipred == 1 && sl->type == H264_B)) {
        wr(VE_H264_SHS_WP, (sl->chroma_denom & 7) << 4 | (sl->luma_denom & 7));
        wr(VE_AVC_SRAM_PORT_OFFSET, SRAM_H264_PRED_WEIGHT_TABLE << 2);
        for (int l = 0; l < 2; l++) {
            for (int i = 0; i < 32; i++) wr(VE_AVC_SRAM_PORT_DATA, ((uint32_t)sl->lo[l][i] & 0x1ff) << 16 | (sl->lw[l][i] & 0x1ff));
            for (int i = 0; i < 32; i++)
                for (int k = 0; k < 2; k++)
                    wr(VE_AVC_SRAM_PORT_DATA, ((uint32_t)sl->co[l][i][k] & 0x1ff) << 16 | (sl->cw[l][i][k] & 0x1ff));
        }
    }
    for (int l = 0; l < (sl->type == H264_B ? 2 : sl->type == H264_P ? 1 : 0); l++) {
        uint8_t ra[32];
        memset(ra, 0, sizeof ra);
        for (int i = 0; i < j->len[l] && i < 32; i++) ra[i] = j->list[l][i] << 1;
        int active = sl->num_ref[l] + 1;
        sram_write(l ? SRAM_H264_REF_LIST_1 : SRAM_H264_REF_LIST_0, ra, ALIGN(active, 4) < 32 ? ALIGN(active, 4) : 32);
    }
    uint32_t reg = (sl->num_ref[0] & 0x1f) << 10 | (sl->num_ref[1] & 0x1f) << 5 | (pps->weighted_bipred & 3) << 2;
    if (pps->entropy) reg |= VE_H264_PPS_ENTROPY_CODING_MODE;
    if (pps->weighted_pred) reg |= VE_H264_PPS_WEIGHTED_PRED;
    if (pps->constrained_intra) reg |= VE_H264_PPS_CONSTRAINED_INTRA_PRED;
    if (pps->t8x8) reg |= VE_H264_PPS_TRANSFORM_8X8_MODE;
    wr(VE_H264_PPS, reg);
    reg = (sps->chroma_format_idc & 7) << 19 | ((sps->width_mbs - 1) & 0xff) << 8 | ((sps->height_map_units - 1) & 0xff);
    if (sps->frame_mbs_only) reg |= VE_H264_SPS_MBS_ONLY;
    if (sps->mbaff) reg |= VE_H264_SPS_MB_ADAPTIVE_FRAME_FIELD;
    if (sps->direct_8x8) reg |= VE_H264_SPS_DIRECT_8X8_INFERENCE;
    wr(VE_H264_SPS, reg);
    reg = (sl->first_mb % sps->width_mbs & 0xff) << 24 | (sl->first_mb / sps->width_mbs & 0xff) << 16 |
          (sl->nal_ref_idc ? BIT(12) : 0) | (sl->type & 0xf) << 8 | (sl->cabac_init & 3);
    if (j->first_slice) reg |= VE_H264_SHS_FIRST_SLICE_IN_PIC;
    if (sl->direct_spatial) reg |= VE_H264_SHS_DIRECT_SPATIAL_MV_PRED;
    wr(VE_H264_SHS, reg);
    wr(VE_H264_SHS2, VE_H264_SHS2_NUM_REF_IDX_ACTIVE_OVRD | (sl->num_ref[0] & 0x1f) << 24 | (sl->num_ref[1] & 0x1f) << 16 |
                     (sl->disable_deblock & 3) << 8 | (sl->alpha & 0xf) << 4 | (sl->beta & 0xf));
    wr(VE_H264_SHS_QP, (pps->second_chroma_qp_offset & 0x3f) << 16 | (pps->chroma_qp_offset & 0x3f) << 8 |
                       ((26 + pps->init_qp + sl->qp_delta) & 0x3f) | VE_H264_SHS_QP_SCALING_MATRIX_DEFAULT);
    wr(VE_H264_STATUS, rd(VE_H264_STATUS));
    wr(VE_H264_CTRL, VE_H264_CTRL_SLICE_DECODE_INT | VE_H264_CTRL_DECODE_ERR_INT | VE_H264_CTRL_VLD_DATA_REQ_INT);
    int r = run(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_AVC_SLICE_DECODE, VE_H264_STATUS, VE_H264_STATUS_SLICE_DECODE_INT,
                VE_H264_STATUS_DECODE_ERR_INT | VE_H264_STATUS_VLD_DATA_REQ_INT, VE_H264_CTRL, VE_H264_STATUS_INT_MASK);
    pthread_mutex_unlock(&dev_lock);
    return r;
}

/* ---------------- HEVC (cedrus_h265.c) ---------------- */
static void h265_frame_info(int index, const Pic *p, int poc)
{
    uint32_t fi[6] = { (uint32_t)poc, (uint32_t)poc, VE_DEC_H265_SRAM_DATA_ADDR_BASE(p->mv), VE_DEC_H265_SRAM_DATA_ADDR_BASE(p->mv),
                       VE_DEC_H265_SRAM_DATA_ADDR_BASE(p->luma), VE_DEC_H265_SRAM_DATA_ADDR_BASE(p->chroma) };
    wr(VE_DEC_H265_SRAM_OFFSET, VE_DEC_H265_SRAM_OFFSET_FRAME_INFO + VE_DEC_H265_SRAM_OFFSET_FRAME_INFO_UNIT * index);
    for (int i = 0; i < 6; i++) wr(VE_DEC_H265_SRAM_DATA, fi[i]);
}

static void h265_ref_list(const int *list, int n, uint32_t off)
{
    uint32_t word = 0;
    wr(VE_DEC_H265_SRAM_OFFSET, off);
    for (int i = 0; i < n; i++) {
        word |= (uint32_t)(list[i] & 0x7f) << (i % 4) * 8;
        if (i % 4 == 3 || i == n - 1) { wr(VE_DEC_H265_SRAM_DATA, word); word = 0; }
    }
}

static void h265_weights(const int8_t *dlw, const int8_t *lo, const int8_t (*dcw)[2], const int8_t (*co)[2], int n,
                         uint32_t lum, uint32_t chr)
{
    uint8_t pw[4] = {0};
    wr(VE_DEC_H265_SRAM_OFFSET, lum);
    for (int i = 0; i < n; i++) {
        pw[(i % 2) * 2] = dlw[i]; pw[(i % 2) * 2 + 1] = lo[i];
        if (i % 2 == 1 || i == n - 1) { uint32_t w; memcpy(&w, pw, 4); wr(VE_DEC_H265_SRAM_DATA, w); memset(pw, 0, 4); }
    }
    wr(VE_DEC_H265_SRAM_OFFSET, chr);
    for (int i = 0; i < n; i++) {
        uint8_t v[4] = { (uint8_t)dcw[i][0], (uint8_t)co[i][0], (uint8_t)dcw[i][1], (uint8_t)co[i][1] };
        uint32_t w; memcpy(&w, v, 4); wr(VE_DEC_H265_SRAM_DATA, w);
    }
}

static int cedar_hevc_slice(void *hw, const HevcJob *j)
{
    Cedar *c = hw;
    const HevcSps *sps = j->sps; const HevcPps *pps = j->pps; const HevcSlice *sl = j->sl;
    if (ensure_bs(c, j->nal_len)) return -1;
    memcpy(c->bs.cpu, j->nal, j->nal_len);
    if (sl->num_ep) memcpy(c->ep.cpu, j->ep_unescaped, sl->num_ep * sizeof(uint32_t));
    __sync_synchronize();
    pthread_mutex_lock(&dev_lock);
    output_format(c, VE_MODE_DEC_H265);
    wr(VE_DEC_H265_BITS_OFFSET, 0);
    wr(VE_DEC_H265_BITS_LEN, j->nal_len * 8);
    wr(VE_DEC_H265_BITS_ADDR, VE_DEC_H265_BITS_ADDR_BASE(c->bs.ve) | VE_DEC_H265_BITS_ADDR_VALID_SLICE_DATA |
                              VE_DEC_H265_BITS_ADDR_LAST_SLICE_DATA | VE_DEC_H265_BITS_ADDR_FIRST_SLICE_DATA);
    wr(VE_DEC_H265_BITS_END_ADDR, VE_DEC_H265_BITS_END_ADDR_BASE(c->bs.ve + j->nal_len));
    int ctb_w = DIV_ROUND_UP(sps->width, 1 << c->g.hevc_ctb_log2);
    wr(VE_DEC_H265_DEC_CTB_ADDR, VE_DEC_H265_DEC_CTB_ADDR_X(sl->address % ctb_w) | VE_DEC_H265_DEC_CTB_ADDR_Y(sl->address / ctb_w));
    wr(VE_DEC_H265_TILE_START_CTB, 0);
    wr(VE_DEC_H265_TILE_END_CTB, 0);
    if (j->first_slice) wr(VE_DEC_H265_DEC_CTB_NUM, 0);
    wr(VE_DEC_H265_TRIGGER, VE_DEC_H265_TRIGGER_INIT_SWDEC);
    /* to the end of the slice header, before byte_alignment(): where Cedrus' skip + show_bits leaves the reader */
    if (skip_bits(VE_DEC_H265_TRIGGER, VE_DEC_H265_STATUS, VE_DEC_H265_STATUS_VLD_BUSY, VE_DEC_H265_TRIGGER_FLUSH_BITS, sl->header_bits)) {
        pthread_mutex_unlock(&dev_lock);
        return -1;
    }
    wr(VE_DEC_H265_DEC_NAL_HDR, VE_DEC_H265_DEC_NAL_HDR_NAL_UNIT_TYPE(sl->nal_type) |
                                VE_DEC_H265_DEC_NAL_HDR_NUH_TEMPORAL_ID_PLUS1(sl->tid + 1));
    uint32_t reg = VE_DEC_H265_DEC_SPS_HDR_MAX_TRANSFORM_HIERARCHY_DEPTH_INTRA(sps->depth_intra) |
        VE_DEC_H265_DEC_SPS_HDR_MAX_TRANSFORM_HIERARCHY_DEPTH_INTER(sps->depth_inter) |
        VE_DEC_H265_DEC_SPS_HDR_LOG2_DIFF_MAX_MIN_TRANSFORM_BLOCK_SIZE(sps->log2_diff_tb) |
        VE_DEC_H265_DEC_SPS_HDR_LOG2_MIN_TRANSFORM_BLOCK_SIZE_MINUS2(sps->log2_min_tb - 2) |
        VE_DEC_H265_DEC_SPS_HDR_LOG2_DIFF_MAX_MIN_LUMA_CODING_BLOCK_SIZE(sps->log2_diff_cb) |
        VE_DEC_H265_DEC_SPS_HDR_LOG2_MIN_LUMA_CODING_BLOCK_SIZE_MINUS3(sps->log2_min_cb - 3) |
        VE_DEC_H265_DEC_SPS_HDR_BIT_DEPTH_CHROMA_MINUS8(sps->bit_depth_c - 8) |
        VE_DEC_H265_DEC_SPS_HDR_BIT_DEPTH_LUMA_MINUS8(sps->bit_depth - 8) |
        VE_DEC_H265_DEC_SPS_HDR_CHROMA_FORMAT_IDC(sps->chroma_format_idc);
    if (sps->strong_intra) reg |= VE_DEC_H265_DEC_SPS_HDR_FLAG_STRONG_INTRA_SMOOTHING_ENABLE;
    if (sps->temporal_mvp) reg |= VE_DEC_H265_DEC_SPS_HDR_FLAG_SPS_TEMPORAL_MVP_ENABLED;
    if (sps->sao) reg |= VE_DEC_H265_DEC_SPS_HDR_FLAG_SAMPLE_ADAPTIVE_OFFSET_ENABLED;
    if (sps->amp) reg |= VE_DEC_H265_DEC_SPS_HDR_FLAG_AMP_ENABLED;
    wr(VE_DEC_H265_DEC_SPS_HDR, reg);
    reg = 0;
    if (sps->pcm)
        reg = VE_DEC_H265_DEC_PCM_CTRL_LOG2_DIFF_MAX_MIN_PCM_LUMA_CODING_BLOCK_SIZE(sps->log2_diff_pcm) |
              VE_DEC_H265_DEC_PCM_CTRL_LOG2_MIN_PCM_LUMA_CODING_BLOCK_SIZE_MINUS3(sps->log2_min_pcm - 3) |
              VE_DEC_H265_DEC_PCM_CTRL_PCM_SAMPLE_BIT_DEPTH_CHROMA_MINUS1(sps->pcm_bd_c - 1) |
              VE_DEC_H265_DEC_PCM_CTRL_PCM_SAMPLE_BIT_DEPTH_LUMA_MINUS1(sps->pcm_bd - 1) |
              VE_DEC_H265_DEC_PCM_CTRL_FLAG_PCM_ENABLED |
              (sps->pcm_lf_disabled ? VE_DEC_H265_DEC_PCM_CTRL_FLAG_PCM_LOOP_FILTER_DISABLED : 0);
    wr(VE_DEC_H265_DEC_PCM_CTRL, reg);
    reg = VE_DEC_H265_DEC_PPS_CTRL0_PPS_CR_QP_OFFSET(pps->cr_qp) | VE_DEC_H265_DEC_PPS_CTRL0_PPS_CB_QP_OFFSET(pps->cb_qp) |
          VE_DEC_H265_DEC_PPS_CTRL0_INIT_QP_MINUS26(pps->init_qp) |
          VE_DEC_H265_DEC_PPS_CTRL0_DIFF_CU_QP_DELTA_DEPTH(pps->diff_cu_qp_delta_depth);
    if (pps->cu_qp_delta) reg |= VE_DEC_H265_DEC_PPS_CTRL0_FLAG_CU_QP_DELTA_ENABLED;
    if (pps->transform_skip) reg |= VE_DEC_H265_DEC_PPS_CTRL0_FLAG_TRANSFORM_SKIP_ENABLED;
    if (pps->constrained_intra) reg |= VE_DEC_H265_DEC_PPS_CTRL0_FLAG_CONSTRAINED_INTRA_PRED;
    if (pps->sign_hiding) reg |= VE_DEC_H265_DEC_PPS_CTRL0_FLAG_SIGN_DATA_HIDING_ENABLED;
    wr(VE_DEC_H265_DEC_PPS_CTRL0, reg);
    reg = VE_DEC_H265_DEC_PPS_CTRL1_LOG2_PARALLEL_MERGE_LEVEL_MINUS2(pps->log2_pml);
    if (pps->lf_across_slices) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED;
    if (pps->wpp) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_ENTROPY_CODING_SYNC_ENABLED;
    if (pps->transquant_bypass) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_TRANSQUANT_BYPASS_ENABLED;
    if (pps->weighted_bipred) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_WEIGHTED_BIPRED;
    if (pps->weighted_pred) reg |= VE_DEC_H265_DEC_PPS_CTRL1_FLAG_WEIGHTED_PRED;
    wr(VE_DEC_H265_DEC_PPS_CTRL1, reg);
    reg = VE_DEC_H265_DEC_SLICE_HDR_INFO0_PICTURE_TYPE(0) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_FIVE_MINUS_MAX_NUM_MERGE_CAND(sl->max_merge) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_NUM_REF_IDX_L1_ACTIVE_MINUS1(sl->num_ref[1]) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_NUM_REF_IDX_L0_ACTIVE_MINUS1(sl->num_ref[0]) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_COLLOCATED_REF_IDX(sl->col_ref_idx) |
          VE_DEC_H265_DEC_SLICE_HDR_INFO0_SLICE_TYPE(sl->type);
    if (j->first_slice) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_FIRST_SLICE_SEGMENT_IN_PIC;
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
    if (sl->type != HEVC_I) {
        /* as cedrus_h265_is_low_delay(): the flag is set when no reference comes after the current picture */
        int future = 0;
        for (int i = 0; i <= sl->num_ref[0]; i++) if (j->slots[j->l0[i]].poc > j->cur_poc) future = 1;
        if (sl->type == HEVC_B) for (int i = 0; i <= sl->num_ref[1]; i++) if (j->slots[j->l1[i]].poc > j->cur_poc) future = 1;
        if (!future) reg |= VE_DEC_H265_DEC_SLICE_HDR_INFO1_FLAG_SLICE_NOT_LOW_DELAY;
    }
    wr(VE_DEC_H265_DEC_SLICE_HDR_INFO1, reg);
    wr(VE_DEC_H265_DEC_SLICE_HDR_INFO2, VE_DEC_H265_DEC_SLICE_HDR_INFO2_NUM_ENTRY_POINT_OFFSETS(sl->num_ep) |
       VE_DEC_H265_DEC_SLICE_HDR_INFO2_CHROMA_LOG2_WEIGHT_DENOM(sl->chroma_denom) |
       VE_DEC_H265_DEC_SLICE_HDR_INFO2_LUMA_LOG2_WEIGHT_DENOM(sl->luma_denom));
    wr(VE_DEC_H265_ENTRY_POINT_OFFSET_ADDR, c->ep.ve >> 8);
    wr(VE_DEC_H265_DEC_PIC_SIZE, VE_DEC_H265_DEC_PIC_SIZE_WIDTH(sps->width) | VE_DEC_H265_DEC_PIC_SIZE_HEIGHT(sps->height));
    wr(VE_DEC_H265_SCALING_LIST_CTRL0, VE_DEC_H265_SCALING_LIST_CTRL0_DEFAULT);
    wr(VE_DEC_H265_NEIGHBOR_INFO_ADDR, VE_DEC_H265_NEIGHBOR_INFO_ADDR_BASE(c->neigh.ve));
    for (int i = 0; i < j->nslots && i < 16; i++) if (j->slots[i].ref && i != j->cur) h265_frame_info(i, &c->pic[i], j->slots[i].poc);
    h265_frame_info(H265_OUTPUT_INDEX, &c->pic[j->cur], j->cur_poc);
    wr(VE_DEC_H265_OUTPUT_FRAME_IDX, H265_OUTPUT_INDEX);
    if (sl->type != HEVC_I) {
        h265_ref_list(j->l0, sl->num_ref[0] + 1, VE_DEC_H265_SRAM_OFFSET_REF_PIC_LIST0);
        if (pps->weighted_pred || pps->weighted_bipred)
            h265_weights(sl->dlw[0], sl->lo[0], sl->dcw[0], sl->co[0], sl->num_ref[0] + 1,
                         VE_DEC_H265_SRAM_OFFSET_PRED_WEIGHT_LUMA_L0, VE_DEC_H265_SRAM_OFFSET_PRED_WEIGHT_CHROMA_L0);
    }
    if (sl->type == HEVC_B) {
        h265_ref_list(j->l1, sl->num_ref[1] + 1, VE_DEC_H265_SRAM_OFFSET_REF_PIC_LIST1);
        if (pps->weighted_bipred)
            h265_weights(sl->dlw[1], sl->lo[1], sl->dcw[1], sl->co[1], sl->num_ref[1] + 1,
                         VE_DEC_H265_SRAM_OFFSET_PRED_WEIGHT_LUMA_L1, VE_DEC_H265_SRAM_OFFSET_PRED_WEIGHT_CHROMA_L1);
    }
    if (sps->bit_depth > 8) {
        wr(VE_DEC_H265_OFFSET_ADDR_FIRST_OUT, c->g.stride * c->g.coded_h * 3 / 2);
        wr(VE_DEC_H265_10BIT_CONFIGURE, VE_DEC_H265_10BIT_CONFIGURE_FIRST_2BIT_STRIDE(ALIGN(c->g.stride / 4, 32)));
    }
    wr(VE_DEC_H265_STATUS, VE_DEC_H265_STATUS_CHECK_MASK);
    wr(VE_DEC_H265_CTRL, VE_DEC_H265_CTRL_IRQ_MASK);
    int r = run(VE_DEC_H265_TRIGGER, VE_DEC_H265_TRIGGER_DEC_SLICE, VE_DEC_H265_STATUS, VE_DEC_H265_STATUS_SUCCESS,
                VE_DEC_H265_STATUS_CHECK_ERROR, VE_DEC_H265_CTRL, VE_DEC_H265_CTRL_IRQ_MASK);
    pthread_mutex_unlock(&dev_lock);
    return r;
}

/* ---------------- VP8 (cedrus_vp8.c) ---------------- */
static int vp8_hw_error;

static void vp8_wait(uint32_t busy)
{
    for (int spin = 0; rd(VE_H264_STATUS) & busy; spin++)
        if (spin > 1000000) { if (!vp8_hw_error) ERR("cedar: VP8 header read timed out"); vp8_hw_error = 1; return; }
}

static uint32_t vp8_bits_p(int n, int prob)
{
    wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_VP8_GET_BITS | VE_H264_TRIGGER_TYPE_BIN_LENS(n) | VE_H264_TRIGGER_TYPE_PROBABILITY(prob));
    vp8_wait(VE_H264_STATUS_VLD_BUSY);
    return rd(VE_H264_BASIC_BITS);
}
static uint32_t vp8_bits(int n) { return vp8_bits_p(n, VP8_PROB_HALF); }

static void vp8_skip_optional(int n) { if (vp8_bits(1)) { vp8_bits(n); vp8_bits(1); } }

/* The engine has to read the frame header itself, or the picture comes out wrong (cedrus_vp8.c: the probability
 * update it triggers seems to set internal state); the values read are not used. */
static void vp8_read_header(int key)
{
    if (key) { vp8_bits(1); vp8_bits(1); }
    if (vp8_bits(1)) {                              /* segmentation */
        int update_map = vp8_bits(1);
        if (vp8_bits(1)) {
            vp8_bits(1);
            for (int i = 0; i < 4; i++) vp8_skip_optional(7);
            for (int i = 0; i < 4; i++) vp8_skip_optional(6);
        }
        if (update_map) for (int i = 0; i < 3; i++) if (vp8_bits(1)) vp8_bits(8);
    }
    vp8_bits(1); vp8_bits(6); vp8_bits(3);
    if (vp8_bits(1) && vp8_bits(1)) {               /* loop filter deltas: enabled, updated */
        for (int i = 0; i < 8; i++) vp8_skip_optional(6);
    }
    vp8_bits(2);
    vp8_bits(7);
    for (int i = 0; i < 5; i++) vp8_skip_optional(4);
    if (!key) {
        int refresh_golden = vp8_bits(1), refresh_alt = vp8_bits(1);
        if (!refresh_golden) vp8_bits(2);
        if (!refresh_alt) vp8_bits(2);
        vp8_bits(1); vp8_bits(1);
    }
    vp8_bits(1);
    if (!key) vp8_bits(1);
    wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_VP8_UPDATE_COEF);
    vp8_wait(VE_H264_STATUS_VP8_UPPROB_BUSY);
    wr(VE_H264_STATUS, VE_H264_STATUS_INT_MASK);
    if (vp8_bits(1)) vp8_bits(8);
    if (!key) {
        vp8_bits(8); vp8_bits(8); vp8_bits(8);
        if (vp8_bits(1)) for (int i = 0; i < 4; i++) vp8_bits(8);
        if (vp8_bits(1)) for (int i = 0; i < 3; i++) vp8_bits(8);
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 19; j++)
                if (vp8_bits_p(1, vp8_mv_update_probs[i][j])) vp8_bits(7);
    }
}

static void vp8_update_probs(uint8_t *t, const Vp8Hdr *h)
{
    memcpy(t + 0x1008, h->e.y_mode, sizeof h->e.y_mode);
    memcpy(t + 0x1010, h->e.uv_mode, sizeof h->e.uv_mode);
    memcpy(t + 0x1018, h->seg_probs, sizeof h->seg_probs);
    t[0x101c] = h->prob_skip;
    t[0x101d] = h->prob_intra;
    t[0x101e] = h->prob_last;
    t[0x101f] = h->prob_gf;
    memcpy(t + 0x1020, h->e.mv[0], 19);
    memcpy(t + 0x1040, h->e.mv[1], 19);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                memcpy(t + i * 512 + j * 64 + k * 16, h->e.coeff[i][j][k], 11);
}

static void vp8_ref(const Cedar *c, int slot, unsigned luma, unsigned chroma)
{
    wr(luma, slot >= 0 ? c->pic[slot].luma : 0);
    wr(chroma, slot >= 0 ? c->pic[slot].chroma : 0);
}

static int cedar_vp8_frame(void *hw, const Vp8Job *j)
{
    Cedar *c = hw;
    const Vp8Hdr *h = j->h;
    if (ensure_bs(c, j->len)) return -1;
    memcpy(c->bs.cpu, j->data, j->len);
    vp8_update_probs(c->probs.cpu, h);
    __sync_synchronize();
    pthread_mutex_lock(&dev_lock);
    vp8_hw_error = 0;
    output_format(c, VE_MODE_DEC_H264);             /* VP8 runs on the H.264 engine */
    wr(VE_H264_CTRL, VE_H264_CTRL_VP8);
    wr(VE_VP8_FIRST_DATA_PART_LEN, h->first_part_size * 8);
    wr(VE_VP8_PART_SIZE_OFFSET, h->first_part_size + h->header_size);
    wr(VE_H264_VLD_LEN, (uint32_t)c->bs.size * 8);
    wr(VE_H264_VLD_OFFSET, h->header_size * 8);
    wr(VE_H264_VLD_END, c->bs.ve + j->len);
    wr(VE_H264_VLD_ADDR, VE_H264_VLD_ADDR_VAL(c->bs.ve) | VE_H264_VLD_ADDR_FIRST | VE_H264_VLD_ADDR_VALID | VE_H264_VLD_ADDR_LAST);
    wr(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_INIT_SWDEC);
    wr(VE_VP8_ENTROPY_PROBS_ADDR, c->probs.ve);
    uint32_t reg = 0;
    if (h->version == 1) reg |= VE_VP8_PPS_FILTER_TYPE_SIMPLE | VE_VP8_PPS_BILINEAR_MC_FILTER;
    else if (h->version == 2) reg |= VE_VP8_PPS_LPF_DISABLE | VE_VP8_PPS_BILINEAR_MC_FILTER;
    else if (h->version == 3) reg |= VE_VP8_PPS_LPF_DISABLE | VE_VP8_PPS_FULL_PIXEL;
    if (h->seg_update_map) reg |= VE_VP8_PPS_UPDATE_MB_SEGMENTATION_MAP;
    if (h->seg_abs) reg |= VE_VP8_PPS_MB_SEGMENT_ABS_DELTA;
    if (h->seg_enabled) reg |= VE_VP8_PPS_SEGMENTATION_ENABLE;
    if (c->vp8_last_simple) reg |= VE_VP8_PPS_LAST_LOOP_FILTER_SIMPLE;
    reg |= VE_VP8_PPS_SHARPNESS_LEVEL(h->sharpness);
    if (h->filter_simple) reg |= VE_VP8_PPS_LOOP_FILTER_SIMPLE;
    reg |= VE_VP8_PPS_LOOP_FILTER_LEVEL(h->lf_level);
    if (h->lf_delta_enabled) reg |= VE_VP8_PPS_MODE_REF_LF_DELTA_ENABLE;
    if (h->lf_delta_update) reg |= VE_VP8_PPS_MODE_REF_LF_DELTA_UPDATE;
    reg |= VE_VP8_PPS_TOKEN_PARTITION(h->log2_parts);
    if (h->no_coeff_skip) reg |= VE_VP8_PPS_MB_NO_COEFF_SKIP;
    reg |= VE_VP8_PPS_RELOAD_ENTROPY_PROBS;
    if (h->sign_golden) reg |= VE_VP8_PPS_GOLDEN_SIGN_BIAS;
    if (h->sign_alt) reg |= VE_VP8_PPS_ALTREF_SIGN_BIAS;
    if (c->vp8_last_p) reg |= VE_VP8_PPS_LAST_PIC_TYPE_P_FRAME;
    reg |= VE_VP8_PPS_LAST_SHARPNESS_LEVEL(c->vp8_last_sharpness);
    if (!h->key) reg |= VE_VP8_PPS_PIC_TYPE_P_FRAME;
    wr(VE_VP8_PPS, reg);
    vp8_read_header(h->key);
    if (vp8_hw_error) { ioctl(ve_fd, IOCTL_RESET_VE, 0); pthread_mutex_unlock(&dev_lock); return -1; }
    wr(VE_H264_CUR_MB_NUM, 0);                      /* the header read changed these */
    wr(VE_H264_MB_ADDR, 0);
    wr(VE_H264_ERROR_CASE, 0);
    wr(VE_VP8_QP_INDEX_DELTA, VE_VP8_QP_INDEX_DELTA_UVAC(h->uv_ac) | VE_VP8_QP_INDEX_DELTA_UVDC(h->uv_dc) |
                              VE_VP8_QP_INDEX_DELTA_Y2AC(h->y2_ac) | VE_VP8_QP_INDEX_DELTA_Y2DC(h->y2_dc) |
                              VE_VP8_QP_INDEX_DELTA_Y1DC(h->y_dc) | VE_VP8_QP_INDEX_DELTA_BASE_QINDEX(h->y_ac_qi));
    wr(VE_VP8_FSIZE, VE_VP8_FSIZE_WIDTH(h->width) | VE_VP8_FSIZE_HEIGHT(h->height));
    wr(VE_VP8_PICSIZE, VE_VP8_PICSIZE_WIDTH(h->width) | VE_VP8_PICSIZE_HEIGHT(h->height));
    wr(VE_VP8_SEGMENT_FEAT_MB_LV0, VE_VP8_SEGMENT3(h->seg_q[3]) | VE_VP8_SEGMENT2(h->seg_q[2]) |
                                   VE_VP8_SEGMENT1(h->seg_q[1]) | VE_VP8_SEGMENT0(h->seg_q[0]));
    wr(VE_VP8_SEGMENT_FEAT_MB_LV1, VE_VP8_SEGMENT3(h->seg_lf[3]) | VE_VP8_SEGMENT2(h->seg_lf[2]) |
                                   VE_VP8_SEGMENT1(h->seg_lf[1]) | VE_VP8_SEGMENT0(h->seg_lf[0]));
    wr(VE_VP8_REF_LF_DELTA, VE_VP8_LF_DELTA3(h->ref_delta[3]) | VE_VP8_LF_DELTA2(h->ref_delta[2]) |
                            VE_VP8_LF_DELTA1(h->ref_delta[1]) | VE_VP8_LF_DELTA0(h->ref_delta[0]));
    wr(VE_VP8_MODE_LF_DELTA, VE_VP8_LF_DELTA3(h->mode_delta[3]) | VE_VP8_LF_DELTA2(h->mode_delta[2]) |
                             VE_VP8_LF_DELTA1(h->mode_delta[1]) | VE_VP8_LF_DELTA0(h->mode_delta[0]));
    wr(VE_VP8_REC_LUMA, c->pic[j->cur].luma);
    wr(VE_VP8_REC_CHROMA, c->pic[j->cur].chroma);
    vp8_ref(c, j->last, VE_VP8_FWD_LUMA, VE_VP8_FWD_CHROMA);
    vp8_ref(c, j->golden, VE_VP8_BWD_LUMA, VE_VP8_BWD_CHROMA);
    vp8_ref(c, j->alt, VE_VP8_ALT_LUMA, VE_VP8_ALT_CHROMA);
    wr(VE_H264_CTRL, VE_H264_CTRL_VP8 | VE_H264_CTRL_DECODE_ERR_INT | VE_H264_CTRL_SLICE_DECODE_INT);
    if (h->lf_level) {
        c->vp8_last_simple = h->filter_simple;
        c->vp8_last_p = !h->key;
        c->vp8_last_sharpness = h->sharpness;
    }
    int r = run(VE_H264_TRIGGER_TYPE, VE_H264_TRIGGER_TYPE_VP8_SLICE_DECODE, VE_H264_STATUS, VE_H264_STATUS_SLICE_DECODE_INT,
                VE_H264_STATUS_DECODE_ERR_INT | VE_H264_STATUS_VLD_DATA_REQ_INT, VE_H264_CTRL, VE_H264_STATUS_INT_MASK);
    pthread_mutex_unlock(&dev_lock);
    return r;
}

/* ---------------- pictures out ---------------- */
static void cedar_export(void *hw, int slot, PmhwPicture *pic)
{
    Cedar *c = hw;
    Pic *p = &c->pic[slot];
    /* the engine wrote the picture behind the CPU caches: drop stale lines before the caller reads it */
    if (p->cached) {
        size_t len = (size_t)c->g.stride * c->g.coded_h * 3 / 2;
        /* 4.9 ION zaps the user mapping of a cached buffer whenever a device maps its dmabuf (a caller importing
         * the picture into EGL, Mali), and the sunxi flush walks user addresses without faulting them in: on missing
         * PTEs it oopses the kernel (__dma_flush_range, RG Cube XX 2026-10-30) and the process hangs in do_exit.
         * One read per page maps them back first (ION's fault handler); costs nothing when they are present. */
        for (size_t o = 0; o < len; o += 4096) (void)*(volatile const uint8_t *)(p->mem.cpu + o);
        (void)*(volatile const uint8_t *)(p->mem.cpu + len - 1);
        cache_flush(p->mem.cpu, len);
    }
    pic->format = PMHW_FMT_NV12;
    pic->plane[0] = p->mem.cpu;
    pic->plane[1] = p->mem.cpu + c->g.stride * c->g.coded_h;
    pic->stride[0] = pic->stride[1] = c->g.stride;
    pic->dmabuf_fd = p->mem.fd;
    pic->drm_fourcc = DRM_FORMAT_NV12;
    pic->drm_modifier = 0;
    pic->offset[0] = 0;
    pic->offset[1] = c->g.stride * c->g.coded_h;
}

static void cedar_close(void *hw)
{
    Cedar *c = hw;
    if (!c) return;
    free_bufs(c);
    buf_free(&c->bs);
    free(c);
    dev_put();
}

const HwOps pmhw_cedar_ops = {
    "cedar", cedar_query, cedar_open, cedar_configure, cedar_h264_slice, cedar_hevc_slice, cedar_export, cedar_close, NULL, cedar_vp8_frame,
};
