// SPDX-License-Identifier: LGPL-2.1-or-later
/* "v4l2m2m": V4L2 stateful (memory-to-memory) decoders on mainline kernels: Qualcomm Venus / Iris (ROCKNIX on
 * SM8250 RP5 / RP Mini / Flip 2, SM8550, SM8650, SM8750, SM6115, SM4450), Amlogic meson-vdec. The driver parses the
 * stream and reorders, so this is a stream backend: access units in, NV12 pictures out in display order.
 * Follows Documentation/userspace-api/media/v4l/dev-decoder.rst (initialization by source-change event, drain with
 * V4L2_DEC_CMD_STOP, seek by restarting the OUTPUT queue). */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include "internal.h"

#define OUT_BUFS 8
#define OUT_SIZE (2 << 20)
#define MAX_CAP 32
#define CAP_EXTRA (SL_EXTRA_SLOTS + 2)        /* beyond the driver's minimum: held by the caller, in flight */

typedef struct M2MDev { char video[32]; int max_w, max_h; } M2MDev;

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

static uint32_t fourcc(PmhwCodec c)
{
    return c == PMHW_CODEC_H264 ? V4L2_PIX_FMT_H264 : c == PMHW_CODEC_HEVC ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_VP9;
}

static int has_fmt(int fd, uint32_t type, uint32_t f)
{
    struct v4l2_fmtdesc d = { .type = type };
    for (d.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &d) == 0; d.index++)
        if (d.pixelformat == f) return 1;
    return 0;
}

static int find_device(PmhwCodec codec, M2MDev *dev)
{
    if (codec != PMHW_CODEC_H264 && codec != PMHW_CODEC_HEVC) return -1;
    for (int i = 0; i < 64; i++) {
        snprintf(dev->video, sizeof dev->video, "/dev/video%d", i);
        int fd = open(dev->video, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        int ok = 0;
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            uint32_t c = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
            ok = (c & V4L2_CAP_VIDEO_M2M_MPLANE) && has_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, fourcc(codec)) &&
                 has_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_NV12);
        }
        if (ok) {
            dev->max_w = 4096; dev->max_h = 2304;
            struct v4l2_frmsizeenum fs = { .pixel_format = fourcc(codec) };
            if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0) {
                if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) { dev->max_w = fs.discrete.width; dev->max_h = fs.discrete.height; }
                else { dev->max_w = fs.stepwise.max_width; dev->max_h = fs.stepwise.max_height; }
            }
            LOG("v4l2m2m: %s = %s (%s), up to %dx%d", dev->video, cap.card, cap.driver, dev->max_w, dev->max_h);
        }
        close(fd);
        if (ok) return 0;
    }
    return -1;
}

static int m_query(PmhwCodec codec, PmhwCaps *caps)
{
    static M2MDev cache[PMHW_CODEC_COUNT];
    static int tried[PMHW_CODEC_COUNT], found[PMHW_CODEC_COUNT];
    if (codec >= PMHW_CODEC_COUNT) return -1;
    if (!tried[codec]) { tried[codec] = 1; found[codec] = find_device(codec, &cache[codec]) == 0; }
    if (!found[codec]) return -1;
    caps->max_width = cache[codec].max_w;
    caps->max_height = cache[codec].max_h;
    caps->max_bit_depth = 8;                  /* 10-bit output (P010 / QC10C) not handled yet */
    caps->backend = "v4l2m2m";
    return 0;
}

/* ---------------- decoder ---------------- */
typedef struct Map { void *p; size_t len; } Map;

