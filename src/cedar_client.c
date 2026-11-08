// SPDX-License-Identifier: LGPL-2.1-or-later
/* "cedar" backend, client side: the Allwinner video engine decoder runs in pmhw-cedard (cedard.c + cedar.c, GPL,
 * shipped next to libpmhwdec), one helper process per decoder; this file only talks cedar_ipc.h to it. Pictures come
 * back as dma-buf fds, mapped here once per buffer (the helper flushes the CPU caches before it hands one over).
 * A helper that dies or does not answer within CD_TIMEOUT_MS ends the decoder with PMHW_UNSUPPORTED, so the caller
 * goes on in software. PMHWDEC_CEDARD=<path> picks another helper binary. */
#define _GNU_SOURCE                     /* dladdr */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "internal.h"
#include "cedar_ipc.h"

extern char **environ;

#define CD_TIMEOUT_MS 3000
#define MAPS 40

typedef struct Map { dev_t dev; ino_t ino; int fd, refs; uint8_t *p; size_t len; } Map;

typedef struct CC {
    pid_t pid;
    int sock, dead;
    unsigned gen;
    Map map[MAPS];
} CC;

static int helper_path(char *out, size_t n)
{
    const char *e = getenv("PMHWDEC_CEDARD");
    if (e && *e) { snprintf(out, n, "%s", e); return access(out, X_OK); }
    /* next to libpmhwdec.so, or next to the program that links libpmhwdec.a (pmhw-test) */
    Dl_info info;
    char exe[512];
    const char *where[2] = { dladdr((void *)helper_path, &info) ? info.dli_fname : NULL, NULL };
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0) { exe[k] = 0; where[1] = exe; }
    for (int i = 0; i < 2; i++) {
        const char *slash = where[i] ? strrchr(where[i], '/') : NULL;
        if (!slash) continue;
        snprintf(out, n, "%.*s/pmhw-cedard", (int)(slash - where[i]), where[i]);
        if (access(out, X_OK) == 0) return 0;
    }
    return -1;
}

static void stop(CC *c)
{
    if (c->sock >= 0) close(c->sock);
    c->sock = -1;
    if (c->pid > 0) {
        int st;
        for (int i = 0; i < 100 && waitpid(c->pid, &st, WNOHANG) == 0; i++) usleep(10000);
        if (waitpid(c->pid, &st, WNOHANG) == 0) { kill(c->pid, SIGKILL); waitpid(c->pid, &st, 0); }
    }
    c->pid = 0;
    c->dead = 1;
}

static int start(CC *c)
{
    char path[512];
    c->sock = -1;
    c->pid = 0;
    if (helper_path(path, sizeof path)) { LOG("cedar: no pmhw-cedard helper next to libpmhwdec"); return -1; }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return -1;
    if (sv[1] == CD_FD) {                             /* dup2() onto itself would keep CLOEXEC */
        int t = fcntl(sv[1], F_DUPFD_CLOEXEC, CD_FD + 1);
        close(sv[1]);
        sv[1] = t;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], CD_FD);
    char *argv[] = { "pmhw-cedard", NULL };
    int r = posix_spawn(&c->pid, path, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);
    if (r) { ERR("cedar: starting %s: %s", path, strerror(r)); close(sv[0]); c->pid = 0; return -1; }
    c->sock = sv[0];
    c->dead = 0;
    return 0;
}

