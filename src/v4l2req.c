// SPDX-License-Identifier: LGPL-2.1-or-later
/* "v4l2req": mainline kernels' V4L2 stateless decoders (request API): hantro (RK3326 / RK3566 H.264), rkvdec
 * (RK3399), and later cedrus (H6 / H616 / H700 on ROCKNIX). Fed by the shared codec layer (h264.c): the parsed
 * syntax goes into the V4L2_CID_STATELESS_H264_* controls of one media request per picture.
 *
 * This version: H.264 in both decode modes. Frame-based with Annex-B start codes (hantro, rkvdec): one request per
 * picture. Both the multi-planar (hantro, rkvdec) and the single-planar API (cedrus). Slice-based (cedrus): one request per slice with SLICE_PARAMS / PRED_WEIGHTS, the capture buffer held
 * across a picture's slices (V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF); frame-based wins when a device offers both.
 * HEVC: slice-based only (cedrus); frame-based HEVC (rkvdec, hantro G2) is not written yet. uapi headers: src/uapi/linux (mainline, GPL-2.0+ WITH Linux-syscall-note OR BSD-3).
 * Docs: Documentation/userspace-api/media/v4l/dev-stateless-decoder.rst, ext-ctrls-codec-stateless.rst. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>     /* src/uapi first on the include path (build.sh): mainline copies */
#include <linux/media.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include "codec.h"

#define OUT_BUFS 4
#define OUT_SIZE (3 << 20)
/* slice-based: one request at a time, one slice each, so two input buffers of up to a picture's size; on cedrus
 * without an IOMMU they come out of the 32 MB CMA pool the capture buffers need (4 x 3 MB left no room at 720p) */
#define SLICE_OUT_BUFS 2

/* mplane: the multi-planar API (hantro, rkvdec); cedrus has only the single-planar one (V4L2_CAP_VIDEO_M2M) */
typedef struct Dev { char video[32], media[32]; int max_w, max_h, slice_mode, start_code, mplane; } Dev;

static unsigned out_size(const Dev *d, const Geom *g)
{
    if (!d->slice_mode) return OUT_SIZE;
    unsigned n = (unsigned)g->width * g->height * 3 / 4;
    return ALIGN(n < (1u << 20) ? 1u << 20 : n, 4096);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

/* menu control: is this item offered? */
static int menu_has(int fd, uint32_t id, int item)
{
    struct v4l2_querymenu m = { .id = id, .index = item };
    return xioctl(fd, VIDIOC_QUERYMENU, &m) == 0;
}

static int find_media(int vfd, const char *video, char *out, size_t n)
{
    /* sysfs: /sys/class/video4linux/videoN/device/media*  (platform decoders) */
    char p[256];
    snprintf(p, sizeof p, "/sys/class/video4linux/%s/device", strrchr(video, '/') + 1);
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (!strncmp(e->d_name, "media", 5)) { snprintf(out, n, "/dev/%s", e->d_name); closedir(d); return 0; }
        closedir(d);
    }
    /* else: the media device whose driver name matches */
    struct v4l2_capability cap;
    if (xioctl(vfd, VIDIOC_QUERYCAP, &cap)) return -1;
    for (int i = 0; i < 16; i++) {
        snprintf(p, sizeof p, "/dev/media%d", i);
        int mfd = open(p, O_RDWR | O_CLOEXEC);
        if (mfd < 0) continue;
        struct media_device_info info;
        int ok = xioctl(mfd, MEDIA_IOC_DEVICE_INFO, &info) == 0 && !strcmp(info.driver, (char *)cap.driver);
        close(mfd);
        if (ok) { snprintf(out, n, "%s", p); return 0; }
    }
    return -1;
}

/* first decoder that takes this codec's slice format: frame-based with Annex-B start codes, or slice-based (with or
 * without start codes) */
static int find_device(PmhwdecCodec codec, Dev *dev)
{
    if (codec != PMHWDEC_CODEC_H264 && codec != PMHWDEC_CODEC_HEVC && codec != PMHWDEC_CODEC_VP8) return -1;
    int hevc = codec == PMHWDEC_CODEC_HEVC, vp8 = codec == PMHWDEC_CODEC_VP8;
    uint32_t fmt = vp8 ? V4L2_PIX_FMT_VP8_FRAME : hevc ? V4L2_PIX_FMT_HEVC_SLICE : V4L2_PIX_FMT_H264_SLICE;
    uint32_t sc_id = hevc ? V4L2_CID_STATELESS_HEVC_START_CODE : V4L2_CID_STATELESS_H264_START_CODE;
    uint32_t dm_id = hevc ? V4L2_CID_STATELESS_HEVC_DECODE_MODE : V4L2_CID_STATELESS_H264_DECODE_MODE;
    /* the H.264 and HEVC menus share values: SLICE_BASED 0, FRAME_BASED 1, START_CODE_NONE 0, ANNEX_B 1 */
    for (int i = 0; i < 64; i++) {
        snprintf(dev->video, sizeof dev->video, "/dev/video%d", i);
        int fd = open(dev->video, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        int ok = 0;
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            uint32_t c = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
            if (c & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_M2M)) {
                dev->mplane = !!(c & V4L2_CAP_VIDEO_M2M_MPLANE);
                struct v4l2_fmtdesc f = { .type = dev->mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT };
                for (f.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &f) == 0; f.index++)
                    if (f.pixelformat == fmt) ok = 1;
            }
        }
        if (ok && vp8) {
            dev->slice_mode = 0;                       /* a whole frame per request, no modes to choose */
            dev->start_code = 0;
        } else if (ok) {
            int annexb = menu_has(fd, sc_id, V4L2_STATELESS_H264_START_CODE_ANNEX_B);
            int none = menu_has(fd, sc_id, V4L2_STATELESS_H264_START_CODE_NONE);
            /* HEVC frame-based (rkvdec, hantro G2: every slice's params as one array control) is not written yet */
            if (!hevc && menu_has(fd, dm_id, V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED) && annexb) {
                dev->slice_mode = 0;
                dev->start_code = V4L2_STATELESS_H264_START_CODE_ANNEX_B;
            } else if (menu_has(fd, dm_id, V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED) &&
                       (none || annexb)) {
                dev->slice_mode = 1;
                dev->start_code = none ? V4L2_STATELESS_H264_START_CODE_NONE : V4L2_STATELESS_H264_START_CODE_ANNEX_B;
            } else {
                LOG("v4l2req: %s (%s): no usable decode mode / start code", dev->video, cap.card);
                ok = 0;
            }
        }
        if (ok && find_media(fd, dev->video, dev->media, sizeof dev->media)) { LOG("v4l2req: %s has no media device", dev->video); ok = 0; }
        if (ok) {
            dev->max_w = 1920; dev->max_h = 1088;
            struct v4l2_frmsizeenum fs = { .pixel_format = fmt };
            if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0) {
                if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) { dev->max_w = fs.discrete.width; dev->max_h = fs.discrete.height; }
                else { dev->max_w = fs.stepwise.max_width; dev->max_h = fs.stepwise.max_height; }
            }
            LOG("v4l2req: %s %s = %s (%s), media %s, up to %dx%d, %s, %s", vp8 ? "VP8" : hevc ? "HEVC" : "H.264", dev->video, cap.card, cap.driver, dev->media,
                dev->max_w, dev->max_h, dev->slice_mode ? "slice-based" : "frame-based", dev->mplane ? "multi-planar" : "single-planar");
        }
        close(fd);
        if (ok) return 0;
    }
    return -1;
}