typedef struct M {
    PmhwCodec codec;
    M2MDev dev;
    int fd;
    Map out[OUT_BUFS];
    int out_busy[OUT_BUFS], nout;
    Map cap[MAX_CAP];
    int cap_fd[MAX_CAP], ncap, cap_on, heap, cap_planes;
    int held[MAX_CAP];
    uint32_t bpl, uv_off, uv_bpl;
    int vis_x, vis_y, vis_w, vis_h;
    int eos_sent, eos_seen, unsupported;
    double waiting_since, last_progress;     /* drivers that refuse a stream may say nothing (Iris: 10-bit) */
} M;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void *m_open(PmhwCodec codec, int width, int height)
{
    PmhwCaps caps;
    if (m_query(codec, &caps) || width > caps.max_width || height > caps.max_height) return NULL;
    M *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->codec = codec;
    for (int i = 0; i < MAX_CAP; i++) m->cap_fd[i] = -1;
    if (find_device(codec, &m->dev)) { free(m); return NULL; }
    m->fd = open(m->dev.video, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m->fd < 0) { free(m); return NULL; }
    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    f.fmt.pix_mp.pixelformat = fourcc(codec);
    f.fmt.pix_mp.width = width > 0 ? width : 1920;
    f.fmt.pix_mp.height = height > 0 ? height : 1080;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = OUT_SIZE;
    struct v4l2_requestbuffers rb = { .count = OUT_BUFS, .memory = V4L2_MEMORY_MMAP, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    if (xioctl(m->fd, VIDIOC_S_FMT, &f) || xioctl(m->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) ||
        xioctl(m->fd, VIDIOC_REQBUFS, &rb) || rb.count < 1) {
        ERR("v4l2m2m: output setup: %s", strerror(errno));
        close(m->fd); free(m);
        return NULL;
    }
    sub.type = V4L2_EVENT_EOS;
    xioctl(m->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    m->nout = rb.count > OUT_BUFS ? OUT_BUFS : rb.count;
    for (int i = 0; i < m->nout; i++) {
        struct v4l2_plane pl;
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP, .index = i,
                                 .length = 1, .m.planes = &pl };
        memset(&pl, 0, sizeof pl);
        if (xioctl(m->fd, VIDIOC_QUERYBUF, &b)) goto fail;
        m->out[i].len = pl.length;
        m->out[i].p = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, pl.m.mem_offset);
        if (m->out[i].p == MAP_FAILED) { m->out[i].p = NULL; goto fail; }
    }
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(m->fd, VIDIOC_STREAMON, &t)) goto fail;
    return m;
fail:
    ERR("v4l2m2m: output buffers: %s", strerror(errno));
    for (int i = 0; i < m->nout; i++) if (m->out[i].p) munmap(m->out[i].p, m->out[i].len);
    close(m->fd); free(m);
    return NULL;
}

static void free_capture(M *m)
{
    if (m->cap_on) { int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; xioctl(m->fd, VIDIOC_STREAMOFF, &t); m->cap_on = 0; }
    for (int i = 0; i < MAX_CAP; i++) {
        if (m->cap[i].p) munmap(m->cap[i].p, m->cap[i].len);
        m->cap[i].p = NULL;
        if (m->cap_fd[i] >= 0) close(m->cap_fd[i]);
        m->cap_fd[i] = -1;
        m->held[i] = 0;
    }
    struct v4l2_requestbuffers rb = { .count = 0, .memory = m->heap ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
                                      .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    xioctl(m->fd, VIDIOC_REQBUFS, &rb);
    m->ncap = 0;
    m->heap = 0;
}

static int queue_cap(M *m, int i)
{
    struct v4l2_plane pl[2];
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .index = i, .length = m->cap_planes, .m.planes = pl };
    memset(pl, 0, sizeof pl);
    b.memory = m->heap ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    if (m->heap) { pl[0].m.fd = m->cap_fd[i]; pl[0].length = m->cap[i].len; }
    return xioctl(m->fd, VIDIOC_QBUF, &b);
}

