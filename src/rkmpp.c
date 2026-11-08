// SPDX-License-Identifier: LGPL-2.1-or-later
/* "rkmpp": Rockchip SoCs on vendor (BSP) kernels through Rockchip MPP (librockchip_mpp, Apache-2.0 / MIT, shipped
 * next to libpmhwdec and dlopen()ed: no hard dependency). MPP parses and reorders itself, so this is a stream
 * backend: access units in, frames out in display order. Covers the legacy 4.4 vcodec nodes (vpu_service, rkvdec,
 * hevc_service: RK3326, RK3399) and 4.19+ mpp_service (RK3566, RK3588).
 * 10-bit output (MPP's packed NV15) is not converted yet: such streams return PMHW_UNSUPPORTED. */
#define _GNU_SOURCE                     /* dladdr */
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "internal.h"
#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"

#define DRM_FORMAT_NV12 0x3231564e

static const char *const nodes[] = { "/dev/mpp_service", "/dev/rkvdec", "/dev/hevc_service", "/dev/vpu_service",
                                     "/dev/vpu-service", NULL };

/* ---------------- librockchip_mpp, loaded at run time ---------------- */
static struct {
    int tried, ok;
    MPP_RET (*create)(MppCtx *, MppApi **);
    MPP_RET (*init)(MppCtx, MppCtxType, MppCodingType);
    MPP_RET (*destroy)(MppCtx);
    MPP_RET (*check_format)(MppCtxType, MppCodingType);
    MPP_RET (*packet_init)(MppPacket *, void *, size_t);
    MPP_RET (*packet_deinit)(MppPacket *);
    void (*packet_set_pts)(MppPacket, RK_S64);
    MPP_RET (*packet_set_eos)(MppPacket);
    MPP_RET (*frame_deinit)(MppFrame *);
    RK_U32 (*frame_width)(const MppFrame), (*frame_height)(const MppFrame);
    RK_U32 (*frame_hstride)(const MppFrame), (*frame_vstride)(const MppFrame);
    RK_U32 (*frame_info_change)(const MppFrame), (*frame_eos)(const MppFrame);
    RK_U32 (*frame_errinfo)(const MppFrame), (*frame_discard)(const MppFrame);
    RK_U32 (*frame_offx)(const MppFrame), (*frame_offy)(const MppFrame);
    RK_S64 (*frame_pts)(const MppFrame);
    MppFrameFormat (*frame_fmt)(MppFrame);
    size_t (*frame_buf_size)(const MppFrame);
    MppBuffer (*frame_buffer)(const MppFrame);
    void *(*buf_ptr)(MppBuffer, const char *);
    int (*buf_fd)(MppBuffer, const char *);
    MPP_RET (*buf_sync_begin)(MppBuffer, RK_S32, const char *);
    MPP_RET (*group_get)(MppBufferGroup *, MppBufferType, MppBufferMode, const char *, const char *);
    MPP_RET (*group_put)(MppBufferGroup);
} M;

static int load_mpp(void)
{
    if (M.tried) return M.ok;
    M.tried = 1;
    void *h = NULL;
    Dl_info info;
    if (dladdr((void *)load_mpp, &info) && info.dli_fname) {      /* next to libpmhwdec first */
        char p[1024];
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash && slash - info.dli_fname < 900) {
            memcpy(p, info.dli_fname, slash - info.dli_fname + 1);
            strcpy(p + (slash - info.dli_fname + 1), "librockchip_mpp.so");
            h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
            if (!h) LOG("rkmpp: %s", dlerror());
        }
    }
    if (!h && !(h = dlopen("librockchip_mpp.so", RTLD_NOW | RTLD_LOCAL))) LOG("rkmpp: %s", dlerror());
    if (!h) h = dlopen("librockchip_mpp.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("librockchip_mpp.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOG("rkmpp: librockchip_mpp not found (%s)", dlerror()); return 0; }