static int v_query(PmhwdecCodec codec, PmhwdecCaps *caps)
{
    static Dev cache[PMHWDEC_CODEC_COUNT];
    static int tried[PMHWDEC_CODEC_COUNT], found[PMHWDEC_CODEC_COUNT];
    if (codec >= PMHWDEC_CODEC_COUNT) return -1;
    if (!tried[codec]) { tried[codec] = 1; found[codec] = find_device(codec, &cache[codec]) == 0; }
    if (!found[codec]) return -1;
    caps->max_width = cache[codec].max_w;
    caps->max_height = cache[codec].max_h;
    caps->max_bit_depth = codec == PMHWDEC_CODEC_HEVC ? 10 : 8;   /* HEVC 10: cedrus on H6/H616; the self-test decides */
    caps->backend = "v4l2req";
    return 0;
}

/* ---------------- per decoder ---------------- */
typedef struct Map { void *p; size_t len; } Map;

typedef struct V {
    PmhwdecCodec codec;
    Dev dev;
    int vfd, mfd, streaming;
    Geom g;
    Map out[OUT_BUFS];
    int req[OUT_BUFS], next_out;
    Map cap[MAX_SLOTS];
    int cap_fd[MAX_SLOTS], ncap, cap_planes;
    int heap;                    /* capture buffers are cached dma_heap/system dma-bufs (V4L2_MEMORY_DMABUF) */
    uint32_t bpl, cap_h, cap_size, cap_off1;
    uint64_t ts[MAX_SLOTS], counter;
    int can_hold;                /* output queue supports V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF */
    /* the picture being collected */
    int len, failed, cur, cap_queued, nsent;
    uint64_t cur_ts;
    struct v4l2_ctrl_h264_sps sps;
    struct v4l2_ctrl_h264_pps pps;
    struct v4l2_ctrl_h264_scaling_matrix scaling;
    struct v4l2_ctrl_h264_decode_params dp;
    /* slice-based: the slice waiting in out[next_out] (sent when the next one arrives, or by end_picture) */
    int pending, weights;
    int8_t dpb_index[MAX_SLOTS]; /* slot -> dp.dpb[] entry, -1 = none */
    struct v4l2_ctrl_h264_slice_params sp;
    struct v4l2_ctrl_h264_pred_weights pw;
    /* HEVC (slice-based): the picture's controls and the pending slice's */
    struct v4l2_ctrl_hevc_sps hsps;
    struct v4l2_ctrl_hevc_pps hpps;
    struct v4l2_ctrl_hevc_decode_params hdp;
    struct v4l2_ctrl_vp8_frame vp8;
    struct v4l2_ctrl_hevc_slice_params hsp;
    uint32_t hep[256];           /* entry points of the pending slice (cedrus: up to 256 per slice) */
    int hnep;
} V;

#define OUT_T(v) ((v)->dev.mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT)
#define CAP_T(v) ((v)->dev.mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE)

/* a v4l2_buffer for either API: a multi-planar one carries its one plane in *pl */
static void buf_init(const V *v, struct v4l2_buffer *b, struct v4l2_plane *pl, int type, int memory, int index)
{
    memset(b, 0, sizeof *b);
    memset(pl, 0, sizeof *pl);
    b->type = type;
    b->memory = memory;
    b->index = index;
    if (v->dev.mplane) { b->length = 1; b->m.planes = pl; }
}

static void buf_set(const V *v, struct v4l2_buffer *b, struct v4l2_plane *pl, uint32_t bytesused, uint32_t length, int fd)
{
    if (v->dev.mplane) { pl->bytesused = bytesused; pl->length = length; if (fd >= 0) pl->m.fd = fd; }
    else { b->bytesused = bytesused; b->length = length; if (fd >= 0) b->m.fd = fd; }
}

static void *v_open(PmhwdecCodec codec)
{
    V *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->codec = codec;
    v->vfd = v->mfd = -1;
    for (int i = 0; i < OUT_BUFS; i++) v->req[i] = -1;
    for (int i = 0; i < MAX_SLOTS; i++) v->cap_fd[i] = -1;
    if (find_device(codec, &v->dev)) { free(v); return NULL; }
    v->vfd = open(v->dev.video, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    v->mfd = open(v->dev.media, O_RDWR | O_CLOEXEC);
    if (v->vfd < 0 || v->mfd < 0) {
        ERR("v4l2req: open %s / %s: %s", v->dev.video, v->dev.media, strerror(errno));
        if (v->vfd >= 0) close(v->vfd);
        if (v->mfd >= 0) close(v->mfd);
        free(v);
        return NULL;
    }
    return v;
}

static void teardown(V *v)
{
    if (v->streaming) {
        int t = OUT_T(v);
        xioctl(v->vfd, VIDIOC_STREAMOFF, &t);
        t = CAP_T(v);
        xioctl(v->vfd, VIDIOC_STREAMOFF, &t);
        v->streaming = 0;
    }
    for (int i = 0; i < OUT_BUFS; i++) {
        if (v->out[i].p) munmap(v->out[i].p, v->out[i].len);
        v->out[i].p = NULL;
        if (v->req[i] >= 0) close(v->req[i]);
        v->req[i] = -1;
    }
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (v->cap[i].p) munmap(v->cap[i].p, v->cap[i].len);
        v->cap[i].p = NULL;
        if (v->cap_fd[i] >= 0) close(v->cap_fd[i]);
        v->cap_fd[i] = -1;
    }
    struct v4l2_requestbuffers rb = { .count = 0, .memory = V4L2_MEMORY_MMAP, .type = OUT_T(v) };
    xioctl(v->vfd, VIDIOC_REQBUFS, &rb);
    rb.type = CAP_T(v);
    rb.memory = v->heap ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    rb.count = 0;
    xioctl(v->vfd, VIDIOC_REQBUFS, &rb);
    v->ncap = 0;
    v->heap = 0;
}

/* cached capture buffers from a dma-heap: /dev/dma_heap/system when the decoder sits behind an IOMMU (its buffers
 * are not physically contiguous), else a CMA heap (contiguous; cedrus on ROCKNIX H700 has no IOMMU: uncached MMAP
 * copy-out was 29.7 ms per 1080p frame). 0 = done, -1 = not possible here (caller uses MMAP buffers). */