/* source change: (re)allocate the capture queue for the stream the driver found */
static int setup_capture(M *m)
{
    free_capture(m);
    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    if (xioctl(m->fd, VIDIOC_G_FMT, &f)) return -1;
    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12) {
        f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        if (xioctl(m->fd, VIDIOC_S_FMT, &f) || f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12) {
            ERR("v4l2m2m: the decoder does not give NV12 for this stream (10-bit?)");
            m->unsupported = 1;
            return -1;
        }
    }
    m->cap_planes = f.fmt.pix_mp.num_planes;
    if (m->cap_planes != 1) { ERR("v4l2m2m: %d-plane NV12 not handled", m->cap_planes); m->unsupported = 1; return -1; }
    m->bpl = m->uv_bpl = f.fmt.pix_mp.plane_fmt[0].bytesperline;
    /* Venus / Iris pad the luma plane to 32 lines (venus_helper_get_framesz); others: the reported height */
    uint32_t h = f.fmt.pix_mp.height;
    struct v4l2_capability cap;
    if (xioctl(m->fd, VIDIOC_QUERYCAP, &cap) == 0 && (strstr((char *)cap.driver, "venus") || strstr((char *)cap.driver, "iris")))
        h = ALIGN(h, 32);
    m->uv_off = m->bpl * h;
    struct v4l2_selection sel = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .target = V4L2_SEL_TGT_COMPOSE };
    if (xioctl(m->fd, VIDIOC_G_SELECTION, &sel) == 0) {
        m->vis_x = sel.r.left; m->vis_y = sel.r.top; m->vis_w = sel.r.width; m->vis_h = sel.r.height;
    } else {
        m->vis_x = m->vis_y = 0; m->vis_w = f.fmt.pix_mp.width; m->vis_h = f.fmt.pix_mp.height;
    }
    struct v4l2_control c = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
    int n = (xioctl(m->fd, VIDIOC_G_CTRL, &c) == 0 ? c.value : 8) + CAP_EXTRA;
    if (n > MAX_CAP) n = MAX_CAP;
    size_t size = ALIGN(f.fmt.pix_mp.plane_fmt[0].sizeimage, 4096);
    LOG("v4l2m2m: capture NV12 %ux%u (visible %dx%d at %d,%d), %u bytes per line, uv at %u, %d buffers",
        f.fmt.pix_mp.width, f.fmt.pix_mp.height, m->vis_w, m->vis_h, m->vis_x, m->vis_y, m->bpl, m->uv_off, n);
    /* cached dma_heap buffers when the decoder has an IOMMU (CPU copies from uncached memory are ~10x slower) */
    char p[128];
    snprintf(p, sizeof p, "/sys/class/video4linux/%s/device/iommu_group", strrchr(m->dev.video, '/') + 1);
    const char *env = getenv("PMHWDEC_V4L2_DMABUF");
    int hfd = (!env || strcmp(env, "0")) && access(p, F_OK) == 0 ? open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC) : -1;
    if (hfd >= 0) {
        struct v4l2_requestbuffers rb = { .count = n, .memory = V4L2_MEMORY_DMABUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
        if (xioctl(m->fd, VIDIOC_REQBUFS, &rb) == 0 && (int)rb.count >= n) {
            m->heap = 1;
            for (int i = 0; i < n; i++) {
                struct dma_heap_allocation_data a = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
                if (xioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a)) { m->heap = 0; break; }
                m->cap_fd[i] = a.fd;
                m->cap[i].len = size;
                m->cap[i].p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
                if (m->cap[i].p == MAP_FAILED) { m->cap[i].p = NULL; m->heap = 0; break; }
            }
        }
        close(hfd);
        if (!m->heap) { m->heap = 1; free_capture(m); }    /* undo a partial heap setup */
    }
    if (!m->heap) {
        struct v4l2_requestbuffers rb = { .count = n, .memory = V4L2_MEMORY_MMAP, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
        if (xioctl(m->fd, VIDIOC_REQBUFS, &rb) || (int)rb.count < 1) return -1;
        n = rb.count > MAX_CAP ? MAX_CAP : rb.count;
        for (int i = 0; i < n; i++) {
            struct v4l2_plane pl;
            struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP, .index = i,
                                     .length = 1, .m.planes = &pl };
            memset(&pl, 0, sizeof pl);
            if (xioctl(m->fd, VIDIOC_QUERYBUF, &b)) return -1;
            m->cap[i].len = pl.length;
            m->cap[i].p = mmap(NULL, pl.length, PROT_READ, MAP_SHARED, m->fd, pl.m.mem_offset);
            if (m->cap[i].p == MAP_FAILED) { m->cap[i].p = NULL; return -1; }
            struct v4l2_exportbuffer e = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .index = i, .flags = O_RDONLY | O_CLOEXEC };
            m->cap_fd[i] = xioctl(m->fd, VIDIOC_EXPBUF, &e) == 0 ? e.fd : -1;
        }
    }
    m->ncap = n;
    m->waiting_since = 0;
    m->last_progress = now();
    LOG("v4l2m2m: capture buffers: %s", m->heap ? "cached dma_heap/system" : "MMAP");
    for (int i = 0; i < n; i++) if (queue_cap(m, i)) { ERR("v4l2m2m: QBUF capture: %s", strerror(errno)); return -1; }
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(m->fd, VIDIOC_STREAMON, &t)) return -1;
    m->cap_on = 1;
    return 0;
}