static int write_all(int fd, const void *a, size_t na, const void *b, size_t nb)
{
    struct iovec iov[2] = { { (void *)a, na }, { (void *)b, nb } };
    int n = nb ? 2 : 1;
    struct iovec *v = iov;
    while (n) {
        struct msghdr m = { .msg_iov = v, .msg_iovlen = n };
        ssize_t r = sendmsg(fd, &m, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        while (n && (size_t)r >= v->iov_len) { r -= v->iov_len; v++; n--; }
        if (n) { v->iov_base = (uint8_t *)v->iov_base + r; v->iov_len -= r; }
    }
    return 0;
}

/* one request, its reply (and the fd that may ride along); -1 = the helper is gone (and is stopped) */
static int call(CC *c, CdReq *q, const void *payload, int want_reply, CdRep *rep, int *fd_out)
{
    if (fd_out) *fd_out = -1;
    if (c->dead) return -1;
    q->version = CD_VERSION;
    if (write_all(c->sock, q, sizeof *q, payload, payload ? q->len : 0)) goto dead;
    if (!want_reply) return 0;
    uint8_t *b = (uint8_t *)rep;
    size_t got = 0;
    while (got < sizeof *rep) {
        struct pollfd pf = { .fd = c->sock, .events = POLLIN };
        int pr = poll(&pf, 1, CD_TIMEOUT_MS);
        if (pr < 0 && errno == EINTR) continue;
        if (pr != 1) { ERR("cedar: pmhw-cedard did not answer in %d ms", CD_TIMEOUT_MS); goto dead; }
        struct iovec iov = { b + got, sizeof *rep - got };
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b };
        ssize_t r = recvmsg(c->sock, &m, MSG_CMSG_CLOEXEC);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) goto dead;
        for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h))
            if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
                int fd;
                memcpy(&fd, CMSG_DATA(h), sizeof fd);
                if (fd_out && *fd_out < 0) *fd_out = fd; else close(fd);
            }
        got += r;
    }
    return 0;
dead:
    ERR("cedar: lost pmhw-cedard: this stream goes to software");
    if (fd_out && *fd_out >= 0) { close(*fd_out); *fd_out = -1; }
    stop(c);
    return -1;
}

/* ---------------- Backend ---------------- */
static int cc_query(PmhwCodec codec, PmhwCaps *caps)
{
    static int done[PMHW_CODEC_COUNT], ok[PMHW_CODEC_COUNT];
    static PmhwCaps cached[PMHW_CODEC_COUNT];
    if (codec != PMHW_CODEC_H264 && codec != PMHW_CODEC_HEVC && codec != PMHW_CODEC_VP8) return -1;
    if (!done[codec]) {
        done[codec] = 1;
        CC c = { 0 };
        if (access("/dev/cedar_dev", R_OK | W_OK) == 0 && start(&c) == 0) {
            CdReq q = { .op = CD_QUERY, .codec = codec };
            CdRep r;
            if (call(&c, &q, NULL, 1, &r, NULL) == 0 && r.status == 0) {
                ok[codec] = 1;
                cached[codec] = (PmhwCaps){ r.max_width, r.max_height, r.max_bit_depth, "cedar" };
            }
            stop(&c);
        }
    }
    if (!ok[codec]) return -1;
    *caps = cached[codec];
    return 0;
}

static void *cc_open(PmhwCodec codec, int width, int height)
{
    CC *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    for (int i = 0; i < MAPS; i++) c->map[i].fd = -1;
    if (start(c)) { free(c); return NULL; }
    CdReq q = { .op = CD_OPEN, .codec = codec, .width = width, .height = height,
                .max_depth = pmhw_selftest_depth_allowed("cedar", codec, 10) ? 10 : 8 };
    CdRep r;
    if (call(c, &q, NULL, 1, &r, NULL) || r.status) { stop(c); free(c); return NULL; }
    return c;
}

static int cc_send(void *ctx, const uint8_t *data, int size, int64_t pts)
{
    CC *c = ctx;
    CdReq q = { .op = CD_SEND, .pts = pts, .len = data && size > 0 ? (uint32_t)size : 0 };
    CdRep r;
    if (call(c, &q, q.len ? data : NULL, 1, &r, NULL)) return PMHW_UNSUPPORTED;
    return r.status;
}

static void unmap(Map *m)
{
    if (m->p) munmap(m->p, m->len);
    if (m->fd >= 0) close(m->fd);
    memset(m, 0, sizeof *m);
    m->fd = -1;
}