static int heap_buffers(V *v, int n, size_t size)
{
    const char *env = getenv("PMHWDEC_V4L2_DMABUF");
    if (env && !strcmp(env, "0")) return -1;
    char p[128];
    snprintf(p, sizeof p, "/sys/class/video4linux/%s/device/iommu_group", strrchr(v->dev.video, '/') + 1);
    int iommu = access(p, F_OK) == 0, hfd = -1;
    static const char *cma[] = { "/dev/dma_heap/default_cma_region", "/dev/dma_heap/linux,cma", "/dev/dma_heap/reserved" };
    const char *heap = "/dev/dma_heap/system";
    if (iommu) hfd = open(heap, O_RDWR | O_CLOEXEC);
    else for (unsigned i = 0; hfd < 0 && i < sizeof cma / sizeof *cma; i++) hfd = open(heap = cma[i], O_RDWR | O_CLOEXEC);
    if (hfd < 0) { LOG("v4l2req: %s for %s: MMAP capture buffers", iommu ? "no system dma-heap" : "no IOMMU, no CMA dma-heap", v->dev.video); return -1; }
    LOG("v4l2req: capture buffers from %s", heap);
    struct v4l2_requestbuffers cb = { .count = n, .memory = V4L2_MEMORY_DMABUF, .type = CAP_T(v) };
    if (xioctl(v->vfd, VIDIOC_REQBUFS, &cb) || (int)cb.count < n) { close(hfd); LOG("v4l2req: DMABUF capture refused"); return -1; }
    v->heap = 1;
    size = ALIGN(size, 4096);
    for (int i = 0; i < n; i++) {
        struct dma_heap_allocation_data a = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
        if (xioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a)) { LOG("v4l2req: %s: buffer %d of %d (%zu bytes): %s", heap, i + 1, n, size, strerror(errno)); close(hfd); return -1; }
        v->cap_fd[i] = a.fd;
        v->cap[i].len = size;
        v->cap[i].p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
        if (v->cap[i].p == MAP_FAILED) { v->cap[i].p = NULL; close(hfd); return -1; }
    }
    close(hfd);
    v->ncap = n;
    return 0;
}

static void cpu_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync s = { .flags = flags };
    xioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
}

static int set_ctrl(int fd, uint32_t id, int32_t val)
{
    struct v4l2_ext_control c = { .id = id, .value = val };
    struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &c };
    return xioctl(fd, VIDIOC_S_EXT_CTRLS, &cs);
}

static int map_buf(V *v, int type, int i, Map *m, int *fd_out)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
    memset(planes, 0, sizeof planes);
    if (v->dev.mplane) { b.length = VIDEO_MAX_PLANES; b.m.planes = planes; }
    if (xioctl(v->vfd, VIDIOC_QUERYBUF, &b)) return -1;
    if (v->dev.mplane && b.length != 1 && type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) { ERR("v4l2req: multi-plane capture buffers not handled"); return -1; }
    m->len = v->dev.mplane ? planes[0].length : b.length;
    m->p = mmap(NULL, m->len, PROT_READ | PROT_WRITE, MAP_SHARED, v->vfd, v->dev.mplane ? planes[0].m.mem_offset : b.m.offset);
    if (m->p == MAP_FAILED) { m->p = NULL; return -1; }
    if (fd_out) {
        struct v4l2_exportbuffer e = { .type = type, .index = i, .plane = 0, .flags = O_RDONLY | O_CLOEXEC };
        *fd_out = xioctl(v->vfd, VIDIOC_EXPBUF, &e) == 0 ? e.fd : -1;
    }
    return 0;
}

static int v_configure(void *hw, const Geom *g)
{
    V *v = hw;
    teardown(v);
    v->g = *g;
    int hevc = v->codec == PMHWDEC_CODEC_HEVC, vp8 = v->codec == PMHWDEC_CODEC_VP8;
    uint32_t fmt = vp8 ? V4L2_PIX_FMT_VP8_FRAME : hevc ? V4L2_PIX_FMT_HEVC_SLICE : V4L2_PIX_FMT_H264_SLICE;
    if (g->bit_depth > (hevc ? 10 : 8)) { LOG("v4l2req: %d-bit not supported", g->bit_depth); return -1; }
    if (!vp8 && (set_ctrl(v->vfd, hevc ? V4L2_CID_STATELESS_HEVC_DECODE_MODE : V4L2_CID_STATELESS_H264_DECODE_MODE,
                 v->dev.slice_mode ? V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED : V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED) ||
        set_ctrl(v->vfd, hevc ? V4L2_CID_STATELESS_HEVC_START_CODE : V4L2_CID_STATELESS_H264_START_CODE, v->dev.start_code))) {
        ERR("v4l2req: decode mode / start code: %s", strerror(errno));
        return -1;
    }
    int mp = v->dev.mplane;
    struct v4l2_format f = { .type = OUT_T(v) };
    if (mp) {
        f.fmt.pix_mp.pixelformat = fmt;
        f.fmt.pix_mp.width = g->width;
        f.fmt.pix_mp.height = g->height;
        f.fmt.pix_mp.num_planes = 1;
        f.fmt.pix_mp.plane_fmt[0].sizeimage = out_size(&v->dev, g);
    } else {
        f.fmt.pix.pixelformat = fmt;
        f.fmt.pix.width = g->width;
        f.fmt.pix.height = g->height;
        f.fmt.pix.sizeimage = out_size(&v->dev, g);
    }
    if (xioctl(v->vfd, VIDIOC_S_FMT, &f)) { ERR("v4l2req: S_FMT output: %s", strerror(errno)); return -1; }
    if (hevc) {
        /* the bit depth sizes the capture buffers (cedrus: 10-bit = NV12 of the top 8 bits + the low 2 bits after it),
         * so the SPS goes in before G_FMT / REQBUFS on the capture side; the per-request SPS repeats it */
        struct v4l2_ctrl_hevc_sps sps = { .pic_width_in_luma_samples = g->width, .pic_height_in_luma_samples = g->height,
                                          .bit_depth_luma_minus8 = g->bit_depth - 8, .bit_depth_chroma_minus8 = g->bit_depth - 8,
                                          .chroma_format_idc = 1 };
        struct v4l2_ext_control c = { .id = V4L2_CID_STATELESS_HEVC_SPS, .size = sizeof sps, .ptr = &sps };
        struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &c };
        if (xioctl(v->vfd, VIDIOC_S_EXT_CTRLS, &cs) && g->bit_depth > 8) {
            LOG("v4l2req: %d-bit HEVC refused by the driver: %s", g->bit_depth, strerror(errno));
            return -1;
        }
    }
    memset(&f, 0, sizeof f);
    f.type = CAP_T(v);
    if (xioctl(v->vfd, VIDIOC_G_FMT, &f)) return -1;
    uint32_t *pixfmt = mp ? &f.fmt.pix_mp.pixelformat : &f.fmt.pix.pixelformat;
    if (*pixfmt != V4L2_PIX_FMT_NV12) {
        *pixfmt = V4L2_PIX_FMT_NV12;
        if (xioctl(v->vfd, VIDIOC_S_FMT, &f) || *pixfmt != V4L2_PIX_FMT_NV12) {
            ERR("v4l2req: the decoder does not output linear NV12");
            return -1;
        }
    }
    v->cap_planes = mp ? f.fmt.pix_mp.num_planes : 1;
    v->bpl = mp ? f.fmt.pix_mp.plane_fmt[0].bytesperline : f.fmt.pix.bytesperline;
    v->cap_h = mp ? f.fmt.pix_mp.height : f.fmt.pix.height;
    v->cap_size = mp ? f.fmt.pix_mp.plane_fmt[0].sizeimage : f.fmt.pix.sizeimage;
    v->cap_off1 = v->bpl * v->cap_h;
    LOG("v4l2req: capture NV12 %ux%u, %u bytes per line, %u planes", mp ? f.fmt.pix_mp.width : f.fmt.pix.width, v->cap_h,
        v->bpl, v->cap_planes);

    struct v4l2_requestbuffers rb = { .count = v->dev.slice_mode || vp8 ? SLICE_OUT_BUFS : OUT_BUFS, .memory = V4L2_MEMORY_MMAP, .type = OUT_T(v) };
    if (xioctl(v->vfd, VIDIOC_REQBUFS, &rb) || rb.count < 1) { ERR("v4l2req: REQBUFS output: %s", strerror(errno)); return -1; }
    v->can_hold = !!(rb.capabilities & V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF);
    if (v->dev.slice_mode && !v->can_hold) LOG("v4l2req: no HOLD_CAPTURE_BUF: single-slice pictures only");
    for (int i = 0; i < (int)rb.count && i < OUT_BUFS; i++) {
        if (map_buf(v, OUT_T(v), i, &v->out[i], NULL)) return -1;
        if (xioctl(v->mfd, MEDIA_IOC_REQUEST_ALLOC, &v->req[i])) { ERR("v4l2req: request alloc: %s", strerror(errno)); return -1; }
    }
    /* capture buffers = picture slots. Cached is what matters for CPU readers (uncached: ~34 ms per 1080p copy):
     * dma_heap/system dma-bufs when possible, else MMAP buffers with the non-coherent hint, else plain MMAP */
    const char *how;
    if (v->cap_planes == 1 && heap_buffers(v, g->nslots, v->cap_size) == 0) {
        how = "cached dma-heap dma-bufs";
    } else {
        if (v->heap) {                                /* partial heap setup: undo it */
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (v->cap[i].p) munmap(v->cap[i].p, v->cap[i].len);
                v->cap[i].p = NULL;
                if (v->cap_fd[i] >= 0) close(v->cap_fd[i]);
                v->cap_fd[i] = -1;
            }
            struct v4l2_requestbuffers z = { .count = 0, .memory = V4L2_MEMORY_DMABUF, .type = CAP_T(v) };
            xioctl(v->vfd, VIDIOC_REQBUFS, &z);
            v->heap = 0;
        }
        struct v4l2_requestbuffers cb = { .count = 0, .memory = V4L2_MEMORY_MMAP, .type = CAP_T(v) };
        xioctl(v->vfd, VIDIOC_REQBUFS, &cb);
        int cached = cb.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS;
        cb.count = g->nslots;
        cb.flags = cached ? V4L2_MEMORY_FLAG_NON_COHERENT : 0;
        if (xioctl(v->vfd, VIDIOC_REQBUFS, &cb) || (int)cb.count < g->nslots) {
            ERR("v4l2req: REQBUFS capture (%d): %s", g->nslots, strerror(errno));
            return -1;
        }
        v->ncap = cb.count;
        for (int i = 0; i < g->nslots; i++) if (map_buf(v, CAP_T(v), i, &v->cap[i], &v->cap_fd[i])) return -1;
        how = cached ? "MMAP, cached (non-coherent)" : "MMAP, uncached";
    }
    LOG("v4l2req: %d capture buffers, %s", v->ncap, how);
    int t = OUT_T(v);
    if (xioctl(v->vfd, VIDIOC_STREAMON, &t)) return -1;
    t = CAP_T(v);
    if (xioctl(v->vfd, VIDIOC_STREAMON, &t)) return -1;
    v->streaming = 1;
    v->next_out = 0;
    v->pending = v->cap_queued = 0;
    return 0;
}