#define SYM(f, n) if (!(*(void **)&M.f = dlsym(h, n))) { LOG("rkmpp: %s missing", n); return 0; }
    SYM(create, "mpp_create") SYM(init, "mpp_init") SYM(destroy, "mpp_destroy")
    SYM(check_format, "mpp_check_support_format")
    SYM(packet_init, "mpp_packet_init") SYM(packet_deinit, "mpp_packet_deinit")
    SYM(packet_set_pts, "mpp_packet_set_pts") SYM(packet_set_eos, "mpp_packet_set_eos")
    SYM(frame_deinit, "mpp_frame_deinit")
    SYM(frame_width, "mpp_frame_get_width") SYM(frame_height, "mpp_frame_get_height")
    SYM(frame_hstride, "mpp_frame_get_hor_stride") SYM(frame_vstride, "mpp_frame_get_ver_stride")
    SYM(frame_info_change, "mpp_frame_get_info_change") SYM(frame_eos, "mpp_frame_get_eos")
    SYM(frame_errinfo, "mpp_frame_get_errinfo") SYM(frame_discard, "mpp_frame_get_discard")
    SYM(frame_offx, "mpp_frame_get_offset_x") SYM(frame_offy, "mpp_frame_get_offset_y")
    SYM(frame_pts, "mpp_frame_get_pts") SYM(frame_fmt, "mpp_frame_get_fmt")
    SYM(frame_buf_size, "mpp_frame_get_buf_size") SYM(frame_buffer, "mpp_frame_get_buffer")
    SYM(buf_ptr, "mpp_buffer_get_ptr_with_caller") SYM(buf_fd, "mpp_buffer_get_fd_with_caller")
    SYM(buf_sync_begin, "mpp_buffer_sync_begin_f")
    SYM(group_get, "mpp_buffer_group_get") SYM(group_put, "mpp_buffer_group_put")
#undef SYM
    M.ok = 1;
    return 1;
}

static MppCodingType coding(PmhwCodec c)
{
    return c == PMHW_CODEC_H264 ? MPP_VIDEO_CodingAVC : c == PMHW_CODEC_HEVC ? MPP_VIDEO_CodingHEVC :
           c == PMHW_CODEC_VP8 ? MPP_VIDEO_CodingVP8 : MPP_VIDEO_CodingVP9;
}

static int soc_is_rk3326(void)
{
    static int known = -1;
    if (known >= 0) return known;
    char buf[512] = "";
    FILE *f = fopen("/proc/device-tree/compatible", "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    for (size_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
    buf[n] = 0;
    /* the plain SoC entries only (the RG552's board string names rk3326 although it is an RK3399) */
    known = 0;
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " "))
        if (!strcmp(t, "rockchip,rk3326") || !strcmp(t, "rockchip,px30")) known = 1;
    return known;
}

static int rk_query(PmhwCodec codec, PmhwCaps *caps)
{
    if (codec == PMHW_CODEC_VP9) return -1;                                /* VP9: later */
    int node = 0;
    const char *denied = NULL;
    for (int i = 0; nodes[i]; i++) {
        if (access(nodes[i], R_OK | W_OK) == 0) node = 1;
        else if (errno == EACCES && !denied) denied = nodes[i];
    }
    /* dArkOS / ArkOS: vpu_service and hevc_service are root-only (crw------- root root); ports run as ark */
    if (!node && denied) LOG("rkmpp: %s is not readable/writable by this user (uid %d): run as root or make it group video",
                             denied, (int)getuid());
    if (!node || !load_mpp()) return -1;
    if (M.check_format(MPP_CTX_DEC, coding(codec)) != MPP_OK) return -1;
    caps->max_width = 4096;
    caps->max_height = 2304;
    /* 10-bit HEVC: NV15 -> NV12 in rk_get. Not on RK3326 / PX30: its HEVC block is Main only, MPP takes Main 10 there
     * but every picture comes out wrong (R36S 2GB, 2026-10-30) */
    caps->max_bit_depth = codec == PMHW_CODEC_HEVC && !soc_is_rk3326() ? 10 : 8;
    caps->backend = "rkmpp";
    return 0;
}

/* ---------------- decoder ---------------- */
#define CONV_BUFS 8
typedef struct Held { MppFrame f; int conv; } Held;   /* what pic->priv points at */

typedef struct Rk {
    PmhwCodec codec;
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup group;
    int eos_sent, eos_seen, unsupported;
    int nv15;                                  /* 10-bit output: converted to 8-bit NV12 for the caller */
    uint8_t *conv[CONV_BUFS];
    size_t conv_size;
    int conv_used[CONV_BUFS];
} Rk;

/* NV15: 4 x 10-bit samples in 5 bytes, little endian. The top 8 bits of each (truncation, as the Allwinner engine
 * does it): s0 = b0 >> 2 | b1 << 6, s1 = b1 >> 4 | b2 << 4, s2 = b2 >> 6 | b3 << 2, s3 = b4. */
