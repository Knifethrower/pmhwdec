// SPDX-License-Identifier: GPL-2.0-or-later
/* pmhw-cedard: the Allwinner video engine decoder (cedar.c, ported from Linux Cedrus, GPL) as its own program.
 * libpmhwdec (LGPL) starts one per decoder with a socketpair on fd 3 (CD_FD) and talks cedar_ipc.h to it; the
 * helper runs the shared stateless core (stateless.c, h264.c, hevc.c) over cedar.c's HwOps and hands pictures back
 * as dma-buf fds. It exits when the socket closes, so a crashed or killed caller leaves nothing behind. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "internal.h"
#include "cedar_ipc.h"

int pmhw_log_on;
static int max_depth = 8;
extern unsigned pmhw_cedar_gen;

/* the client ran the self-test: it tells us at OPEN how deep we may go */
int pmhw_selftest_depth_allowed(const char *backend, PmhwCodec codec, int depth)
{
    (void)backend; (void)codec;
    return depth <= max_depth;
}

static int read_full(int fd, void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r; n -= r;
    }
    return 0;
}

static int send_rep(int fd, const CdRep *rep, int pass_fd)
{
    struct iovec iov = { .iov_base = (void *)rep, .iov_len = sizeof *rep };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (pass_fd >= 0) {
        memset(&u, 0, sizeof u);
        m.msg_control = u.b;
        m.msg_controllen = sizeof u.b;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    ssize_t r;
    do r = sendmsg(fd, &m, MSG_NOSIGNAL); while (r < 0 && errno == EINTR);
    return r == (ssize_t)sizeof *rep ? 0 : -1;
}

int main(void)
{
    const char *l = getenv("PMHWDEC_LOG");
    pmhw_log_on = l && *l && strcmp(l, "0");
    const Backend *be = pmhw_stateless_backend(&pmhw_cedar_ops);
    int fd = CD_FD;
    void *ctx = NULL;
    uint8_t *buf = NULL;
    uint32_t cap = 0;
    CdReq q;
    while (read_full(fd, &q, sizeof q) == 0) {
        if (q.version != CD_VERSION) { ERR("pmhw-cedard: protocol %u, want %u", q.version, CD_VERSION); break; }
        CdRep rep = { 0 };
        int pass = -1;
        switch (q.op) {
        case CD_QUERY: {
            PmhwCaps caps;
            rep.status = be->query((PmhwCodec)q.codec, &caps);
            if (!rep.status) { rep.max_width = caps.max_width; rep.max_height = caps.max_height; rep.max_bit_depth = caps.max_bit_depth; }
            break;
        }
        case CD_OPEN:
            max_depth = q.max_depth > 8 ? q.max_depth : 8;
            if (!ctx) ctx = be->open((PmhwCodec)q.codec, q.width, q.height);
            rep.status = ctx ? 0 : -1;
            break;
        case CD_SEND:
            if (q.len > cap) {
                uint8_t *n = realloc(buf, q.len);
                if (!n) { ERR("pmhw-cedard: out of memory (%u bytes)", q.len); goto out; }
                buf = n; cap = q.len;
            }
            if (q.len && read_full(fd, buf, q.len)) goto out;
            rep.status = ctx ? be->send(ctx, q.len ? buf : NULL, (int)q.len, q.pts) : PMHW_ERROR;
            break;
        case CD_GET: {
            PmhwPicture pic;
            rep.status = ctx ? be->get(ctx, &pic) : 0;
            if (rep.status == 1) {
                if (pic.dmabuf_fd < 0) { be->put(ctx, &pic); rep.status = -1; break; }
                rep.width = pic.width; rep.height = pic.height; rep.left = pic.left; rep.top = pic.top;
                rep.stride0 = pic.stride[0]; rep.stride1 = pic.stride[1]; rep.corrupt = pic.corrupt;
                rep.offset0 = pic.offset[0]; rep.offset1 = pic.offset[1]; rep.fourcc = pic.drm_fourcc;
                rep.modifier = pic.drm_modifier; rep.pts = pic.pts;
                rep.token = (uint32_t)(intptr_t)pic.priv;
                rep.gen = pmhw_cedar_gen;
                pass = pic.dmabuf_fd;
            }
            break;
        }
        case CD_PUT:
            if (ctx) { PmhwPicture pic = { .priv = (void *)(intptr_t)q.token }; be->put(ctx, &pic); }
            continue;                                     /* no reply */
        case CD_FLUSH:
            if (ctx) be->flush(ctx);
            break;
        default:
            ERR("pmhw-cedard: unknown request %u", q.op);
            goto out;
        }
        if (send_rep(fd, &rep, pass)) break;
    }
out:
    if (ctx) be->close(ctx);
    free(buf);
    return 0;
}