/* one request: the controls + out[o] (len bytes), queued and waited for; the output buffer comes back, the capture
 * buffer stays with the driver when hold is set */
static int send_request(V *v, int o, int hold)
{
    int req = v->req[o];
    struct v4l2_ext_control c[6] = {
        { .id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof v->sps, .ptr = &v->sps },
        { .id = V4L2_CID_STATELESS_H264_PPS, .size = sizeof v->pps, .ptr = &v->pps },
        { .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX, .size = sizeof v->scaling, .ptr = &v->scaling },
        { .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS, .size = sizeof v->dp, .ptr = &v->dp },
        { .id = V4L2_CID_STATELESS_H264_SLICE_PARAMS, .size = sizeof v->sp, .ptr = &v->sp },
        { .id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS, .size = sizeof v->pw, .ptr = &v->pw },
    };
    int n = !v->dev.slice_mode ? 4 : v->weights ? 6 : 5;
    if (v->codec == PMHWDEC_CODEC_HEVC) {               /* no scaling matrix: streams with scaling lists are refused */
        struct v4l2_ext_control h[5] = {
            { .id = V4L2_CID_STATELESS_HEVC_SPS, .size = sizeof v->hsps, .ptr = &v->hsps },
            { .id = V4L2_CID_STATELESS_HEVC_PPS, .size = sizeof v->hpps, .ptr = &v->hpps },
            { .id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS, .size = sizeof v->hdp, .ptr = &v->hdp },
            { .id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS, .size = sizeof v->hsp, .ptr = &v->hsp },
            { .id = V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS, .size = v->hnep * 4, .ptr = v->hep },
        };
        n = v->hnep ? 5 : 4;
        memcpy(c, h, sizeof h);
    } else if (v->codec == PMHWDEC_CODEC_VP8) {
        c[0] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_VP8_FRAME, .size = sizeof v->vp8, .ptr = &v->vp8 };
        n = 1;
    }
    struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_REQUEST_VAL, .count = n, .request_fd = req, .controls = c };
    int rc = 0;
    if (xioctl(v->vfd, VIDIOC_S_EXT_CTRLS, &cs)) { ERR("v4l2req: controls: %s (control %u)", strerror(errno), cs.error_idx); rc = -1; }
    struct v4l2_plane op;
    struct v4l2_buffer out;
    buf_init(v, &out, &op, OUT_T(v), V4L2_MEMORY_MMAP, o);
    buf_set(v, &out, &op, v->len, v->out[o].len, -1);
    out.flags = V4L2_BUF_FLAG_REQUEST_FD | (hold ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0);
    out.request_fd = req;
    out.timestamp.tv_sec = v->cur_ts;                 /* every slice of a picture: the same timestamp */
    if (!rc && xioctl(v->vfd, VIDIOC_QBUF, &out)) { ERR("v4l2req: QBUF output: %s", strerror(errno)); rc = -1; }
    if (!rc && xioctl(req, MEDIA_REQUEST_IOC_QUEUE, NULL)) { ERR("v4l2req: request queue: %s", strerror(errno)); rc = -1; }
    if (!rc) {
        struct pollfd pf = { .fd = req, .events = POLLPRI };
        if (poll(&pf, 1, 2000) != 1) { ERR("v4l2req: decode timed out"); rc = -1; }
    }
    if (!rc) {                                        /* the output buffer back (only if it was queued) */
        struct v4l2_plane dp;
        struct v4l2_buffer b;
        buf_init(v, &b, &dp, OUT_T(v), V4L2_MEMORY_MMAP, 0);
        xioctl(v->vfd, VIDIOC_DQBUF, &b);
    }
    xioctl(req, MEDIA_REQUEST_IOC_REINIT, NULL);
    v->next_out = (o + 1) % OUT_BUFS;
    if (!v->out[v->next_out].p) v->next_out = 0;
    v->len = 0;
    v->nsent++;
    return rc;
}