/* the mapping of this dma-buf: an old one (same buffer, fd closed), or a new one */
static int map_of(CC *c, int fd)
{
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return -1; }
    int free_i = -1;
    for (int i = 0; i < MAPS; i++) {
        Map *m = &c->map[i];
        if (m->p && m->dev == st.st_dev && m->ino == st.st_ino) { close(fd); return i; }
        if (!m->p && free_i < 0) free_i = i;
    }
    if (free_i < 0)                                    /* full: drop a mapping nobody holds */
        for (int i = 0; i < MAPS && free_i < 0; i++) if (!c->map[i].refs) { unmap(&c->map[i]); free_i = i; }
    off_t len = lseek(fd, 0, SEEK_END);
    if (free_i < 0 || len <= 0) { close(fd); return -1; }
    void *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { ERR("cedar: mapping a picture: %s", strerror(errno)); close(fd); return -1; }
    c->map[free_i] = (Map){ st.st_dev, st.st_ino, fd, 0, p, (size_t)len };
    return free_i;
}

static int cc_get(void *ctx, PmhwPicture *pic)
{
    CC *c = ctx;
    CdReq q = { .op = CD_GET };
    CdRep r;
    int fd;
    if (call(c, &q, NULL, 1, &r, &fd) || r.status != 1) return 0;
    if (r.gen != c->gen) {                             /* the helper reallocated: its old buffers are gone */
        for (int i = 0; i < MAPS; i++) if (c->map[i].p && !c->map[i].refs) unmap(&c->map[i]);
        c->gen = r.gen;
    }
    int i = fd >= 0 ? map_of(c, fd) : -1;
    if (i < 0) {
        CdReq p = { .op = CD_PUT, .token = r.token };
        call(c, &p, NULL, 0, NULL, NULL);
        return 0;
    }
    Map *m = &c->map[i];
    m->refs++;
    memset(pic, 0, sizeof *pic);
    pic->width = r.width; pic->height = r.height; pic->left = r.left; pic->top = r.top;
    pic->format = PMHW_FMT_NV12;
    pic->plane[0] = m->p + r.offset0;
    pic->plane[1] = m->p + r.offset1;
    pic->stride[0] = r.stride0; pic->stride[1] = r.stride1;
    pic->pts = r.pts;
    pic->corrupt = r.corrupt;
    pic->dmabuf_fd = m->fd;
    pic->drm_fourcc = r.fourcc;
    pic->drm_modifier = r.modifier;
    pic->offset[0] = r.offset0; pic->offset[1] = r.offset1;
    pic->priv = (void *)(intptr_t)(r.token * 64 + i + 1);
    return 1;
}

static void cc_put(void *ctx, PmhwPicture *pic)
{
    CC *c = ctx;
    intptr_t v = (intptr_t)pic->priv;
    pic->priv = NULL;
    if (v <= 0) return;
    int i = (int)(v % 64) - 1;
    if (i >= 0 && i < MAPS && c->map[i].refs > 0) c->map[i].refs--;
    CdReq q = { .op = CD_PUT, .token = (uint32_t)(v / 64) };
    call(c, &q, NULL, 0, NULL, NULL);
}

static void cc_flush(void *ctx)
{
    CC *c = ctx;
    CdReq q = { .op = CD_FLUSH };
    CdRep r;
    call(c, &q, NULL, 1, &r, NULL);
}

static void cc_close(void *ctx)
{
    CC *c = ctx;
    if (!c) return;
    stop(c);                                           /* the helper exits on EOF, freeing the engine */
    for (int i = 0; i < MAPS; i++) unmap(&c->map[i]);
    free(c);
}

const Backend pmhw_cedar_client_backend = {
    "cedar", cc_query, cc_open, cc_send, cc_get, cc_put, cc_flush, cc_close,
};