/* events (source change, EOS) and finished OUTPUT buffers */
static void pump(M *m)
{
    struct v4l2_event ev;
    while (xioctl(m->fd, VIDIOC_DQEVENT, &ev) == 0) {
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE && setup_capture(m)) ERR("v4l2m2m: capture setup failed");
        if (ev.type == V4L2_EVENT_EOS) m->eos_seen = 1;
    }
    struct v4l2_plane pl;
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &pl };
    for (;;) {
        memset(&pl, 0, sizeof pl);
        if (xioctl(m->fd, VIDIOC_DQBUF, &b)) break;
        if (b.index < OUT_BUFS) m->out_busy[b.index] = 0;
        m->last_progress = now();
    }
}

static int m_send(void *ctx, const uint8_t *data, int size, int64_t pts)
{
    M *m = ctx;
    if (m->unsupported) return PMHW_UNSUPPORTED;
    pump(m);
    if (!data || size <= 0) {
        struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_STOP };
        xioctl(m->fd, VIDIOC_DECODER_CMD, &cmd);
        m->eos_sent = 1;
        return PMHW_OK;
    }
    int i = 0;
    while (i < m->nout && m->out_busy[i]) i++;
    if (i == m->nout) {                               /* all in the decoder: wait a little, then let the caller drain */
        struct pollfd pf = { .fd = m->fd, .events = POLLOUT | POLLIN | POLLPRI };
        poll(&pf, 1, 5);
        pump(m);
        double t = now();
        if (!m->cap_on) {                             /* fed, but the driver never set up its output: refused */
            if (!m->waiting_since) m->waiting_since = t;
            else if (t - m->waiting_since > 3.0) {
                ERR("v4l2m2m: the decoder did not start on this stream (unsupported, e.g. 10-bit)");
                m->unsupported = 1;
                return PMHW_UNSUPPORTED;
            }
        } else if (m->last_progress && t - m->last_progress > 5.0) {
            ERR("v4l2m2m: the decoder makes no progress");
            m->last_progress = t;
            return PMHW_ERROR;
        }
        return PMHW_AGAIN;
    }
    if ((size_t)size > m->out[i].len) { ERR("v4l2m2m: access unit of %d bytes too big", size); return PMHW_ERROR; }
    memcpy(m->out[i].p, data, size);
    struct v4l2_plane pl = { .bytesused = size, .length = m->out[i].len };
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP, .index = i,
                             .length = 1, .m.planes = &pl };
    int64_t t = pts < 0 ? 0 : pts;                    /* pts travels in the buffer timestamp (copied to the capture side) */
    b.timestamp.tv_sec = t / 1000000;
    b.timestamp.tv_usec = t % 1000000;
    if (xioctl(m->fd, VIDIOC_QBUF, &b)) { ERR("v4l2m2m: QBUF output: %s", strerror(errno)); return PMHW_ERROR; }
    m->out_busy[i] = 1;
    return PMHW_OK;
}