static int queue_capture(V *v)
{
    if (v->cap_queued) return 0;
    struct v4l2_plane cp;
    struct v4l2_buffer cap;
    buf_init(v, &cap, &cp, CAP_T(v), v->heap ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP, v->cur);
    if (v->heap) buf_set(v, &cap, &cp, 0, v->cap[v->cur].len, v->cap_fd[v->cur]);
    if (xioctl(v->vfd, VIDIOC_QBUF, &cap)) { ERR("v4l2req: QBUF capture %d: %s", v->cur, strerror(errno)); return -1; }
    v->cap_queued = 1;
    return 0;
}

/* slice-based: send the waiting slice, holding the capture buffer for the slices still to come */
static int flush_pending(V *v, int hold)
{
    if (!v->pending) return 0;
    v->pending = 0;
    if (hold && !v->can_hold) { ERR("v4l2req: multi-slice picture, but the driver cannot hold the capture buffer"); return -1; }
    if (queue_capture(v)) return -1;
    return send_request(v, v->next_out, hold);
}

/* a picture failed with the capture buffer held: STREAMOFF hands every capture buffer back */
static void drop_capture(V *v)
{
    int t = CAP_T(v);
    xioctl(v->vfd, VIDIOC_STREAMOFF, &t);
    xioctl(v->vfd, VIDIOC_STREAMON, &t);
    v->cap_queued = 0;
}

static void fill_slice_params(V *v, const H264Job *j)
{
    const H264Slice *sl = j->sl;
    memset(&v->sp, 0, sizeof v->sp);
    v->sp.header_bit_size = sl->header_bits;          /* from the NAL header byte, as Cedrus skips it */
    v->sp.first_mb_in_slice = sl->first_mb;
    v->sp.slice_type = sl->type;                      /* H264_P/B/I = V4L2_H264_SLICE_TYPE_P/B/I */
    v->sp.cabac_init_idc = sl->cabac_init;
    v->sp.slice_qp_delta = sl->qp_delta;
    v->sp.disable_deblocking_filter_idc = sl->disable_deblock;
    v->sp.slice_alpha_c0_offset_div2 = sl->alpha;
    v->sp.slice_beta_offset_div2 = sl->beta;
    v->sp.num_ref_idx_l0_active_minus1 = sl->type != H264_I ? sl->num_ref[0] : 0;
    v->sp.num_ref_idx_l1_active_minus1 = sl->type == H264_B ? sl->num_ref[1] : 0;
    v->sp.flags = sl->direct_spatial ? V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED : 0;
    for (int l = 0; l < 2; l++) {
        struct v4l2_h264_reference *r = l ? v->sp.ref_pic_list1 : v->sp.ref_pic_list0;
        for (int i = 0; i < j->len[l] && i < V4L2_H264_REF_LIST_LEN; i++) {
            int s = j->list[l][i];
            r[i].fields = V4L2_H264_FRAME_REF;
            r[i].index = s >= 0 && s < MAX_SLOTS && v->dpb_index[s] >= 0 ? v->dpb_index[s] : 0;
        }
    }
    const H264Pps *p = j->pps;
    v->weights = (p->weighted_pred && sl->type == H264_P) || (p->weighted_bipred == 1 && sl->type == H264_B);
    if (v->weights) {
        memset(&v->pw, 0, sizeof v->pw);
        v->pw.luma_log2_weight_denom = sl->luma_denom;
        v->pw.chroma_log2_weight_denom = sl->chroma_denom;
        for (int l = 0; l < (sl->type == H264_B ? 2 : 1); l++)
            for (int i = 0; i <= sl->num_ref[l] && i < 32; i++) {
                struct v4l2_h264_weight_factors *f = &v->pw.weight_factors[l];
                f->luma_weight[i] = sl->lw[l][i];
                f->luma_offset[i] = sl->lo[l][i];
                for (int c = 0; c < 2; c++) { f->chroma_weight[i][c] = sl->cw[l][i][c]; f->chroma_offset[i][c] = sl->co[l][i][c]; }
            }
    }
}

/* first slice: the picture's controls; every slice: start code + NAL into the output buffer */
static int v_h264_slice(void *hw, const H264Job *j)
{
    V *v = hw;
    const H264Sps *s = j->sps; const H264Pps *p = j->pps; const H264Slice *sl = j->sl;
    if (j->first_slice) {
        v->len = 0;
        v->failed = 0;
        v->pending = v->cap_queued = v->nsent = 0;
        v->cur = j->cur;
        v->cur_ts = ++v->counter;
        v->ts[j->cur] = v->cur_ts * 1000000000ull;    /* reference_ts = the capture buffer's timestamp in ns */
        memset(v->dpb_index, -1, sizeof v->dpb_index);
        memset(&v->sps, 0, sizeof v->sps);
        v->sps.profile_idc = s->profile;
        v->sps.constraint_set_flags = s->constraint_flags;
        v->sps.level_idc = s->level;
        v->sps.seq_parameter_set_id = s->id;
        v->sps.chroma_format_idc = s->chroma_format_idc;
        v->sps.log2_max_frame_num_minus4 = s->log2_max_frame_num - 4;
        v->sps.pic_order_cnt_type = s->poc_type;
        v->sps.log2_max_pic_order_cnt_lsb_minus4 = s->poc_type == 0 ? s->log2_max_poc_lsb - 4 : 0;
        v->sps.max_num_ref_frames = s->max_num_ref_frames;
        v->sps.num_ref_frames_in_pic_order_cnt_cycle = s->num_ref_cycle;
        for (int i = 0; i < s->num_ref_cycle && i < 255; i++) v->sps.offset_for_ref_frame[i] = s->offset_ref[i];
        v->sps.offset_for_non_ref_pic = s->offset_non_ref;
        v->sps.offset_for_top_to_bottom_field = s->offset_top_bottom;
        v->sps.pic_width_in_mbs_minus1 = s->width_mbs - 1;
        v->sps.pic_height_in_map_units_minus1 = s->height_map_units - 1;
        v->sps.flags = (s->qpprime_bypass ? V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS : 0) |
                       (s->delta_pic_order_always_zero ? V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO : 0) |
                       (s->gaps_allowed ? V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED : 0) |
                       (s->frame_mbs_only ? V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY : 0) |
                       (s->mbaff ? V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD : 0) |
                       (s->direct_8x8 ? V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE : 0);
        memset(&v->pps, 0, sizeof v->pps);
        v->pps.pic_parameter_set_id = p->id;
        v->pps.seq_parameter_set_id = p->sps_id;
        v->pps.num_ref_idx_l0_default_active_minus1 = p->num_ref_l0;
        v->pps.num_ref_idx_l1_default_active_minus1 = p->num_ref_l1;
        v->pps.weighted_bipred_idc = p->weighted_bipred;
        v->pps.pic_init_qp_minus26 = p->init_qp;
        v->pps.pic_init_qs_minus26 = p->init_qs;
        v->pps.chroma_qp_index_offset = p->chroma_qp_offset;
        v->pps.second_chroma_qp_index_offset = p->second_chroma_qp_offset;
        v->pps.flags = (p->entropy ? V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE : 0) |
                       (p->bottom_field_poc_present ? V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT : 0) |
                       (p->weighted_pred ? V4L2_H264_PPS_FLAG_WEIGHTED_PRED : 0) |
                       (p->deblock_ctrl ? V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT : 0) |
                       (p->constrained_intra ? V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED : 0) |
                       (p->redundant_pic_cnt ? V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT : 0) |
                       (p->t8x8 ? V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE : 0);
        memset(&v->scaling, 16, sizeof v->scaling);  /* flat (streams with scaling lists are refused) */
        memset(&v->dp, 0, sizeof v->dp);
        int k = 0;
        for (int i = 0; i < j->nslots && k < V4L2_H264_NUM_DPB_ENTRIES; i++) {
            if (!j->slots[i].ref || i == j->cur) continue;
            v->dpb_index[i] = k;
            struct v4l2_h264_dpb_entry *e = &v->dp.dpb[k++];
            e->reference_ts = v->ts[i];
            e->pic_num = j->slots[i].pic_num;
            e->frame_num = j->slots[i].frame_num;
            e->fields = V4L2_H264_FRAME_REF;
            e->top_field_order_cnt = e->bottom_field_order_cnt = j->slots[i].poc;
            e->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
        }
        v->dp.nal_ref_idc = sl->nal_ref_idc;
        v->dp.frame_num = sl->frame_num;
        v->dp.top_field_order_cnt = v->dp.bottom_field_order_cnt = j->cur_poc;
        v->dp.idr_pic_id = sl->idr_pic_id;
        v->dp.pic_order_cnt_lsb = sl->poc_lsb;
        v->dp.delta_pic_order_cnt_bottom = sl->delta_poc_bottom;
        v->dp.delta_pic_order_cnt0 = sl->delta_poc[0];
        v->dp.delta_pic_order_cnt1 = sl->delta_poc[1];
        v->dp.dec_ref_pic_marking_bit_size = sl->ref_marking_bits;
        v->dp.pic_order_cnt_bit_size = sl->poc_bits;
        v->dp.flags = (sl->idr ? V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC : 0) |
                      (sl->type == H264_P ? V4L2_H264_DECODE_PARAM_FLAG_PFRAME : 0) |
                      (sl->type == H264_B ? V4L2_H264_DECODE_PARAM_FLAG_BFRAME : 0);
    }
    if (v->failed) return -1;
    /* slice-based: the previous slice goes now, holding the capture buffer for this one */
    if (v->dev.slice_mode && flush_pending(v, 1)) { v->failed = 1; return -1; }
    Map *o = &v->out[v->next_out];
    int sc = v->dev.start_code == V4L2_STATELESS_H264_START_CODE_ANNEX_B ? 3 : 0;
    if (!o->p || v->len + sc + j->nal_len > (int)o->len) { ERR("v4l2req: picture does not fit the input buffer"); v->failed = 1; return -1; }
    uint8_t *d = (uint8_t *)o->p + v->len;
    if (sc) { d[0] = 0; d[1] = 0; d[2] = 1; }
    memcpy(d + sc, j->nal, j->nal_len);
    v->len += sc + j->nal_len;
    if (v->dev.slice_mode) { fill_slice_params(v, j); v->pending = 1; }
    return 0;
}