static void nv15_row(uint8_t *d, const uint8_t *s, int n)
{
    int i = 0;
    for (; i + 4 <= n; i += 4, s += 5, d += 4) {
        d[0] = (uint8_t)(s[0] >> 2 | s[1] << 6);
        d[1] = (uint8_t)(s[1] >> 4 | s[2] << 4);
        d[2] = (uint8_t)(s[2] >> 6 | s[3] << 2);
        d[3] = s[4];
    }
    for (int k = 0; i < n; i++, k++) {         /* tail */
        int bit = k * 10, v = (s[bit >> 3] | s[(bit >> 3) + 1] << 8) >> (bit & 7);
        *d++ = (uint8_t)((v & 0x3ff) >> 2);
    }
}

static int conv_get(Rk *r, size_t size)
{
    if (size > r->conv_size) {                 /* new geometry: drop the free ones */
        for (int i = 0; i < CONV_BUFS; i++) if (!r->conv_used[i]) { free(r->conv[i]); r->conv[i] = NULL; }
        r->conv_size = size;
    }
    for (int i = 0; i < CONV_BUFS; i++) {
        if (r->conv_used[i]) continue;
        if (!r->conv[i] && !(r->conv[i] = malloc(r->conv_size))) return -1;
        r->conv_used[i] = 1;
        return i;
    }
    return -1;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void *rk_open(PmhwCodec codec, int width, int height)
{
    PmhwCaps caps;
    if (rk_query(codec, &caps)) return NULL;
    if (width > caps.max_width || height > caps.max_height) return NULL;
    Rk *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->codec = codec;
    if (M.create(&r->ctx, &r->mpi) != MPP_OK) { free(r); return NULL; }
    RK_U32 split = 0;                               /* we hand over whole access units */
    r->mpi->control(r->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);
    if (M.init(r->ctx, MPP_CTX_DEC, coding(codec)) != MPP_OK) { M.destroy(r->ctx); free(r); LOG("rkmpp: mpp_init failed"); return NULL; }
    RK_S64 timeout = 0;                             /* non-blocking decode_get_frame */
    r->mpi->control(r->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    return r;
}

static int rk_send(void *ctx, const uint8_t *data, int size, int64_t pts)
{
    Rk *r = ctx;
    if (r->unsupported) return PMHW_UNSUPPORTED;
    MppPacket pkt = NULL;
    if (!data || size <= 0) {
        if (M.packet_init(&pkt, NULL, 0) != MPP_OK) return PMHW_ERROR;
        M.packet_set_eos(pkt);
        r->eos_sent = 1;
    } else {
        if (M.packet_init(&pkt, (void *)data, size) != MPP_OK) return PMHW_ERROR;
        M.packet_set_pts(pkt, pts);
    }
    MPP_RET ret = r->mpi->decode_put_packet(r->ctx, pkt);
    M.packet_deinit(&pkt);
    if (ret == MPP_ERR_BUFFER_FULL) { usleep(1000); return PMHW_AGAIN; }   /* MPP copies the data: resend later */
    return ret == MPP_OK ? PMHW_OK : PMHW_ERROR;
}

static int info_change(Rk *r, MppFrame f)
{
    MppFrameFormat fmt = M.frame_fmt(f);
    LOG("rkmpp: %ux%u (stride %ux%u), format 0x%x, buffer %zu bytes", M.frame_width(f), M.frame_height(f),
        M.frame_hstride(f), M.frame_vstride(f), fmt, M.frame_buf_size(f));
    r->nv15 = (fmt & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP_10BIT && !MPP_FRAME_FMT_IS_FBC(fmt);
    if (r->nv15 && soc_is_rk3326()) {
        ERR("rkmpp: 10-bit HEVC is not decoded correctly on RK3326: use software");
        r->unsupported = 1;
    } else if (r->nv15 && !pmhw_selftest_depth_allowed("rkmpp", PMHW_CODEC_HEVC, 10)) {
        ERR("rkmpp: 10-bit HEVC failed the self-test on this chip: use software");
        r->unsupported = 1;
    }
    if (((fmt & MPP_FRAME_FMT_MASK) != MPP_FMT_YUV420SP && !r->nv15) || MPP_FRAME_FMT_IS_FBC(fmt)) {
        ERR("rkmpp: output format 0x%x (compressed / not 4:2:0) is not supported", fmt);
        r->unsupported = 1;
    }
    if (!r->group) {
        /* cached DRM buffers (synced before the CPU reads them); plain DRM if the kernel refuses */
        if (M.group_get(&r->group, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE, MPP_BUFFER_INTERNAL, "pmhwdec", __func__) != MPP_OK &&
            M.group_get(&r->group, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, "pmhwdec", __func__) != MPP_OK &&
            M.group_get(&r->group, MPP_BUFFER_TYPE_ION, MPP_BUFFER_INTERNAL, "pmhwdec", __func__) != MPP_OK)
            r->group = NULL;
        if (r->group) r->mpi->control(r->ctx, MPP_DEC_SET_EXT_BUF_GROUP, r->group);
    }
    r->mpi->control(r->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
    return 0;
}

static int rk_get(void *ctx, PmhwPicture *pic)
{
    Rk *r = ctx;
    double deadline = now() + 2.0;
    for (;;) {
        MppFrame f = NULL;
        MPP_RET ret = r->mpi->decode_get_frame(r->ctx, &f);
        if (ret != MPP_OK || !f) {
            /* after end of stream, wait for the remaining frames (MPP decodes in its own thread) */
            if (r->eos_sent && !r->eos_seen && now() < deadline) { usleep(2000); continue; }
            return 0;
        }
        if (M.frame_info_change(f)) { info_change(r, f); M.frame_deinit(&f); continue; }
        MppBuffer b = M.frame_buffer(f);
        if (M.frame_eos(f)) r->eos_seen = 1;
        if (!b || r->unsupported) { M.frame_deinit(&f); if (r->eos_seen) return 0; continue; }
        memset(pic, 0, sizeof *pic);
        M.buf_sync_begin(b, 1, __func__);            /* read-only CPU access: invalidate cached lines */
        uint8_t *p = M.buf_ptr(b, __func__);
        int hs = M.frame_hstride(f), vs = M.frame_vstride(f);
        pic->width = M.frame_width(f);
        pic->height = M.frame_height(f);
        pic->left = M.frame_offx(f);
        pic->top = M.frame_offy(f);
        pic->format = PMHW_FMT_NV12;
        pic->plane[0] = p;
        pic->plane[1] = p + hs * vs;
        pic->stride[0] = pic->stride[1] = hs;
        pic->pts = M.frame_pts(f);
        pic->corrupt = M.frame_errinfo(f) || M.frame_discard(f);
        pic->dmabuf_fd = M.buf_fd(b, __func__);
        pic->drm_fourcc = DRM_FORMAT_NV12;
        pic->offset[0] = 0;
        pic->offset[1] = hs * vs;
        Held *held = malloc(sizeof *held);
        if (!held) { M.frame_deinit(&f); return 0; }
        held->f = f;
        held->conv = -1;
        if (r->nv15) {                             /* 10-bit: top 8 bits into a plain NV12 buffer of ours */
            int w = pic->left + pic->width, h = pic->top + pic->height, st = ALIGN(w, 16);
            int ci = conv_get(r, (size_t)st * ALIGN(h, 2) * 3 / 2);
            if (ci < 0) { ERR("rkmpp: no conversion buffer free"); free(held); M.frame_deinit(&f); return 0; }
            uint8_t *dst = r->conv[ci];
            for (int y = 0; y < h; y++) nv15_row(dst + y * st, p + y * hs, w);
            for (int y = 0; y < (h + 1) / 2; y++) nv15_row(dst + st * ALIGN(h, 2) + y * st, p + hs * vs + y * hs, w);
            pic->plane[0] = dst;
            pic->plane[1] = dst + st * ALIGN(h, 2);
            pic->stride[0] = pic->stride[1] = st;
            pic->dmabuf_fd = -1;                   /* a CPU copy: no zero-copy for 10-bit */
            pic->drm_fourcc = 0;
            pic->offset[1] = 0;
            held->conv = ci;
        }
        pic->priv = held;
        return 1;
    }
}

static void rk_put(void *ctx, PmhwPicture *pic)
{
    Rk *r = ctx;
    Held *held = pic->priv;
    if (held) {
        if (held->f) M.frame_deinit(&held->f);
        if (held->conv >= 0) r->conv_used[held->conv] = 0;
        free(held);
    }
    pic->priv = NULL;
}

static void rk_flush(void *ctx)
{
    Rk *r = ctx;
    r->mpi->reset(r->ctx);
    r->eos_sent = r->eos_seen = 0;
}

static void rk_close(void *ctx)
{
    Rk *r = ctx;
    if (!r) return;
    r->mpi->reset(r->ctx);
    M.destroy(r->ctx);
    if (r->group) M.group_put(r->group);
    for (int i = 0; i < CONV_BUFS; i++) free(r->conv[i]);
    free(r);
}

const Backend pmhw_rkmpp_backend = { "rkmpp", rk_query, rk_open, rk_send, rk_get, rk_put, rk_flush, rk_close };