static int m_get(void *ctx, PmhwPicture *pic)
{
    M *m = ctx;
    double deadline = now() + 2.0;
    for (;;) {
        pump(m);
        if (m->cap_on) {
            struct v4l2_plane pl[2];
            struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .length = m->cap_planes, .m.planes = pl };
            b.memory = m->heap ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
            memset(pl, 0, sizeof pl);
            if (xioctl(m->fd, VIDIOC_DQBUF, &b) == 0) {
                m->last_progress = now();
                if (b.flags & V4L2_BUF_FLAG_LAST) m->eos_seen = 1;
                if (!pl[0].bytesused || b.index >= (unsigned)m->ncap) {   /* empty (e.g. the LAST marker): give it back */
                    if (b.index < (unsigned)m->ncap && !m->eos_seen) queue_cap(m, b.index);
                    if (m->eos_seen) return 0;
                    continue;
                }
                if (m->heap) {
                    struct dma_buf_sync s = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
                    xioctl(m->cap_fd[b.index], DMA_BUF_IOCTL_SYNC, &s);
                    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                    xioctl(m->cap_fd[b.index], DMA_BUF_IOCTL_SYNC, &s);
                }
                uint8_t *base = m->cap[b.index].p;
                memset(pic, 0, sizeof *pic);
                pic->width = m->vis_w; pic->height = m->vis_h;
                pic->left = m->vis_x; pic->top = m->vis_y;
                pic->format = PMHW_FMT_NV12;
                pic->plane[0] = base;
                pic->plane[1] = base + m->uv_off;
                pic->stride[0] = m->bpl; pic->stride[1] = m->uv_bpl;
                pic->pts = (int64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
                pic->corrupt = !!(b.flags & V4L2_BUF_FLAG_ERROR);
                pic->dmabuf_fd = m->cap_fd[b.index];
                pic->drm_fourcc = 0x3231564e;
                pic->offset[1] = m->uv_off;
                pic->priv = (void *)(intptr_t)(b.index + 1);
                m->held[b.index] = 1;
                return 1;
            }
            if (errno == EPIPE) m->eos_seen = 1;      /* after the LAST buffer */
        }
        if (m->unsupported || !m->eos_sent || m->eos_seen || now() > deadline) return 0;
        struct pollfd pf = { .fd = m->fd, .events = POLLIN | POLLPRI };
        poll(&pf, 1, 10);                             /* draining: wait for the remaining pictures */
    }
}

static void m_put(void *ctx, PmhwPicture *pic)
{
    M *m = ctx;
    int i = (int)(intptr_t)pic->priv - 1;
    if (i >= 0 && i < m->ncap && m->held[i]) {
        m->held[i] = 0;
        if (m->cap_on && !m->eos_seen) queue_cap(m, i);
    }
    pic->priv = NULL;
}

static void m_flush(void *ctx)
{
    M *m = ctx;
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;        /* seek: restart the OUTPUT queue; capture keeps going */
    xioctl(m->fd, VIDIOC_STREAMOFF, &t);
    memset(m->out_busy, 0, sizeof m->out_busy);
    xioctl(m->fd, VIDIOC_STREAMON, &t);
    if (m->eos_seen && m->cap_on) {                   /* after a drain the capture queue needs a restart too */
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(m->fd, VIDIOC_STREAMOFF, &t);
        xioctl(m->fd, VIDIOC_STREAMON, &t);
        for (int i = 0; i < m->ncap; i++) if (!m->held[i]) queue_cap(m, i);
        struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_START };
        xioctl(m->fd, VIDIOC_DECODER_CMD, &cmd);
    }
    m->eos_sent = m->eos_seen = 0;
}

static void m_close(void *ctx)
{
    M *m = ctx;
    if (!m) return;
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(m->fd, VIDIOC_STREAMOFF, &t);
    free_capture(m);
    for (int i = 0; i < m->nout; i++) if (m->out[i].p) munmap(m->out[i].p, m->out[i].len);
    struct v4l2_requestbuffers rb = { .count = 0, .memory = V4L2_MEMORY_MMAP, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    xioctl(m->fd, VIDIOC_REQBUFS, &rb);
    close(m->fd);
    free(m);
}

const Backend pmhw_v4l2m2m_backend = { "v4l2m2m", m_query, m_open, m_send, m_get, m_put, m_flush, m_close };