/* the dp.dpb[] entry holding this POC, -1 = none */
static int hevc_dpb_of_poc(const V *v, int poc)
{
    for (int i = 0; i < v->hdp.num_active_dpb_entries; i++) if (v->hdp.dpb[i].pic_order_cnt_val == poc) return i;
    return -1;
}

/* slice-based HEVC (cedrus): as v_h264_slice, one request per slice segment. Cedrus reads data_byte_offset as the
 * RBSP byte where slice data starts, counted from the NAL header (it skips (offset - 1) * 8 bits with its own
 * emulation-prevention-aware reader, then reads the byte_alignment() byte), and copies ENTRY_POINT_OFFSETS straight
 * into the engine: the unescaped offsets - 1 our cedar backend writes too. */
static int v_hevc_slice(void *hw, const HevcJob *j)
{
    V *v = hw;
    const HevcSps *s = j->sps; const HevcPps *p = j->pps; const HevcSlice *sl = j->sl;
    if (!v->dev.slice_mode) { ERR("v4l2req: frame-based HEVC is not written yet"); return -1; }
    if (j->first_slice) {
        v->len = 0;
        v->failed = 0;
        v->pending = v->cap_queued = v->nsent = 0;
        v->cur = j->cur;
        v->cur_ts = ++v->counter;
        v->ts[j->cur] = v->cur_ts * 1000000000ull;
        memset(v->dpb_index, -1, sizeof v->dpb_index);

        struct v4l2_ctrl_hevc_sps *hs = &v->hsps;
        memset(hs, 0, sizeof *hs);
        hs->pic_width_in_luma_samples = s->width;
        hs->pic_height_in_luma_samples = s->height;
        hs->bit_depth_luma_minus8 = s->bit_depth - 8;
        hs->bit_depth_chroma_minus8 = s->bit_depth_c - 8;
        hs->log2_max_pic_order_cnt_lsb_minus4 = s->log2_max_poc_lsb - 4;
        hs->sps_max_dec_pic_buffering_minus1 = s->max_dec_pic_buffering - 1;
        hs->sps_max_num_reorder_pics = s->max_num_reorder;
        hs->log2_min_luma_coding_block_size_minus3 = s->log2_min_cb - 3;
        hs->log2_diff_max_min_luma_coding_block_size = s->log2_diff_cb;
        hs->log2_min_luma_transform_block_size_minus2 = s->log2_min_tb - 2;
        hs->log2_diff_max_min_luma_transform_block_size = s->log2_diff_tb;
        hs->max_transform_hierarchy_depth_inter = s->depth_inter;
        hs->max_transform_hierarchy_depth_intra = s->depth_intra;
        if (s->pcm) {
            hs->pcm_sample_bit_depth_luma_minus1 = s->pcm_bd - 1;
            hs->pcm_sample_bit_depth_chroma_minus1 = s->pcm_bd_c - 1;
            hs->log2_min_pcm_luma_coding_block_size_minus3 = s->log2_min_pcm - 3;
            hs->log2_diff_max_min_pcm_luma_coding_block_size = s->log2_diff_pcm;
        }
        hs->num_short_term_ref_pic_sets = s->num_rps;
        hs->chroma_format_idc = s->chroma_format_idc;
        hs->flags = (s->amp ? V4L2_HEVC_SPS_FLAG_AMP_ENABLED : 0) |
                    (s->sao ? V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET : 0) |
                    (s->pcm ? V4L2_HEVC_SPS_FLAG_PCM_ENABLED : 0) |
                    (s->pcm && s->pcm_lf_disabled ? V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED : 0) |
                    (s->temporal_mvp ? V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED : 0) |
                    (s->strong_intra ? V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED : 0);

        struct v4l2_ctrl_hevc_pps *hp = &v->hpps;
        memset(hp, 0, sizeof *hp);
        hp->pic_parameter_set_id = sl->pps_id;
        hp->num_extra_slice_header_bits = p->extra_bits;
        hp->num_ref_idx_l0_default_active_minus1 = p->num_ref_l0;
        hp->num_ref_idx_l1_default_active_minus1 = p->num_ref_l1;
        hp->init_qp_minus26 = p->init_qp;
        hp->diff_cu_qp_delta_depth = p->diff_cu_qp_delta_depth;
        hp->pps_cb_qp_offset = p->cb_qp;
        hp->pps_cr_qp_offset = p->cr_qp;
        hp->pps_beta_offset_div2 = p->beta;
        hp->pps_tc_offset_div2 = p->tc;
        hp->log2_parallel_merge_level_minus2 = p->log2_pml;
        hp->flags = (p->dependent_slices ? V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED : 0) |
                    (p->output_flag ? V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT : 0) |
                    (p->sign_hiding ? V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED : 0) |
                    (p->cabac_init_present ? V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT : 0) |
                    (p->constrained_intra ? V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED : 0) |
                    (p->transform_skip ? V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED : 0) |
                    (p->cu_qp_delta ? V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED : 0) |
                    (p->slice_chroma_qp_offsets ? V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT : 0) |
                    (p->weighted_pred ? V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED : 0) |
                    (p->weighted_bipred ? V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED : 0) |
                    (p->transquant_bypass ? V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED : 0) |
                    (p->wpp ? V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED : 0) |
                    (p->lf_across_slices ? V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED : 0) |
                    (p->deblock_override_enabled ? V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED : 0) |
                    (p->deblock_disabled ? V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER : 0) |
                    (p->lists_mod ? V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT : 0) |
                    (p->slice_ext ? V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT : 0) |
                    /* not kept by the parser: present whenever anything it controls is not the default */
                    (p->deblock_override_enabled || p->deblock_disabled || p->beta || p->tc ?
                     V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT : 0);

        struct v4l2_ctrl_hevc_decode_params *dp = &v->hdp;
        memset(dp, 0, sizeof *dp);
        dp->pic_order_cnt_val = j->cur_poc;
        int k = 0;
        for (int i = 0; i < j->nslots && k < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
            if (!j->slots[i].ref || i == j->cur) continue;
            v->dpb_index[i] = k;
            dp->dpb[k].timestamp = v->ts[i];
            dp->dpb[k].pic_order_cnt_val = j->slots[i].poc;
            k++;
        }
        dp->num_active_dpb_entries = k;
        for (int q = 0; q < sl->rps.num_neg + sl->rps.num_pos; q++) {   /* 8.3.2: the RPS entries used by this picture */
            int neg = q < sl->rps.num_neg;
            if (!(neg ? sl->rps.u0[q] : sl->rps.u1[q - sl->rps.num_neg])) continue;
            int e = hevc_dpb_of_poc(v, j->cur_poc + (neg ? sl->rps.d0[q] : sl->rps.d1[q - sl->rps.num_neg]));
            if (e < 0) continue;
            if (neg) dp->poc_st_curr_before[dp->num_poc_st_curr_before++] = e;
            else dp->poc_st_curr_after[dp->num_poc_st_curr_after++] = e;
        }
        int t = sl->nal_type;
        dp->flags = (t >= 16 && t <= 23 ? V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC : 0) |
                    (t == 19 || t == 20 ? V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC : 0);
    }
    if (v->failed) return -1;
    /* the previous slice segment goes now, holding the capture buffer for this one */
    if (flush_pending(v, 1)) { v->failed = 1; return -1; }
    if (sl->num_ep > 256) { ERR("v4l2req: %d entry points in one slice (256 at most)", sl->num_ep); v->failed = 1; return -1; }
    Map *o = &v->out[v->next_out];
    int sc = v->dev.start_code == V4L2_STATELESS_HEVC_START_CODE_ANNEX_B ? 3 : 0;
    if (!o->p || sc + j->nal_len > (int)o->len) { ERR("v4l2req: slice does not fit the input buffer"); v->failed = 1; return -1; }
    uint8_t *d = o->p;
    if (sc) { d[0] = 0; d[1] = 0; d[2] = 1; }
    memcpy(d + sc, j->nal, j->nal_len);
    v->len = sc + j->nal_len;

    struct v4l2_ctrl_hevc_slice_params *sp = &v->hsp;
    memset(sp, 0, sizeof *sp);
    sp->bit_size = v->len * 8;
    sp->data_byte_offset = sc + sl->data_byte;
    sp->num_entry_point_offsets = sl->num_ep;
    sp->nal_unit_type = sl->nal_type;
    sp->nuh_temporal_id_plus1 = sl->tid + 1;
    sp->slice_type = sl->type;                       /* HEVC_B/P/I = V4L2_HEVC_SLICE_TYPE_B/P/I */
    sp->slice_pic_order_cnt = j->cur_poc;
    if (sl->type != HEVC_I) sp->num_ref_idx_l0_active_minus1 = sl->num_ref[0];
    if (sl->type == HEVC_B) sp->num_ref_idx_l1_active_minus1 = sl->num_ref[1];
    sp->collocated_ref_idx = sl->col_ref_idx;
    sp->five_minus_max_num_merge_cand = sl->max_merge;
    sp->slice_qp_delta = sl->qp_delta;
    sp->slice_cb_qp_offset = sl->cb_qp;
    sp->slice_cr_qp_offset = sl->cr_qp;
    sp->slice_beta_offset_div2 = sl->beta;
    sp->slice_tc_offset_div2 = sl->tc;
    sp->slice_segment_addr = sl->address;
    for (int l = 0; l < 2; l++) {
        if (l == 0 ? sl->type == HEVC_I : sl->type != HEVC_B) continue;
        const int *list = l ? j->l1 : j->l0;
        uint8_t *ri = l ? sp->ref_idx_l1 : sp->ref_idx_l0;
        for (int i = 0; i <= sl->num_ref[l] && i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
            int slot = list[i];
            ri[i] = slot >= 0 && slot < MAX_SLOTS && v->dpb_index[slot] >= 0 ? v->dpb_index[slot] : 0;
        }
    }
    struct v4l2_hevc_pred_weight_table *pw = &sp->pred_weight_table;
    if ((p->weighted_pred && sl->type == HEVC_P) || (p->weighted_bipred && sl->type == HEVC_B)) {
        pw->luma_log2_weight_denom = sl->luma_denom;
        pw->delta_chroma_log2_weight_denom = sl->chroma_denom - sl->luma_denom;
        for (int l = 0; l < (sl->type == HEVC_B ? 2 : 1); l++)
            for (int i = 0; i <= sl->num_ref[l] && i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
                (l ? pw->delta_luma_weight_l1 : pw->delta_luma_weight_l0)[i] = sl->dlw[l][i];
                (l ? pw->luma_offset_l1 : pw->luma_offset_l0)[i] = sl->lo[l][i];
                for (int c = 0; c < 2; c++) {
                    (l ? pw->delta_chroma_weight_l1 : pw->delta_chroma_weight_l0)[i][c] = sl->dcw[l][i][c];
                    (l ? pw->chroma_offset_l1 : pw->chroma_offset_l0)[i][c] = sl->co[l][i][c];
                }
            }
    }
    sp->flags = (sl->sao_luma ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA : 0) |
                (sl->sao_chroma ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA : 0) |
                (sl->temporal_mvp ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED : 0) |
                (sl->mvd_l1_zero ? V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO : 0) |
                (sl->cabac_init ? V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT : 0) |
                (sl->col_from_l0 ? V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0 : 0) |
                (sl->deblock_disabled ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED : 0) |
                (sl->lf_across ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED : 0);
    v->hnep = sl->num_ep;
    if (sl->num_ep) memcpy(v->hep, j->ep_unescaped, sl->num_ep * sizeof *v->hep);
    v->pending = 1;
    return 0;
}

