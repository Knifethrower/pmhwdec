// SPDX-License-Identifier: LGPL-2.1-or-later
/* Known-answer self-test: before a backend is used for a codec, it decodes a tiny built-in clip (selftest_data.h:
 * H.264 High with B-frames, HEVC Main, HEVC Main 10; 160x96, 6 frames) and every frame's 8x8 block means must be
 * within ST_TOLERANCE of FFmpeg's. A chip nobody tested (or a driver that misbehaves) then either decodes right or
 * is not used: the caller gets "no hardware" and decodes in software, never a wrong picture. Block means, not exact
 * checksums: some decoders differ from FFmpeg by +-1 now and then (RP5 Iris H.264), which moves no mean by more
 * than 1, while wrong formats, colors, references or frame order move them by tens.
 * Each test runs in its own thread with a time limit: a decoder that hangs counts as failed for every codec of that
 * backend. Results are kept in /tmp/pmhwdec-selftest.<uid> for the current boot and library build.
 * PMHWDEC_SELFTEST=0 skips the tests, =1 ignores the saved results. PMHWDEC_SELFTEST_INJECT=<n> adds n to every
 * measured block mean (proves a wrong decoder is caught; use with PMHWDEC_SELFTEST=1). */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "internal.h"
#include "selftest_data.h"

#define ST_TOLERANCE 2           /* max difference of a block mean (8-bit units) */
#define ST_TIME_LIMIT 5          /* seconds per clip */
#define ST_BUILD __DATE__ " " __TIME__
#define ST_HUNG_CODEC 99         /* table entry: this backend hung, none of its codecs is used */

enum { ST_UNTESTED, ST_RUNNING, ST_PASS, ST_FAIL };

typedef struct Entry { char name[24]; int codec, depth, state; } Entry;   /* depth: 8 or 10 */

static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static Entry st_tab[48];
static int st_n, st_loaded;

static int env_mode(void)       /* 0 off, 1 on, 2 on without the saved results */
{
    const char *e = getenv("PMHWDEC_SELFTEST");
    if (!e || !*e) return 1;
    return !strcmp(e, "0") ? 0 : !strcmp(e, "1") ? 2 : 1;
}

static Entry *find(const char *name, int codec, int depth, int add)
{
    for (int i = 0; i < st_n; i++)
        if (!strcmp(st_tab[i].name, name) && st_tab[i].codec == codec && st_tab[i].depth == depth) return &st_tab[i];
    if (!add || st_n == (int)(sizeof st_tab / sizeof st_tab[0])) return NULL;
    Entry *e = &st_tab[st_n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->codec = codec;
    e->depth = depth;
    e->state = ST_UNTESTED;
    return e;
}

static int state(const char *name, int codec, int depth)
{
    Entry *e = find(name, codec, depth, 0);
    return e ? e->state : ST_UNTESTED;
}

static int hung(const char *name) { return state(name, ST_HUNG_CODEC, 0) == ST_FAIL; }

static const struct StClip *clip_for(PmhwCodec codec, int depth)
{
    for (size_t i = 0; i < sizeof st_clips / sizeof st_clips[0]; i++)
        if (st_clips[i].codec == (int)codec && st_clips[i].depth == depth) return &st_clips[i];
    return NULL;
}

/* ---- saved results ---- */
static void cache_path(char *p, size_t n) { snprintf(p, n, "/tmp/pmhwdec-selftest.%d", (int)getuid()); }

static void read_line(FILE *f, char *b, int n)
{
    if (!fgets(b, n, f)) b[0] = 0;
    b[strcspn(b, "\n")] = 0;
}

static void boot_id(char *b, int n)
{
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    b[0] = 0;
    if (f) { read_line(f, b, n); fclose(f); }
}

static void cache_load(void)
{
    char path[64], id[64], line[160], want[96];
    cache_path(path, sizeof path);
    boot_id(id, sizeof id);
    FILE *f = fopen(path, "r");
    if (!f) return;
    read_line(f, line, sizeof line);
    snprintf(want, sizeof want, "boot %s", id);
    int ok = id[0] && !strcmp(line, want);
    read_line(f, line, sizeof line);
    snprintf(want, sizeof want, "build %s", ST_BUILD);
    ok = ok && !strcmp(line, want);
    while (ok && fgets(line, sizeof line, f)) {
        char name[24]; int c, d, r;
        if (sscanf(line, "%23s %d %d %d", name, &c, &d, &r) != 4 || (r != ST_PASS && r != ST_FAIL)) continue;
        Entry *e = find(name, c, d, 1);
        if (e) e->state = r;
    }
    fclose(f);
}

static void cache_save(void)
{
    char path[64], tmp[80], id[64];
    cache_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
    boot_id(id, sizeof id);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "boot %s\nbuild %s\n", id, ST_BUILD);
    for (int i = 0; i < st_n; i++)
        if (st_tab[i].state == ST_PASS || st_tab[i].state == ST_FAIL)
            fprintf(f, "%s %d %d %d\n", st_tab[i].name, st_tab[i].codec, st_tab[i].depth, st_tab[i].state);
    fclose(f);
    if (rename(tmp, path)) unlink(tmp);
}