static int v_end_picture(void *hw, int slot)
{
    V *v = hw;
    if (slot != v->cur) { ERR("v4l2req: end of picture for slot %d, collected %d", slot, v->cur); v->failed = 1; }
    int rc;
    if (v->failed || (v->dev.slice_mode ? !v->pending : !v->len)) {
        rc = -1;
    } else if (v->dev.slice_mode) {
        rc = flush_pending(v, 0);                     /* the last slice releases the capture buffer */
    } else {
        rc = queue_capture(v) ? -1 : send_request(v, v->next_out, 0);
    }
    v->pending = 0;
    if (!v->cap_queued) { v->len = 0; return -1; }
    if (rc) {                                         /* queued, but no clean decode (or held for slices that never came) */
        drop_capture(v);
        v->len = 0;
        return -1;
    }
    /* the capture buffer back */
    struct v4l2_plane dp;
    struct v4l2_buffer b;
    buf_init(v, &b, &dp, CAP_T(v), v->heap ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP, 0);
    for (int tries = 0; tries < 200; tries++) {
        if (xioctl(v->vfd, VIDIOC_DQBUF, &b) == 0) break;
        if (errno != EAGAIN || rc) break;
        usleep(1000);
    }
    v->cap_queued = 0;
    if (!rc && (int)b.index != slot) { ERR("v4l2req: got capture buffer %u, expected %d", b.index, slot); rc = 1; }
    if (!rc && (b.flags & V4L2_BUF_FLAG_ERROR)) rc = 1;
    v->len = 0;
    return rc;
}

/* a whole frame, tag included, in one request */
static int v_vp8_frame(void *hw, const Vp8Job *j)
{
    V *v = hw;
    const Vp8Hdr *h = j->h;
    Map *o = &v->out[v->next_out];
    if (!o->p || j->len > (int)o->len) { ERR("v4l2req: frame does not fit the input buffer"); return -1; }
    memcpy(o->p, j->data, j->len);
    v->len = j->len;
    v->failed = v->pending = v->cap_queued = v->nsent = 0;
    v->cur = j->cur;
    v->cur_ts = ++v->counter;
    v->ts[j->cur] = v->cur_ts * 1000000000ull;
    struct v4l2_ctrl_vp8_frame *f = &v->vp8;
    memset(f, 0, sizeof *f);
    memcpy(f->segment.quant_update, h->seg_q, sizeof h->seg_q);
    memcpy(f->segment.lf_update, h->seg_lf, sizeof h->seg_lf);
    memcpy(f->segment.segment_probs, h->seg_probs, sizeof h->seg_probs);
    f->segment.flags = (h->seg_enabled ? V4L2_VP8_SEGMENT_FLAG_ENABLED : 0) |
                       (h->seg_update_map ? V4L2_VP8_SEGMENT_FLAG_UPDATE_MAP : 0) |
                       (h->seg_update_data ? V4L2_VP8_SEGMENT_FLAG_UPDATE_FEATURE_DATA : 0) |
                       (h->seg_abs ? 0 : V4L2_VP8_SEGMENT_FLAG_DELTA_VALUE_MODE);
    memcpy(f->lf.ref_frm_delta, h->ref_delta, sizeof h->ref_delta);
    memcpy(f->lf.mb_mode_delta, h->mode_delta, sizeof h->mode_delta);
    f->lf.sharpness_level = h->sharpness;
    f->lf.level = h->lf_level;
    f->lf.flags = (h->lf_delta_enabled ? V4L2_VP8_LF_ADJ_ENABLE : 0) | (h->lf_delta_update ? V4L2_VP8_LF_DELTA_UPDATE : 0) |
                  (h->filter_simple ? V4L2_VP8_LF_FILTER_TYPE_SIMPLE : 0);
    f->quant.y_ac_qi = h->y_ac_qi;
    f->quant.y_dc_delta = h->y_dc;
    f->quant.y2_dc_delta = h->y2_dc;
    f->quant.y2_ac_delta = h->y2_ac;
    f->quant.uv_dc_delta = h->uv_dc;
    f->quant.uv_ac_delta = h->uv_ac;
    memcpy(f->entropy.coeff_probs, h->e.coeff, sizeof h->e.coeff);
    memcpy(f->entropy.y_mode_probs, h->e.y_mode, sizeof h->e.y_mode);
    memcpy(f->entropy.uv_mode_probs, h->e.uv_mode, sizeof h->e.uv_mode);
    memcpy(f->entropy.mv_probs, h->e.mv, sizeof h->e.mv);
    f->coder_state.range = h->coder_range;
    f->coder_state.value = h->coder_value;
    f->coder_state.bit_count = h->coder_bits;
    f->width = h->width;
    f->height = h->height;
    f->horizontal_scale = h->hscale;
    f->vertical_scale = h->vscale;
    f->version = h->version;
    f->prob_skip_false = h->prob_skip;
    f->prob_intra = h->prob_intra;
    f->prob_last = h->prob_last;
    f->prob_gf = h->prob_gf;
    f->num_dct_parts = 1 << h->log2_parts;
    f->first_part_size = h->first_part_size;
    f->first_part_header_bits = h->header_bits;
    for (int i = 0; i < f->num_dct_parts; i++) f->dct_part_sizes[i] = h->part_size[i];
    if (j->last >= 0) f->last_frame_ts = v->ts[j->last];
    if (j->golden >= 0) f->golden_frame_ts = v->ts[j->golden];
    if (j->alt >= 0) f->alt_frame_ts = v->ts[j->alt];
    f->flags = (h->key ? V4L2_VP8_FRAME_FLAG_KEY_FRAME : 0) | (h->show ? V4L2_VP8_FRAME_FLAG_SHOW_FRAME : 0) |
               (h->no_coeff_skip ? V4L2_VP8_FRAME_FLAG_MB_NO_SKIP_COEFF : 0) |
               (h->sign_golden ? V4L2_VP8_FRAME_FLAG_SIGN_BIAS_GOLDEN : 0) | (h->sign_alt ? V4L2_VP8_FRAME_FLAG_SIGN_BIAS_ALT : 0);
    return v_end_picture(hw, j->cur);
}

static void v_export(void *hw, int slot, PmhwdecPicture *pic)
{
    V *v = hw;
    uint8_t *base = v->cap[slot].p;
    if (v->heap) {                                    /* the decoder wrote behind the CPU caches */
        cpu_sync(v->cap_fd[slot], DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        cpu_sync(v->cap_fd[slot], DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    }
    pic->format = PMHWDEC_FMT_NV12;
    pic->plane[0] = base;
    pic->plane[1] = base + v->cap_off1;
    pic->stride[0] = pic->stride[1] = v->bpl;
    pic->dmabuf_fd = v->cap_fd[slot];
    pic->drm_fourcc = 0x3231564e;                     /* DRM_FORMAT_NV12 */
    pic->drm_modifier = 0;
    pic->offset[0] = 0;
    pic->offset[1] = v->cap_off1;
}

static void v_close(void *hw)
{
    V *v = hw;
    if (!v) return;
    teardown(v);
    close(v->vfd);
    close(v->mfd);
    free(v);
}

static void *v_open_ops(PmhwdecCodec codec) { return v_open(codec); }

const HwOps pmhwdec_v4l2req_ops = {
    "v4l2req", v_query, v_open_ops, v_configure, v_h264_slice, v_hevc_slice, v_export, v_close, v_end_picture, v_vp8_frame,
};