/* ---- one clip through a backend ---- */
typedef struct Job {
    const Backend *be;
    const struct StClip *clip;
    int result;                  /* ST_PASS / ST_FAIL */
    int done, abandoned;
    char why[160];
    pthread_mutex_t m;
    pthread_cond_t cv;
} Job;

static int check_picture(Job *j, const PmhwPicture *p, int n)
{
    const struct StClip *c = j->clip;
    if (n >= ST_FRAMES) { snprintf(j->why, sizeof j->why, "more than %d pictures", ST_FRAMES); return -1; }
    if (p->width != ST_W || p->height != ST_H) {
        snprintf(j->why, sizeof j->why, "picture %d is %dx%d, not %dx%d", n, p->width, p->height, ST_W, ST_H);
        return -1;
    }
    if (p->corrupt) { snprintf(j->why, sizeof j->why, "picture %d marked corrupt", n); return -1; }
    const uint8_t *ref = c->means + (size_t)n * ST_MEANS;
    const uint8_t *y = p->plane[0] + (size_t)p->top * p->stride[0] + p->left;
    const uint8_t *uv = p->plane[1] + (size_t)(p->top / 2) * p->stride[1] + (p->left & ~1);
    int k = 0, worst = 0, at = 0;
    int inject = getenv("PMHWDEC_SELFTEST_INJECT") ? atoi(getenv("PMHWDEC_SELFTEST_INJECT")) : 0;   /* test hook */
    for (int pl = 0; pl < 3; pl++) {
        int pw = pl ? ST_W / 2 : ST_W, ph = pl ? ST_H / 2 : ST_H;
        for (int by = 0; by < ph; by += 8)
            for (int bx = 0; bx < pw; bx += 8, k++) {
                unsigned s = 0, cnt = 0;
                for (int yy = by; yy < by + 8 && yy < ph; yy++)
                    for (int xx = bx; xx < bx + 8 && xx < pw; xx++, cnt++)
                        s += pl == 0 ? y[(size_t)yy * p->stride[0] + xx] : uv[(size_t)yy * p->stride[1] + 2 * xx + (pl - 1)];
                int d = abs((int)((s + cnt / 2) / cnt) + inject - ref[k]);
                if (d > worst) { worst = d; at = k; }
            }
    }
    if (worst > ST_TOLERANCE) {
        snprintf(j->why, sizeof j->why, "picture %d differs from FFmpeg (block %d off by %d)", n, at, worst);
        return -1;
    }
    return 0;
}

static void *run_job(void *arg)
{
    Job *j = arg;
    const struct StClip *c = j->clip;
    int n = 0, bad = 0, sent = 0, spins = 0;
    struct timespec ms = { 0, 1000000 };
    void *ctx = j->be->open(c->codec, ST_W, ST_H);
    if (!ctx) { snprintf(j->why, sizeof j->why, "open failed"); bad = 1; }
    while (ctx && !bad) {
        PmhwPicture p;
        if (j->be->get(ctx, &p) == 1) {
            if (check_picture(j, &p, n)) bad = 1;
            n++;
            j->be->put(ctx, &p);
            spins = 0;
            continue;
        }
        if (sent > c->naus) {    /* end of stream sent: wait for the last pictures */
            if (n >= ST_FRAMES || ++spins > 2000) break;
            nanosleep(&ms, NULL);
            continue;
        }
        int r = sent < c->naus ? j->be->send(ctx, c->data + c->au[sent], (int)(c->au[sent + 1] - c->au[sent]), sent)
                               : j->be->send(ctx, NULL, 0, 0);
        if (r == PMHW_AGAIN) {
            if (++spins > 2000) { snprintf(j->why, sizeof j->why, "decoder never takes access unit %d", sent); bad = 1; }
            nanosleep(&ms, NULL);
            continue;
        }
        spins = 0;
        if (r == PMHW_UNSUPPORTED) { snprintf(j->why, sizeof j->why, "stream refused"); bad = 1; }
        else if (r == PMHW_ERROR) { snprintf(j->why, sizeof j->why, "access unit %d failed", sent); bad = 1; }
        sent++;
    }
    if (!bad && n != ST_FRAMES) { snprintf(j->why, sizeof j->why, "%d of %d pictures came out", n, ST_FRAMES); bad = 1; }
    if (ctx) j->be->close(ctx);
    pthread_mutex_lock(&j->m);
    j->result = bad ? ST_FAIL : ST_PASS;
    j->done = 1;
    int abandoned = j->abandoned;
    pthread_cond_signal(&j->cv);
    pthread_mutex_unlock(&j->m);
    if (abandoned) { pthread_mutex_destroy(&j->m); pthread_cond_destroy(&j->cv); free(j); }
    return NULL;
}

/* ST_PASS / ST_FAIL; *timed_out = the decoder did not finish in time */
static int run_clip(const Backend *be, const struct StClip *c, int *timed_out)
{
    Job *j = calloc(1, sizeof *j);
    if (!j) return ST_FAIL;
    j->be = be;
    j->clip = c;
    pthread_mutex_init(&j->m, NULL);
    pthread_cond_init(&j->cv, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, run_job, j)) { free(j); return ST_FAIL; }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ST_TIME_LIMIT;
    pthread_mutex_lock(&j->m);
    while (!j->done)
        if (pthread_cond_timedwait(&j->cv, &j->m, &until) == ETIMEDOUT) break;
    int done = j->done, result = j->result;
    char why[160];
    memcpy(why, j->why, sizeof why);
    if (!done) j->abandoned = 1;         /* the thread frees the job if it ever comes back */
    pthread_mutex_unlock(&j->m);
    if (done) {
        pthread_join(t, NULL);
        pthread_mutex_destroy(&j->m);
        pthread_cond_destroy(&j->cv);
        free(j);
    } else {
        pthread_detach(t);
        snprintf(why, sizeof why, "no result after %d s (the decoder hangs)", ST_TIME_LIMIT);
        result = ST_FAIL;
        *timed_out = 1;
    }
    if (result == ST_PASS) LOG("self-test %s %s: OK", be->name, c->name);
    else ERR("self-test %s %s failed: %s; this decoder is not used for it (software decoding instead)", be->name, c->name, why);
    return result;
}

/* the bit depth this backend may decode for this codec: 0 = none (failed or hung), else up to max_depth */
int pmhw_selftest(const Backend *be, PmhwCodec codec, int max_depth)
{
    int mode = env_mode();
    if (!mode) return max_depth;
    pthread_mutex_lock(&st_lock);
    if (!st_loaded) { st_loaded = 1; if (mode == 1) cache_load(); }
    int result = max_depth, changed = 0;
    for (int depth = 8; depth <= 10 && depth <= max_depth; depth += 2) {
        const struct StClip *c = clip_for(codec, depth);
        if (!c) break;                   /* no clip for it (VP9): trust the caps */
        if (hung(be->name)) { result = 0; break; }
        Entry *e = find(be->name, codec, depth, 1);
        if (!e || e->state == ST_RUNNING) break;   /* table full / the test itself is decoding: allow */
        if (e->state == ST_UNTESTED) {
            e->state = ST_RUNNING;
            int timed_out = 0;
            pthread_mutex_unlock(&st_lock);
            int r = run_clip(be, c, &timed_out);
            pthread_mutex_lock(&st_lock);
            e = find(be->name, codec, depth, 1);
            if (e) e->state = r;
            if (timed_out) { Entry *h = find(be->name, ST_HUNG_CODEC, 0, 1); if (h) h->state = ST_FAIL; }
            changed = 1;
        }
        if (hung(be->name)) { result = 0; break; }
        if (state(be->name, codec, depth) == ST_FAIL) { result = depth == 8 ? 0 : 8; break; }
    }
    if (changed) cache_save();
    pthread_mutex_unlock(&st_lock);
    return result;
}

/* for backends that learn a stream's depth only from its headers: may they decode it? (yes while untested) */
int pmhw_selftest_depth_allowed(const char *backend, PmhwCodec codec, int depth)
{
    if (depth <= 8 || !env_mode()) return 1;
    pthread_mutex_lock(&st_lock);
    int ok = !hung(backend) && state(backend, codec, 10) != ST_FAIL;
    pthread_mutex_unlock(&st_lock);
    return ok;
}
